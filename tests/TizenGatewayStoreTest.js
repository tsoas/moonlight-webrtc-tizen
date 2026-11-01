"use strict";

const assert = require("assert");

global.window = global;
require("../tizen/wake-on-lan.js");
require("../tizen/gateway-auth.js");
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

const primary = store.upsert({ host: "192.0.2.69", name: "Gaming PC" });
assert.deepStrictEqual(primary, {
  id: "192.0.2.69:8000", host: "192.0.2.69", port: 8000, name: "Gaming PC",
}, "a valid Gateway must persist using the existing port");
assert.strictEqual(global.GatewayStore.create(persistentStorage).load().length, 1,
  "saved Gateways must survive a reload");
store.upsert({ host: "192.0.2.69", name: "Renamed PC" });
assert.strictEqual(store.list().length, 1, "an exact duplicate must not create a second Gateway");
assert.strictEqual(store.list()[0].name, "Renamed PC", "an exact duplicate may refresh its display name");
store.upsert({ host: "192.0.2.20", name: "Office PC" });
assert.strictEqual(store.list().length, 2, "multiple Gateways must persist");
assert.strictEqual(store.replace(primary.id, { host: "192.0.2.70", name: "Gaming PC" }).host,
  "192.0.2.70", "editing must replace the saved host only after validation calls replace");
assert.strictEqual(store.remove("192.0.2.20:8000"), true, "removing a Gateway must update storage");
assert.strictEqual(store.list().length, 1, "removed Gateways must not remain in storage");

persistentStorage.setItem(global.GatewayStore.STORAGE_KEY, JSON.stringify([
  { host: "bad-host", name: "Invalid" },
  { host: "198.51.100.4", port: 8000, name: "Valid" },
  { host: "198.51.100.4", port: 8000, name: "Duplicate" },
]));
assert.deepStrictEqual(global.GatewayStore.create(persistentStorage).load(), [{
  id: "198.51.100.4:8000", host: "198.51.100.4", port: 8000, name: "Valid",
}], "malformed and duplicate stored Gateways must be ignored safely");

const wakeable = global.GatewayStore.create(storage());
assert.strictEqual(wakeable.upsert({ host: "192.0.2.10", name: "PC", macAddress: "2c-f0-5d-7b-e6-d0" }).macAddress,
  "2C:F0:5D:7B:E6:D0", "a Gateway must keep the canonical form of its Wake-on-LAN address");
assert.strictEqual(wakeable.upsert({ host: "192.0.2.10", name: "PC", macAddress: "00:00:00:00:00:00" }).macAddress,
  undefined, "an address that cannot be woken must not be stored");
assert.ok(!Object.prototype.hasOwnProperty.call(wakeable.upsert({ host: "192.0.2.11", name: "Old PC" }), "macAddress"),
  "a Gateway that never reported its address must store none");

const paired = global.GatewayStore.create(storage());
const clientId = "00112233445566778899aabbccddeeff";
const clientSecret = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
const pairedGateway = paired.upsert({ host: "192.0.2.12", name: "PC", clientId: clientId, clientSecret: clientSecret });
assert.strictEqual(pairedGateway.clientId, clientId, "a paired Gateway must keep its client ID");
assert.strictEqual(pairedGateway.clientSecret, clientSecret, "a paired Gateway must keep its secret");
assert.strictEqual(paired.replace(pairedGateway.id, Object.assign({}, pairedGateway, { host: "192.0.2.13" })).clientId,
  clientId, "changing a Gateway's address must not lose its pairing");
const halfPaired = paired.upsert({ host: "192.0.2.14", name: "PC", clientId: clientId });
assert.ok(!Object.prototype.hasOwnProperty.call(halfPaired, "clientId"),
  "a client ID without its secret is useless and must not be stored");
assert.ok(!Object.prototype.hasOwnProperty.call(
  paired.upsert({ host: "192.0.2.15", name: "PC", clientId: "xyz", clientSecret: clientSecret }), "clientSecret"),
  "malformed credentials must not be stored");

assert.deepStrictEqual(global.GatewayIpv4.parse(""), [192, 168, 0, 0],
  "the IPv4 editor must default to 192.168.0.0");
assert.deepStrictEqual(global.GatewayIpv4.parse("192.0.2.69"), [192, 0, 2, 69],
  "editing an existing Gateway must initialize its four octets");
assert.strictEqual(global.GatewayIpv4.adjust(255, 1), 0, "255 plus UP must wrap to 0");
assert.strictEqual(global.GatewayIpv4.adjust(0, -1), 255, "0 plus DOWN must wrap to 255");
assert.strictEqual(global.GatewayIpv4.format([192, 0, 2, 69]), "192.0.2.69",
  "four octets must assemble into the persisted IPv4 host");

console.log("Tizen Gateway store tests passed");
