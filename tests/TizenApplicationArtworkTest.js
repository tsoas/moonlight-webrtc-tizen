"use strict";

const assert = require("assert");

global.window = global;
require("../tizen/application-artwork.js");

const requests = [];
const updates = [];
const loader = global.ApplicationArtworkLoader.create({
  requestArtwork: function (appId) { requests.push(appId); },
  onArtwork: function (appId, dataUrl) { updates.push({ appId: appId, dataUrl: dataUrl }); },
});

loader.setApplications([{ id: "7", title: "Desktop" }, { id: "8", title: "Steam" }]);
assert.deepStrictEqual(requests, ["7"], "cards must render before one lazy artwork request starts");
assert.ok(loader.handleResponse({
  appId: "7", available: true, mimeType: "image/jpeg", data: "/9j/",
}), "a matching Sunshine artwork response must be accepted");
assert.ok(updates[0].dataUrl.startsWith("data:image/jpeg;base64,"),
  "artwork must become display media only");
assert.deepStrictEqual(requests, ["7", "8"], "artwork requests must advance progressively");
assert.ok(loader.handleResponse({ appId: "8", available: false }),
  "missing Sunshine artwork must be handled as a fallback");
assert.strictEqual(updates[1].dataUrl, null, "missing artwork must retain the fallback card");
loader.setApplications([{ id: "7", title: "Desktop" }, { id: "8", title: "Steam" }]);
assert.deepStrictEqual(requests, ["7", "8"], "cached success and absence must not redownload artwork");
assert.strictEqual(global.ApplicationArtworkLoader.testing.validArtwork({
  appId: "7", available: true, mimeType: "image/svg+xml", data: "PHN2Zz4=",
}), false, "active image content must reject SVG");

const exactIds = [];
const exactLoader = global.ApplicationArtworkLoader.create({
  requestArtwork: function (appId) { exactIds.push(appId); },
  onArtwork: function () {},
});
exactLoader.setApplications([
  { id: "4294967295", title: "First" },
  { id: "4294967296", title: "Second" },
]);
assert.deepStrictEqual(exactIds, ["4294967295"],
  "the Tizen artwork request must preserve the first opaque Sunshine ID");
assert.ok(exactLoader.handleResponse({
  appId: "4294967295", available: false,
}), "the first opaque Sunshine ID should accept its own response");
assert.deepStrictEqual(exactIds, ["4294967295", "4294967296"],
  "distinct opaque Sunshine IDs must not collide in lazy artwork loading");

console.log("Tizen application artwork tests passed");
