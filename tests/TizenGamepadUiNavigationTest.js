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
  menu: function () { actions.push("menu"); },
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
actions.length = 0;
navigator.handleGamepad(gamepad([], []), 90);
navigator.handleGamepad(gamepad(buttons([9])), 100);
assert.deepStrictEqual(actions, ["menu"], "Start/Menu opens the contextual UI action");
route = "gameplay";
actions.length = 0;
navigator.handleGamepad(gamepad(buttons([1, 9, 12])), 120);
assert.deepStrictEqual(actions, [], "gamepad buttons are not consumed by UI during gameplay");

route = "ui";
actions.length = 0;
navigator.handleGamepad(gamepad(buttons([15])), 200, "gameplay");
assert.deepStrictEqual(actions, [],
  "an explicitly passed gameplay route must suppress UI actions");

const scheduledDelays = [];
const realSetTimeout = global.setTimeout;
global.setTimeout = function (_callback, delay) { scheduledDelays.push(delay); return 0; };
navigator.schedule("gameplay");
navigator.schedule("stream-menu");
navigator.schedule(null);
global.setTimeout = realSetTimeout;
navigator.timer = null;
assert.ok(scheduledDelays[0] > 20 && scheduledDelays[0] <= 50,
  "gameplay backs this poller off, but not so far that the next menu press feels stuck");
assert.ok(scheduledDelays[1] < 20 && scheduledDelays[2] < 20,
  "menus and the home screen keep the responsive cadence");

const failing = global.GamepadUiNavigation.create({
  route: function () { throw new Error("route lookup blew up"); },
  navigate: function () {},
  activate: function () {},
  back: function () {},
});
const failingScheduled = [];
global.setTimeout = function (_callback, delay) { failingScheduled.push(delay); return 0; };
failing.poll();
global.setTimeout = realSetTimeout;
failing.timer = null;
assert.strictEqual(failingScheduled.length, 1,
  "a throwing UI action must still leave the poller scheduled");

console.log("Tizen gamepad UI navigation tests passed");
