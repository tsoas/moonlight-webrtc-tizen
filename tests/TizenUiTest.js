"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");

global.window = global;
require("../tizen/ui.js");
require("../tizen/preferences.js");
require("../tizen/wake-on-lan.js");
require("../tizen/gateway-store.js");
require("../tizen/gateway-ipv4.js");
require("../tizen/application-artwork.js");

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
assert.strictEqual(testing.readableGatewayAddress("ws://198.51.100.4:8000"), "198.51.100.4",
  "the gateway address should be derived from the real signaling URL");
assert.strictEqual(testing.readableGatewayAddress("not a URL"), "-",
  "invalid gateway URLs must not fabricate an address");

// Several assertions below span more than one line. Git checks these sources out with CRLF
// endings wherever core.autocrlf is on, which is the default on Windows, so reading them
// verbatim makes those assertions fail on the source they are meant to be checking.
function readSource(name) {
  return fs.readFileSync(path.join(__dirname, "../tizen/", name), "utf8").replace(/\r\n/g, "\n");
}

const html = readSource("index.html");
const config = readSource("config.xml");
const uiSource = readSource("ui.js");
const appSource = readSource("app.js");
const uiCss = readSource("ui.css");
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
assert.ok(html.includes('src="application-artwork.js"'),
  "the Applications screen must load the Gateway artwork transport");
assert.ok(html.includes('src="gateway-store.js"') && html.includes('src="gateway-ipv4.js"'),
  "manual Gateway setup requires persisted Gateway storage and the IPv4 editor");
assert.ok(html.includes('id="add-gateway-card"') === false && html.includes('id="gateway-grid"'),
  "the Gateway list must be rendered from persisted data rather than a hard-coded card");
assert.ok(html.includes('id="gateway-editor-dialog"') && html.includes('data-octet-index="3"'),
  "manual Gateway setup requires the four-octet IPv4 editor");
assert.ok(uiSource.includes("this.ensureGatewayFocus();"),
  "the first selectable gateway must receive initial focus when it becomes available");
assert.ok(uiSource.includes("TizenUi.prototype.focusSettingsCategory"),
  "settings categories must use their own vertical focus graph");
assert.ok(uiSource.includes("categories[index - 1].focus();"),
  "UP from Input must stay within the settings category list");
assert.ok(uiSource.includes("TizenUi.prototype.enterSettingsCategory"),
  "OK on a category must explicitly enter that category's settings panel");
assert.ok(uiSource.includes("ui.enterSettingsCategory(button.dataset.category)"),
  "remote and gamepad activation must use the shared Settings entry action");
assert.ok(!uiSource.includes('if (direction === "right") {\n      this.selectSettingsCategory(active.dataset.category);'),
  "RIGHT must not enter Settings options");
assert.ok(uiSource.includes("this.settingsPanelElements().indexOf(document.activeElement) >= 0"),
  "Back from Settings options must return to the selected category");
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
assert.ok(appSource.includes('type === "app-artwork"'),
  "Gateway artwork responses must update application cards asynchronously");
assert.ok(appSource.includes("setRunningApplication(message.runningAppId)"),
  "Gateway running application state must reach the cards");
assert.ok(appSource.includes("Stream disconnected. Running applications remain available."),
  "local disconnect must preserve Sunshine running state until the app list refreshes");
assert.ok(html.includes('id="launching-screen"')
  && html.includes('id="launching-app"')
  && html.includes('id="launching-status"'),
  "application launch must present a dedicated, readable progress screen");
assert.ok(appSource.includes("function launchingStep(state)")
  && appSource.includes('"starting-webrtc"')
  && appSource.includes('"connecting-sunshine"')
  && appSource.includes('"starting-moonlight"'),
  "launch progress must describe the existing Gateway session stages");
assert.ok(appSource.includes("function cancelLaunchingSession()")
  && appSource.includes('type: "stop-session"'),
  "Back during application launch must cancel through the existing session teardown");
assert.ok(uiCss.includes(".launching-screen") && uiCss.includes("var(--accent)"),
  "launch progress must retain the approved dark and violet visual language");
assert.ok(html.includes('id="running-app-menu"')
  && html.includes("Resume session") && html.includes("Stop session"),
  "running applications require a resume/stop context menu");
assert.ok(html.includes('id="switch-app-dialog"') && html.includes("Stop &amp; Launch"),
  "switching applications requires an explicit confirmation dialog");
assert.ok(appSource.includes("function openRunningAppMenu()")
  && appSource.includes("function resumeRunningApplication()")
  && appSource.includes("function stopRunningApplication()"),
  "running app operations must use shared UI actions");
assert.ok(appSource.includes('type: "stop-host-session"')
  && appSource.includes('applicationSessionRequest("switch-session", targetId)'),
  "host stop and switch must use explicit Gateway protocol operations");
assert.ok(appSource.includes("if (String(runningAppId) === String(applicationId))"),
  "selecting the current Sunshine application must resume rather than relaunch");
assert.ok(appSource.includes("hostOperationBusy"),
  "host operations must prevent duplicate UI actions while Sunshine reconciles state");
assert.ok(appSource.includes('showHome("applications");'),
  "normal session shutdown must return to the application list");
assert.ok(!appSource.includes("192.168.0.69"),
  "the Tizen runtime must not retain a developer Gateway fallback");
assert.ok(appSource.includes("GatewayStore.DEFAULT_PORT"),
  "manual Gateway setup must reuse the fixed existing Gateway port");
assert.ok(appSource.includes("function activateGateway(gatewayId)"),
  "Gateway selection must establish the selected active Gateway context");
assert.ok(appSource.includes("function probeSavedGateways()"),
  "saved Gateway state must be refreshed asynchronously");
assert.ok(appSource.includes("function startGatewayValidationTimeout(host, gatewayId)")
  && appSource.includes("within 10 seconds"),
  "manual Gateway validation must stop after a bounded ten second timeout");
assert.ok(appSource.includes('setGatewayRuntimeState(activeGateway.id, "Online")'),
  "a newly saved Gateway must receive its persistent Online runtime state");
assert.ok(appSource.includes("function openGatewayContextMenu()")
  && appSource.includes("function openGatewayEditor(mode, gatewayId)"),
  "saved Gateways require the edit/remove context actions");
assert.ok(uiSource.includes('const preferred = container.querySelector("[data-default-focus=\'true\']");'),
  "Gateway focus restoration must use the rendered default card rather than the obsolete singleton card");
assert.ok(uiSource.includes('this.currentView === "applications" || this.currentView === "gateway"'),
  "Gateway cards and Add Gateway must share horizontal focus navigation");
assert.ok(uiSource.includes("application-running"),
  "running applications require a non-focusable visual indicator");
assert.ok(uiSource.indexOf('card.appendChild(art);') < uiSource.lastIndexOf('card.appendChild(name);'),
  "application artwork must render before its title in the same focusable item");
assert.ok(uiCss.includes("aspect-ratio:2 / 3"),
  "application artwork tiles must use stable portrait geometry");
assert.ok(uiCss.includes(".application-art img { width:100%; height:100%; object-fit:cover; }"),
  "application artwork must fill its tile without distortion");
assert.ok(uiCss.includes(".application-card:focus .application-art"),
  "application focus must outline the artwork tile rather than the title");
assert.ok(uiCss.includes("text-overflow:ellipsis"),
  "long application titles must remain constrained to the tile width");

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
  resolution: null, codec: null, hdr: false, bitrateKbps: null, frameInterpolation: false,
}, "empty storage must use the current defaults");
preferences.update("resolution", "3840x2160");
preferences.update("codec", "hevc");
preferences.update("hdr", true);
preferences.update("bitrateKbps", 50000);
preferences.update("frameInterpolation", true);
assert.deepStrictEqual(global.ClientPreferences.create(persistentStorage).load(), {
  resolution: "3840x2160", codec: "hevc", hdr: true, bitrateKbps: 50000,
  frameInterpolation: true,
}, "saved preferences must survive a simulated application reload");
assert.ok(!Object.prototype.hasOwnProperty.call(preferences.snapshot(), "fps"),
  "FPS must not become a persisted selectable preference");
persistentStorage.setItem(global.ClientPreferences.STORAGE_KEY, JSON.stringify({
  resolution: 4, codec: false, hdr: "true", bitrateKbps: -1, frameInterpolation: "yes",
}));
assert.deepStrictEqual(global.ClientPreferences.create(persistentStorage).load(), {
  resolution: null, codec: null, hdr: false, bitrateKbps: null, frameInterpolation: false,
}, "invalid persisted values must fall back safely");
assert.strictEqual(global.ClientPreferences.resolveSupported("av1", ["h264", "hevc"], "h264"), "h264",
  "unsupported persisted options must fall back to the current supported default");

// The resolution list is only ever populated from the Gateway's "capabilities" message, so
// without a cache the modes above 1080p are missing from Settings until the TV has connected.
assert.ok(appSource.includes("function restoreCachedCapabilities()"),
  "the advertised video modes must survive a restart, or 4K is absent until the app connects");
assert.ok(appSource.includes("cacheCapabilities(message);"),
  "every capabilities message must refresh the cache it is restored from");
assert.ok(
  appSource.indexOf('interpolationSelect.value = savedPreferences.frameInterpolation')
    < appSource.indexOf("restoreCachedCapabilities();\nupdateInterpolationStatus();"),
  "restoring the cache persists every preference, so the selects must hold their saved "
    + "values before it runs");
assert.ok(appSource.includes("frameInterpolation.setEnabled(false);"),
  "tearing a session down must stop the interpolation loop");
assert.ok(html.includes('id="interpolation-select"') && html.includes('id="interpolation-canvas"'),
  "frame interpolation needs both its setting and its presentation surface");
// Remote and gamepad navigation both walk the .focusable elements of the open panel, so a
// control that loses that class is on screen and unreachable - which is exactly how this
// setting was first reported.
assert.ok(/id="interpolation-select"[^>]*class="[^"]*\bfocusable\b/.test(html),
  "the interpolation setting must carry the class that puts it in the navigation chain");
const interpolationRow = html.slice(
  html.indexOf('for="interpolation-select"'),
  html.indexOf("</label>", html.indexOf('for="interpolation-select"'))
);
assert.ok(interpolationRow.includes('id="interpolation-status"'),
  "the status belongs inside the interpolation row: a second row repeating \"Off\" next to "
    + "it is a target the remote cannot land on");

// The widget is assembled from an explicit file list, so a script added to the markup but
// not to that list produces a package that loads nothing and shows a blank screen on the TV.
const packagingScript = fs.readFileSync(
  path.join(__dirname, "../packaging/tizen/build-package.ps1"), "utf8");
const referencedScripts = (html.match(/<script src="([^"]+)"/g) || []).map(function (tag) {
  return tag.replace(/^<script src="/, "").replace(/"$/, "");
});
assert.ok(referencedScripts.length > 0, "the client must load its modules from the markup");
referencedScripts.forEach(function (name) {
  assert.ok(fs.existsSync(path.join(__dirname, "../tizen/", name)),
    name + " is loaded by index.html but does not exist");
  assert.ok(packagingScript.includes("'" + name + "'"),
    name + " is loaded by index.html but is not packaged into the widget");
});

assert.ok(html.indexOf('src="wake-on-lan.js"') >= 0
  && html.indexOf('src="wake-on-lan.js"') < html.indexOf('src="gateway-store.js"'),
  "the Gateway store validates Wake-on-LAN addresses, so wake-on-lan.js must load first");
assert.ok(html.includes('id="gateway-wake-button"') && html.includes('type="button" hidden>Wake PC</button>'),
  "the Gateway menu must offer Wake PC, hidden until the Gateway's address is known");
assert.ok(packagingScript.includes("build-wake-on-lan.ps1") && packagingScript.includes("'wasm'"),
  "the widget must package the Wake-on-LAN WebAssembly module next to its loader");
assert.ok(appSource.includes("learnGatewayMacAddress(gateway.id, message);"),
  "Gateway probes must remember the address needed to wake each saved PC");
assert.ok(appSource.includes('gatewayRuntimeStates.get(gateway.id) === "Offline" && wakeOnLan.isSupported()'),
  "selecting an offline Gateway must wake it when the TV can");
assert.ok(appSource.includes("if (activeGateway && gatewayWakeIsActive(activeGateway.id)) {\n      setGatewayRuntimeState"),
  "failed attempts while a PC boots must retry quietly instead of reporting a disconnect");
assert.ok(appSource.includes("if (active === gatewayWakeButton) {"),
  "remote OK and gamepad A must activate Wake PC through the shared menu action");

console.log("Tizen UI tests passed");
