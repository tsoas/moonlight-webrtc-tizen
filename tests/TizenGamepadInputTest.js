"use strict";

const assert = require("assert");

global.window = global;
global.document = { hidden: false };
global.performance = { now: function () { return Date.now(); } };

let gamepads = [];
Object.defineProperty(global, "navigator", {
  configurable: true,
  value: {
    getGamepads: function () { return gamepads; },
  },
});

require("../tizen/gamepad-input.js");

function element() {
  return { textContent: "", hidden: true };
}

function channel() {
  return {
    readyState: "open",
    messages: [],
    send: function (message) { this.messages.push(JSON.parse(message)); },
  };
}

function createGamepad(index, id, actuator) {
  return {
    index: index,
    id: id,
    mapping: "standard",
    buttons: Array.from({ length: 17 }, function () {
      return { pressed: false, value: 0 };
    }),
    axes: [0, 0, 0, 0],
    vibrationActuator: actuator,
  };
}

const effectCalls = [];
let resetCalls = 0;
const actuator = {
  effects: ["dual-rumble"],
  playEffect: function (effect, parameters) {
    effectCalls.push({ effect: effect, parameters: parameters });
    return Promise.resolve("complete");
  },
  reset: function () {
    resetCalls += 1;
    return Promise.resolve("complete");
  },
};

const control = channel();
const input = channel();
const diagnostics = {
  state: element(),
  id: element(),
  mapping: element(),
  sendRate: element(),
  lastSequence: element(),
  controllers: element(),
};
const overlay = element();
const mouseOverlay = element();
const logs = [];
const mouseModeChanges = [];
const shortcutStops = [];

const manager = new global.GamepadInputManager({
  controlChannel: function () { return control; },
  gamepadChannel: function () { return input; },
  isStreaming: function () { return true; },
  log: function (message) { logs.push(message); },
  reportError: function (context, error) { throw new Error(context + ": " + error); },
  diagnostics: diagnostics,
  overlay: overlay,
  mouseOverlay: mouseOverlay,
  onStopShortcut: function (record) { shortcutStops.push(record.controllerId); },
  onMouseModeChanged: function (record, active) {
    mouseModeChanges.push({ controllerId: record.controllerId, active: active });
  },
});

gamepads = [
  null,
  createGamepad(4, "Xbox Wireless Controller Vendor: 045e Product: 02e0", actuator),
  null,
  createGamepad(9, "Generic Controller", null),
];
manager.resume();

assert.strictEqual(manager.connectedCount(), 2, "non-contiguous gamepads were not enumerated");
assert.strictEqual(control.messages.length, 2, "controllers were not announced");
assert.deepStrictEqual(control.messages.map(function (message) { return message.controllerId; }),
  [1, 2], "stable client controller identities were not assigned");
assert.strictEqual(control.messages[0].v, 2, "gamepad protocol was not bumped to v2");
assert.strictEqual(control.messages[0].dualRumble, true,
  "dual-rumble capability was not detected");
assert.strictEqual(control.messages[0].triggerRumble, false,
  "unsupported trigger-rumble was advertised");

manager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "controller-status",
  controllerId: 1,
  controllerSlot: 0,
}));
assert.ok(diagnostics.controllers.textContent.indexOf("slot 0") >= 0,
  "Moonlight slot assignment was not displayed");
manager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "input-diagnostics",
  controllerId: 1,
  controllerSlot: 0,
  messagesPerSecond: 12.5,
  lastSequence: 42,
  sequenceGaps: 3,
  staleStates: 1,
}));
assert.ok(diagnostics.controllers.textContent.indexOf("gaps 3") >= 0
    && diagnostics.controllers.textContent.indexOf("stale 1") >= 0,
  "per-controller Gateway sequence diagnostics were not displayed");

manager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "rumble",
  controllerId: 1,
  controllerSlot: 0,
  strongMagnitude: 0.75,
  weakMagnitude: 0.25,
  leftTrigger: 1,
  rightTrigger: 1,
}));
assert.strictEqual(effectCalls.length, 1, "rumble effect was not applied");
assert.strictEqual(effectCalls[0].effect, "dual-rumble", "wrong haptic effect selected");
assert.strictEqual(effectCalls[0].parameters.duration, 5000,
  "rumble did not mirror BrightCraft's five-second effect duration");
assert.strictEqual(effectCalls[0].parameters.strongMagnitude, 0.75,
  "strong rumble magnitude changed");
assert.strictEqual(effectCalls[0].parameters.weakMagnitude, 0.25,
  "weak rumble magnitude changed");

const freshEffectCalls = [];
gamepads[1] = createGamepad(4, "Xbox Wireless Controller Vendor: 045e Product: 02e0", {
  effects: ["dual-rumble"],
  playEffect: function (effect, parameters) {
    freshEffectCalls.push({ effect: effect, parameters: parameters });
    return Promise.resolve("complete");
  },
});
manager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "rumble",
  controllerId: 1,
  controllerSlot: 0,
  strongMagnitude: 0.5,
  weakMagnitude: 0.5,
}));
assert.strictEqual(freshEffectCalls.length, 1,
  "rumble did not obtain a fresh Gamepad object for the update");

manager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "rumble",
  controllerId: 1,
  controllerSlot: 0,
  strongMagnitude: 0,
  weakMagnitude: 0,
  leftTrigger: 0,
  rightTrigger: 0,
}));
assert.strictEqual(freshEffectCalls.length, 2,
  "zero rumble did not override the prior dual-rumble effect");
assert.strictEqual(freshEffectCalls[1].parameters.strongMagnitude, 0,
  "zero rumble retained a strong motor magnitude");
assert.strictEqual(freshEffectCalls[1].parameters.weakMagnitude, 0,
  "zero rumble retained a weak motor magnitude");

manager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "mouse-mode",
  controllerId: 1,
  controllerSlot: 0,
  active: true,
}));
assert.strictEqual(mouseOverlay.hidden, false, "mouse mode overlay was not shown");
assert.deepStrictEqual(mouseModeChanges, [{ controllerId: 1, active: true }],
  "mouse mode activation did not emit one state-transition notification");
manager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "mouse-mode",
  controllerId: 1,
  controllerSlot: 0,
  active: true,
}));
assert.strictEqual(mouseModeChanges.length, 1,
  "unchanged mouse mode emitted a duplicate notification");
manager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "mouse-mode",
  controllerId: 1,
  controllerSlot: 0,
  active: false,
}));
assert.strictEqual(mouseOverlay.hidden, true, "mouse mode overlay was not hidden");
assert.deepStrictEqual(mouseModeChanges, [
  { controllerId: 1, active: true },
  { controllerId: 1, active: false },
], "mouse mode disable did not emit one state-transition notification");

const shortcutRecord = manager.recordsById.get(1);
const LB = 1 << 4;
const RB = 1 << 5;
const BACK = 1 << 6;
const START = 1 << 7;
manager.observeStopShortcut(shortcutRecord, { buttons: LB });
manager.observeStopShortcut(shortcutRecord, { buttons: RB });
manager.observeStopShortcut(shortcutRecord, { buttons: BACK | START });
manager.observeStopShortcut(shortcutRecord, { buttons: LB | RB | BACK });
manager.observeStopShortcut(shortcutRecord, { buttons: LB | RB | START });
assert.strictEqual(shortcutStops.length, 0, "partial shortcut stopped the session");
manager.observeStopShortcut(shortcutRecord, { buttons: LB | RB | BACK | START });
assert.deepStrictEqual(shortcutStops, [1], "full shortcut did not stop once");
manager.observeStopShortcut(shortcutRecord, { buttons: LB | RB | BACK | START });
assert.deepStrictEqual(shortcutStops, [1], "held shortcut stopped repeatedly");
manager.observeStopShortcut(shortcutRecord, { buttons: LB | RB | BACK });
manager.observeStopShortcut(shortcutRecord, { buttons: LB | RB | BACK | START });
assert.deepStrictEqual(shortcutStops, [1, 1], "shortcut latch did not reset after release");

const inactiveStops = [];
gamepads = [createGamepad(0, "Inactive shortcut test controller", null)];
gamepads[0].buttons[4].pressed = true;
gamepads[0].buttons[5].pressed = true;
gamepads[0].buttons[8].pressed = true;
gamepads[0].buttons[9].pressed = true;
const inactiveManager = new global.GamepadInputManager({
  controlChannel: function () { return control; },
  gamepadChannel: function () { return input; },
  isStreaming: function () { return false; },
  log: function () {},
  reportError: function () {},
  diagnostics: diagnostics,
  overlay: overlay,
  mouseOverlay: mouseOverlay,
  onStopShortcut: function () { inactiveStops.push(true); },
});
inactiveManager.resume();
assert.strictEqual(inactiveStops.length, 0,
  "shortcut was observed while no streaming session was active");
inactiveManager.suspend(false);

manager.suspend(true);
assert.strictEqual(manager.connectedCount(), 0, "session cleanup retained controllers");

const triggerEffectCalls = [];
const triggerActuator = {
  effects: ["trigger-rumble"],
  playEffect: function (effect, parameters) {
    triggerEffectCalls.push({ effect: effect, parameters: parameters });
    return Promise.resolve("complete");
  },
  reset: function () { return Promise.resolve("complete"); },
};
gamepads = [createGamepad(0, "Trigger-only test controller", triggerActuator)];
const triggerManager = new global.GamepadInputManager({
  controlChannel: function () { return control; },
  gamepadChannel: function () { return input; },
  isStreaming: function () { return true; },
  log: function () {},
  reportError: function (context, error) { throw new Error(context + ": " + error); },
  diagnostics: diagnostics,
  overlay: overlay,
  mouseOverlay: mouseOverlay,
});
triggerManager.resume();
triggerManager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "controller-status",
  controllerId: 1,
  controllerSlot: 0,
}));
triggerManager.handleControlMessage(JSON.stringify({
  v: 2,
  type: "rumble",
  controllerId: 1,
  controllerSlot: 0,
  strongMagnitude: 0.4,
  weakMagnitude: 0.2,
  leftTrigger: 0.8,
  rightTrigger: 0.6,
}));
assert.strictEqual(triggerEffectCalls.length, 1,
  "trigger-only haptic actuator ignored rumble state");
assert.strictEqual(triggerEffectCalls[0].effect, "dual-rumble",
  "BrightCraft-compatible rumble did not use dual-rumble");
triggerManager.suspend(false);

console.log("Tizen gamepad input tests passed");
