const assert = require("assert");
const fs = require("fs");
const path = require("path");

const index = fs.readFileSync(path.join(__dirname, "..", "tizen", "index.html"), "utf8");
const app = fs.readFileSync(path.join(__dirname, "..", "tizen", "app.js"), "utf8");
const gamepad = fs.readFileSync(path.join(__dirname, "..", "tizen", "gamepad-input.js"), "utf8");

assert(index.indexOf('src="gamepad-input.js"') < index.indexOf('src="app.js"'));
assert(app.includes("new window.GamepadInputManager"));
assert(app.includes("gamepadInputManager.handleControlMessage(event.data)"));
assert(app.includes("gamepadInputManager.beginSession()"));
assert((app.match(/function activeGamepad\(/g) || []).length === 1);
assert((app.match(/function configureControlDataChannel\(/g) || []).length === 1);
assert((app.match(/function configureGamepadDataChannel\(/g) || []).length === 1);
assert((app.match(/function suspendGamepadInput\(/g) || []).length === 1);
assert(app.includes("gamepadInputManager.suspend(Boolean(notifyGateway))"));
assert(app.includes("gamepadInputManager.pauseForUi()"));
assert(!app.includes("suspendGamepadInput(true);\n  continueButton.focus();"));
assert(!app.includes("if (bPressed && !state.bHeld)"));
assert(gamepad.includes("const recordById = Number.isFinite(controllerId)"));
assert(gamepad.includes("message.v !== PROTOCOL_VERSION && message.v !== 1"));

console.log("Tizen rumble integration tests passed.");
