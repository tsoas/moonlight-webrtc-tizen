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

function fakeFilesystem() {
  const files = new Map();
  return {
    files: files,
    pathExists: function (path) { return files.has(path); },
    deleteFile: function (path) { files.delete(path); },
    openFile: function (path, mode) {
      let contents = mode === "w" ? "" : files.get(path);
      if (contents === undefined) { throw new Error("NotFoundError"); }
      return {
        readString: function () { return contents; },
        writeString: function (text) { contents += text; },
        sync: function () { files.set(path, contents); },
        close: function () { files.set(path, contents); },
      };
    },
  };
}

const local = fakeLocalStorage();
const preference = fakePreference();
const messages = [];
const filesystem = fakeFilesystem();
const FILE_PATH = "wgt-private/gateways.json";
const storage = global.DurableStorage.create({
  storage: local,
  preference: preference,
  filesystem: filesystem,
  log: function (message) { messages.push(message); },
});

assert.strictEqual(storage.describe(), "tizen.preference + tizen.filesystem + localStorage",
  "every backend is used when the platform offers it, durable ones first");

storage.setItem("gateways", "[1]");
assert.strictEqual(local.values.get("gateways"), "[1]", "the value reaches localStorage");
assert.strictEqual(preference.values.get("gateways"), "[1]",
  "the value is mirrored where an app kill cannot lose it");
assert.strictEqual(JSON.parse(filesystem.files.get(FILE_PATH)).value, "[1]",
  "the value is also written to a file in the widget's private directory");

// The bug this module exists for: the TV discarded the buffered localStorage write when the
// app was killed, so on the next launch the key is simply absent.
local.values.delete("gateways");
assert.strictEqual(storage.getItem("gateways"), "[1]",
  "a value lost by localStorage is recovered from the mirror");
assert.strictEqual(local.values.get("gateways"), "[1]",
  "recovering also repairs localStorage for the rest of the session");
assert.ok(messages.some(function (message) { return message.indexOf("Recovered") === 0; }),
  "recovery is logged, because it is the evidence that the platform dropped a write");

// After a kill localStorage does not come back empty: it comes back with the value from an
// earlier session. A read must not let that stale value win over the durable copies.
storage.setItem("gateways", "[1,2]");
local.values.set("gateways", "[1]");
assert.strictEqual(storage.getItem("gateways"), "[1,2]",
  "a stale localStorage value does not hide the newest durable value");
assert.strictEqual(local.values.get("gateways"), "[1,2]", "the stale localStorage is repaired");

// tizen.preference may be missing or empty on some firmwares; the file still has the value.
preference.values.delete("gateways");
local.values.delete("gateways");
assert.strictEqual(storage.getItem("gateways"), "[1,2]", "the file alone recovers the value");
assert.strictEqual(preference.values.get("gateways"), "[1,2]", "recovery repairs the preference");

// A file cut short by a kill mid-write is treated as missing, not as a value.
filesystem.files.set(FILE_PATH, "{\"value\":\"[1,");
const fileOnly = global.DurableStorage.create({
  storage: null, preference: null, filesystem: filesystem,
});
assert.strictEqual(fileOnly.getItem("gateways"), null, "a truncated file yields no value");

local.failWrites = true;
assert.strictEqual(storage.setItem("gateways", "[2]"), true,
  "a refusal from localStorage still leaves the value stored in the mirror");
assert.strictEqual(preference.values.get("gateways"), "[2]",
  "the mirror holds the newest value even when localStorage rejects it");
local.failWrites = false;

storage.removeItem("gateways");
assert.strictEqual(local.values.has("gateways"), false, "removal clears localStorage");
assert.strictEqual(preference.values.has("gateways"), false, "removal clears the mirror");
assert.strictEqual(filesystem.files.has(FILE_PATH), false, "removal deletes the file");

const localOnly = global.DurableStorage.create({ storage: fakeLocalStorage() });
assert.strictEqual(localOnly.describe(), "localStorage",
  "a TV without durable storage APIs degrades to plain localStorage rather than failing");

const nowhere = global.DurableStorage.create({ storage: null, preference: null });
assert.strictEqual(nowhere.describe(), "none",
  "storage that exists nowhere is reported as such instead of pretending to persist");
assert.strictEqual(nowhere.getItem("gateways"), null, "reading from nothing yields nothing");
assert.strictEqual(nowhere.setItem("gateways", "[3]"), false,
  "a write that reached no backend must report failure, not silence");

console.log("Tizen durable storage tests passed");
