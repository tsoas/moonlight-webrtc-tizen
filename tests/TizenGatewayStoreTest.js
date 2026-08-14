"use strict";

const assert = require("assert");

global.window = global;
require("../tizen/gateway-store.js");
require("../tizen/gateway-ipv4.js");

function storage() {
  const values = new Map();
  return {
    getItem: function (key) { return values.has(key) ? values.get(key) : null; },
    setItem: function (key, value) { values.set(key, value); },
    removeItem: function (key) { values.delete(key); },
  };
}

const persistentStorage = storage();
const store = global.GatewayStore.create(persistentStorage);
assert.deepStrictEqual(store.load(), [], "a fresh install must contain no Gateway");

const primary = store.upsert({ host: "192.168.0.69", name: "Gaming PC" });
assert.deepStrictEqual(primary, {
  id: "192.168.0.69:8000", host: "192.168.0.69", port: 8000, name: "Gaming PC",
}, "a valid Gateway must persist using the existing port");
assert.strictEqual(global.GatewayStore.create(persistentStorage).load().length, 1,
  "saved Gateways must survive a reload");
store.upsert({ host: "192.168.0.69", name: "Renamed PC" });
assert.strictEqual(store.list().length, 1, "an exact duplicate must not create a second Gateway");
assert.strictEqual(store.list()[0].name, "Renamed PC", "an exact duplicate may refresh its display name");
store.upsert({ host: "192.168.1.20", name: "Office PC" });
assert.strictEqual(store.list().length, 2, "multiple Gateways must persist");
assert.strictEqual(store.replace(primary.id, { host: "192.168.0.70", name: "Gaming PC" }).host,
  "192.168.0.70", "editing must replace the saved host only after validation calls replace");
assert.strictEqual(store.remove("192.168.1.20:8000"), true, "removing a Gateway must update storage");
assert.strictEqual(store.list().length, 1, "removed Gateways must not remain in storage");

persistentStorage.setItem(global.GatewayStore.STORAGE_KEY, JSON.stringify([
  { host: "bad-host", name: "Invalid" },
  { host: "10.0.0.4", port: 8000, name: "Valid" },
  { host: "10.0.0.4", port: 8000, name: "Duplicate" },
]));
assert.deepStrictEqual(global.GatewayStore.create(persistentStorage).load(), [{
  id: "10.0.0.4:8000", host: "10.0.0.4", port: 8000, name: "Valid",
}], "malformed and duplicate stored Gateways must be ignored safely");

assert.deepStrictEqual(global.GatewayIpv4.parse(""), [192, 168, 0, 0],
  "the IPv4 editor must default to 192.168.0.0");
assert.deepStrictEqual(global.GatewayIpv4.parse("192.168.0.69"), [192, 168, 0, 69],
  "editing an existing Gateway must initialize its four octets");
assert.strictEqual(global.GatewayIpv4.adjust(255, 1), 0, "255 plus UP must wrap to 0");
assert.strictEqual(global.GatewayIpv4.adjust(0, -1), 255, "0 plus DOWN must wrap to 255");
assert.strictEqual(global.GatewayIpv4.format([192, 168, 0, 69]), "192.168.0.69",
  "four octets must assemble into the persisted IPv4 host");

console.log("Tizen Gateway store tests passed");
