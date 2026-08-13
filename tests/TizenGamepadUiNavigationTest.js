"use strict";

const assert = require("assert");

global.window = global;
global.document = { hidden: false };
require("../tizen/gamepad-ui-navigation.js");

const testing = global.GamepadUiNavigation.testing;
function gamepad(buttons, axes) {
  return { index: 3, buttons: buttons || [], axes: axes || [] };
}
function buttons(indexes) {
  const values = [];
  indexes.forEach(function (index) { values[index] = { pressed: true, value: 1 }; });
  return values;
}

assert.strictEqual(testing.directionFor(gamepad(buttons([12]))), "up", "D-pad Up maps to UI Up");
assert.strictEqual(testing.directionFor(gamepad(buttons([13]))), "down", "D-pad Down maps to UI Down");
assert.strictEqual(testing.directionFor(gamepad(buttons([14]))), "left", "D-pad Left maps to UI Left");
assert.strictEqual(testing.directionFor(gamepad(buttons([15]))), "right", "D-pad Right maps to UI Right");
assert.strictEqual(testing.directionFor(gamepad([], [0.7, 0])), "right", "left stick crosses UI threshold");
assert.strictEqual(testing.directionFor(gamepad([], [0.2, 0.2])), null, "left stick deadzone prevents accidental navigation");

let route = "ui";
const actions = [];
const navigator = global.GamepadUiNavigation.create({
  route: function () { return route; },
  navigate: function (direction) { actions.push("navigate:" + direction); },
  activate: function () { actions.push("activate"); },
  back: function () { actions.push("back"); },
});
navigator.handleGamepad(gamepad(buttons([15])), 0);
assert.deepStrictEqual(actions, ["navigate:right"], "UI route receives gamepad direction");
actions.length = 0;
navigator.handleGamepad(gamepad([], []), 20);
navigator.handleGamepad(gamepad(buttons([0])), 40);
assert.deepStrictEqual(actions, ["activate"], "A activates the focused UI element");
actions.length = 0;
navigator.handleGamepad(gamepad([], []), 60);
navigator.handleGamepad(gamepad(buttons([1])), 80);
assert.deepStrictEqual(actions, ["back"], "B follows the shared Back action");
route = "gameplay";
actions.length = 0;
navigator.handleGamepad(gamepad(buttons([1, 12])), 100);
assert.deepStrictEqual(actions, [], "gamepad buttons are not consumed by UI during gameplay");

console.log("Tizen gamepad UI navigation tests passed");
