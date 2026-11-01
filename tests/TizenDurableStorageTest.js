"use strict";

const assert = require("assert");

global.window = global;
require("../tizen/durable-storage.js");

function fakeLocalStorage() {
  const values = new Map();
  return {
    values: values,
    failWrites: false,
    getItem: function (key) { return values.has(key) ? values.get(key) : null; },
    setItem: function (key, value) {
      if (this.failWrites) { throw new Error("QuotaExceededError"); }
      values.set(key, String(value));
    },
    removeItem: function (key) { values.delete(key); },
  };
}

function fakePreference() {
  const values = new Map();
  return {
    values: values,
    exists: function (key) { return values.has(key); },
    getValue: function (key) { return values.get(key); },
    setValue: function (key, value) { values.set(key, String(value)); },
    remove: function (key) { values.delete(key); },
  };
}

const local = fakeLocalStorage();
const preference = fakePreference();
const messages = [];
const storage = global.DurableStorage.create({
  storage: local,
  preference: preference,
  log: function (message) { messages.push(message); },
});

assert.strictEqual(storage.describe(), "localStorage + tizen.preference",
  "both backends are used when the platform offers them");

storage.setItem("gateways", "[1]");
assert.strictEqual(local.values.get("gateways"), "[1]", "the value reaches localStorage");
assert.strictEqual(preference.values.get("gateways"), "[1]",
  "the value is mirrored where an app kill cannot lose it");

// The bug this module exists for: the TV discarded the buffered localStorage write when the
// app was killed, so on the next launch the key is simply absent.
local.values.delete("gateways");
assert.strictEqual(storage.getItem("gateways"), "[1]",
  "a value lost by localStorage is recovered from the mirror");
assert.strictEqual(local.values.get("gateways"), "[1]",
  "recovering also repairs localStorage for the rest of the session");
assert.ok(messages.some(function (message) { return message.indexOf("Recovered") === 0; }),
  "recovery is logged, because it is the evidence that the platform dropped a write");

local.failWrites = true;
assert.strictEqual(storage.setItem("gateways", "[2]"), true,
  "a refusal from localStorage still leaves the value stored in the mirror");
assert.strictEqual(preference.values.get("gateways"), "[2]",
  "the mirror holds the newest value even when localStorage rejects it");
local.failWrites = false;

storage.removeItem("gateways");
assert.strictEqual(local.values.has("gateways"), false, "removal clears localStorage");
assert.strictEqual(preference.values.has("gateways"), false, "removal clears the mirror");

const localOnly = global.DurableStorage.create({ storage: fakeLocalStorage() });
assert.strictEqual(localOnly.describe(), "localStorage",
  "a TV without tizen.preference degrades to plain localStorage rather than failing");

const nowhere = global.DurableStorage.create({ storage: null, preference: null });
assert.strictEqual(nowhere.describe(), "none",
  "storage that exists nowhere is reported as such instead of pretending to persist");
assert.strictEqual(nowhere.getItem("gateways"), null, "reading from nothing yields nothing");
assert.strictEqual(nowhere.setItem("gateways", "[3]"), false,
  "a write that reached no backend must report failure, not silence");

console.log("Tizen durable storage tests passed");
