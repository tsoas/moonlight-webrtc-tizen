"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");

global.window = global;
require("../tizen/ui.js");
require("../tizen/preferences.js");

const testing = global.TizenUi.testing;

assert.strictEqual(testing.nextFocusableIndex(0, 0, 1), -1,
  "an empty view must have no focus target");
assert.strictEqual(testing.nextFocusableIndex(4, -1, 1), 0,
  "a view must select its first enabled target by default");
assert.strictEqual(testing.nextFocusableIndex(4, 0, -1), 3,
  "backward navigation must wrap deterministically");
assert.strictEqual(testing.nextFocusableIndex(4, 3, 1), 0,
  "forward navigation must wrap deterministically");
assert.strictEqual(testing.nextNavigationZone("content", "up", true), "top",
  "UP from content must enter the top application bar when it has an action");
assert.strictEqual(testing.nextNavigationZone("top", "down", true), "content",
  "DOWN from the top application bar must return to content");
assert.strictEqual(testing.nextNavigationZone("content", "up", false), "content",
  "navigation must not invent a missing top-bar focus target");
assert.strictEqual(testing.readableGatewayAddress("ws://192.168.0.69:8000"), "192.168.0.69",
  "the gateway address should be derived from the real signaling URL");
assert.strictEqual(testing.readableGatewayAddress("not a URL"), "-",
  "invalid gateway URLs must not fabricate an address");

const html = fs.readFileSync(path.join(__dirname, "../tizen/index.html"), "utf8");
const config = fs.readFileSync(path.join(__dirname, "../tizen/config.xml"), "utf8");
const uiSource = fs.readFileSync(path.join(__dirname, "../tizen/ui.js"), "utf8");
const appSource = fs.readFileSync(path.join(__dirname, "../tizen/app.js"), "utf8");
assert.ok(html.includes('id="settings-screen"'), "settings view is missing");
assert.ok(html.includes("Moonlight WebRTC Client"), "the Client title is missing from the UI");
assert.ok(!html.includes("Moonlight WebRTC Tizen"), "the old visible Tizen title remains");
assert.ok(config.includes("<name>Moonlight WebRTC Client</name>"),
  "the installed Tizen application title must identify the Client");
assert.ok(html.includes('data-category="video"') && html.includes('data-category="about"'),
  "all settings categories are required");
assert.ok(html.includes('60 FPS</strong><small>Fixed</small>'),
  "FPS must remain informational and fixed");
assert.ok(!html.includes("Optimize game settings"),
  "host game optimization must not be exposed in the TV UI");
assert.ok(html.includes('aria-disabled="true"'),
  "coming-soon rows must be disabled rather than focusable");
assert.ok(html.includes('id="stream-menu"') && html.includes('id="diagnostics-button"'),
  "the streaming Back menu and statistics control must remain available");
assert.ok(html.includes('id="toast-region"'), "notification region is missing");
assert.ok(uiSource.includes("this.ensureGatewayFocus();"),
  "the first selectable gateway must receive initial focus when it becomes available");
assert.ok(uiSource.includes("TizenUi.prototype.focusSettingsCategory"),
  "settings categories must use their own vertical focus graph");
assert.ok(uiSource.includes("categories[index - 1].focus();"),
  "UP from Input must stay within the settings category list");
assert.ok(uiSource.includes("this.selectSettingsCategory(active.dataset.category);"),
  "RIGHT from a category must open that category's own settings panel");
assert.ok(uiSource.includes("if (index === 0)"),
  "only the first settings category may enter the header with UP");
assert.ok(appSource.includes("isStreaming: isGameplayInputActive"),
  "the Moonlight gamepad path must only run during fullscreen gameplay");
assert.ok(appSource.includes("const gamepadUiNavigation = window.GamepadUiNavigation.create"),
  "gamepad UI navigation must use the shared application actions");
assert.ok(appSource.includes("function goBackFromUiInput()"),
  "remote and gamepad Back input must share one route-aware action");
assert.ok(html.includes('id="settings-selector-menu"'),
  "Settings selectors require a deterministic TV menu");
assert.ok(appSource.includes("function openSettingsSelector(select)"),
  "gamepad A must open the shared Settings selector menu");
assert.ok(appSource.includes("function closeSettingsSelector()"),
  "Settings selector Back behavior must be deterministic");
assert.ok(!appSource.includes("isEnter && document.activeElement.tagName !== \"SELECT\""),
  "remote OK and gamepad A must share Settings selector activation");
assert.ok(appSource.includes("function reportDataChannelError(context, error)"),
  "DataChannel teardown errors require dedicated lifecycle handling");
assert.ok(appSource.includes("if (sessionTeardownInProgress)"),
  "expected DataChannel teardown errors must not become home-screen errors");
assert.ok(appSource.includes("suspendGamepadInput(false);"),
  "normal peer teardown must not send controller lifecycle traffic over a closing channel");
assert.ok(appSource.includes("function handleGamepadStopShortcut()"),
  "the gamepad stop shortcut requires a shared application action");
assert.ok(appSource.includes("if (sessionState === \"streaming\") {\n    stopCurrentSession();"),
  "the gamepad stop shortcut must reuse the regular stop-session path");
assert.ok(appSource.includes("function handleMouseModeChanged(record, active)"),
  "mouse mode transitions must route through the existing toast system");

function storage() {
  const values = new Map();
  return {
    getItem: function (key) { return values.has(key) ? values.get(key) : null; },
    setItem: function (key, value) { values.set(key, value); },
    removeItem: function (key) { values.delete(key); },
  };
}

const persistentStorage = storage();
const preferences = global.ClientPreferences.create(persistentStorage);
assert.deepStrictEqual(preferences.load(), {
  resolution: null, codec: null, hdr: false, bitrateKbps: null,
}, "empty storage must use the current defaults");
preferences.update("resolution", "3840x2160");
preferences.update("codec", "hevc");
preferences.update("hdr", true);
preferences.update("bitrateKbps", 50000);
assert.deepStrictEqual(global.ClientPreferences.create(persistentStorage).load(), {
  resolution: "3840x2160", codec: "hevc", hdr: true, bitrateKbps: 50000,
}, "saved preferences must survive a simulated application reload");
assert.ok(!Object.prototype.hasOwnProperty.call(preferences.snapshot(), "fps"),
  "FPS must not become a persisted selectable preference");
persistentStorage.setItem(global.ClientPreferences.STORAGE_KEY, JSON.stringify({
  resolution: 4, codec: false, hdr: "true", bitrateKbps: -1,
}));
assert.deepStrictEqual(global.ClientPreferences.create(persistentStorage).load(), {
  resolution: null, codec: null, hdr: false, bitrateKbps: null,
}, "invalid persisted values must fall back safely");
assert.strictEqual(global.ClientPreferences.resolveSupported("av1", ["h264", "hevc"], "h264"), "h264",
  "unsupported persisted options must fall back to the current supported default");

console.log("Tizen UI tests passed");
