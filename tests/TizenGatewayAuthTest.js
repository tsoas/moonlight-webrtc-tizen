"use strict";

const assert = require("assert");
const crypto = require("crypto");

global.window = global;
require("../tizen/gateway-auth.js");

const auth = global.GatewayAuth;
const testing = auth.testing;

function nodeSha256(bytes) {
  return crypto.createHash("sha256").update(Buffer.from(bytes)).digest("hex");
}

// Every padding boundary, including the lengths where the length field spills into a
// second block.
for (const length of [0, 1, 55, 56, 63, 64, 65, 119, 120, 128, 1000]) {
  const bytes = crypto.randomBytes(length);
  assert.strictEqual(testing.bytesToHex(testing.sha256(new Uint8Array(bytes))), nodeSha256(bytes),
    "SHA-256 differs from Node for " + length + " bytes");
}

// RFC 4231 test case 2 and a key longer than one block (test case 6).
assert.strictEqual(
  testing.bytesToHex(testing.hmacSha256(testing.asciiBytes("Jefe"),
    testing.asciiBytes("what do ya want for nothing?"))),
  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
  "HMAC-SHA256 does not match RFC 4231");
const longKey = new Uint8Array(131).fill(0xaa);
assert.strictEqual(
  testing.bytesToHex(testing.hmacSha256(longKey,
    testing.asciiBytes("Test Using Larger Than Block-Size Key - Hash Key First"))),
  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
  "HMAC-SHA256 mishandles keys longer than one block");

// The same vector is asserted by tests/TvClientAuthTest.cpp, so both ends agree.
const secret = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
const nonce = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
assert.strictEqual(auth.authenticationProof(secret, nonce),
  "a8daa311fff071e004bcdf679a27d61367025038036ed95f4ff96691000053d5",
  "the TV proof does not match the Gateway");

for (let trial = 0; trial < 20; trial += 1) {
  const randomSecret = crypto.randomBytes(32).toString("hex");
  const randomNonce = crypto.randomBytes(32).toString("hex");
  const expected = crypto.createHmac("sha256", Buffer.from(randomSecret, "hex"))
    .update("moonlight-webrtc-tv-auth-v1:" + randomNonce).digest("hex");
  assert.strictEqual(auth.authenticationProof(randomSecret, randomNonce), expected,
    "the TV proof differs from Node's HMAC");
}

assert.throws(function () { auth.authenticationProof("abcd", nonce); },
  "a malformed secret must not produce a proof");
assert.throws(function () { auth.authenticationProof(secret, nonce.toUpperCase()); },
  "an upper-case nonce is not what the Gateway sends");
assert.ok(auth.isValidCredentials("00112233445566778899aabbccddeeff", secret),
  "well-formed credentials were rejected");
assert.ok(!auth.isValidCredentials("0011", secret), "a short client ID was accepted");
assert.ok(!auth.isValidCredentials("00112233445566778899aabbccddeeff", "zz"),
  "a malformed secret was accepted");

console.log("Tizen Gateway authentication tests passed");
