"use strict";

const assert = require("assert");

// Brightness of every pixel the fake video readback returns. Zero stands for the case this
// module is mostly built to survive: a TV that composites video on a hardware overlay and
// hands the application a black rectangle instead of the frame.
let pixelValue = 255;

function fakeContext() {
  const context = {
    draws: 0,
    blends: 0,
    globalAlpha: 1,
    drawImage: function () {
      context.draws += 1;
      if (context.globalAlpha < 1) { context.blends += 1; }
    },
    getImageData: function (_x, _y, width, height) {
      const data = new Uint8ClampedArray(width * height * 4);
      data.fill(pixelValue);
      return { data: data };
    },
  };
  return context;
}

function fakeCanvas() {
  const canvas = { width: 0, height: 0, hidden: true, context: fakeContext() };
  canvas.getContext = function () { return canvas.context; };
  return canvas;
}

global.window = global;
global.document = { createElement: function () { return fakeCanvas(); } };
require("../tizen/frame-interpolation.js");

function harness() {
  const video = { videoWidth: 1920, videoHeight: 1080, currentTime: 0, style: {} };
  const canvas = fakeCanvas();
  const logs = [];
  const state = { pending: null };
  const interpolation = global.FrameInterpolation.create({
    video: video,
    canvas: canvas,
    log: function (message) { logs.push(message); },
    requestFrame: function (callback) { state.pending = callback; return 1; },
    cancelFrame: function () { state.pending = null; },
  });
  return {
    video: video,
    canvas: canvas,
    logs: logs,
    interpolation: interpolation,
    step: function (timestamp) {
      const callback = state.pending;
      state.pending = null;
      if (callback) { callback(timestamp); }
    },
    pending: function () { return state.pending !== null; },
  };
}

// An overlay-composited TV must end up exactly where it started: video visible, canvas
// hidden, feature off. Anything else covers the game with a black rectangle.
pixelValue = 0;
const blocked = harness();
blocked.interpolation.setEnabled(true);
for (let attempt = 0; attempt < 60 && blocked.pending(); attempt += 1) {
  blocked.step(1000 + attempt * 16);
}
assert.strictEqual(blocked.interpolation.status().reason, "unsupported",
  "a TV that returns black frames must be detected, not drawn over");
assert.strictEqual(blocked.canvas.hidden, true, "the canvas stays hidden when unsupported");
assert.strictEqual(blocked.video.style.visibility, "",
  "the video element must be left visible when interpolation gives up");
assert.strictEqual(blocked.pending(), false, "giving up also stops the presentation loop");

// A single black frame proves nothing - streams start on black - so the probe must not
// abandon the feature on the first attempt.
pixelValue = 0;
const patient = harness();
patient.interpolation.setEnabled(true);
patient.step(1000);
assert.strictEqual(patient.interpolation.status().reason, "probing",
  "one black readback is not enough to declare the TV unsupported");
patient.interpolation.setEnabled(false);

pixelValue = 255;
const running = harness();
running.interpolation.setEnabled(true);

// A 30 fps source presented on a 60 Hz loop: every second output frame falls halfway
// between two decoded frames, which is the only situation where this can add anything.
running.video.currentTime = 0;
running.step(1000);
running.step(1016.6);
running.video.currentTime = 0.0333;
running.step(1033.3);
running.canvas.context.draws = 0;
running.canvas.context.blends = 0;
running.step(1050);
assert.strictEqual(running.canvas.context.draws, 2,
  "a frame halfway between two decoded frames is composited from both");
assert.strictEqual(running.canvas.context.blends, 1,
  "the second composite is the weighted one, so the result is an intermediate frame");

running.canvas.context.draws = 0;
running.video.currentTime = 0.0666;
running.step(1066.6);
assert.strictEqual(running.canvas.context.draws, 1,
  "a frame landing on a decoded frame is drawn straight, without a pointless blend");

assert.strictEqual(running.canvas.hidden, false, "the canvas is shown once it is drawing");
assert.strictEqual(running.video.style.visibility, "hidden",
  "the video is hidden underneath rather than stopped, so decoding continues");
assert.ok(running.interpolation.status().addedLatencyMs > 30,
  "the reported cost is a real source interval, not an estimate of zero");

running.interpolation.setEnabled(false);
assert.strictEqual(running.video.style.visibility, "",
  "turning it off restores the video element immediately");
assert.strictEqual(running.canvas.hidden, true, "turning it off hides the canvas");

// The lesson from the gamepad poller: a throwing presentation loop must not leave a hidden
// video behind a canvas that has stopped updating. That is a frozen picture with live audio.
pixelValue = 255;
const hostile = harness();
hostile.interpolation.setEnabled(true);
hostile.step(1000);
hostile.canvas.context.drawImage = function () { throw new Error("context lost"); };
hostile.video.currentTime = 0.0333;
assert.doesNotThrow(function () { hostile.step(1033.3); },
  "a failure inside the presentation loop must not escape it");
assert.strictEqual(hostile.video.style.visibility, "",
  "a failed interpolator hands the picture back to the video element");
assert.strictEqual(hostile.interpolation.status().reason, "failed",
  "the failure is reported rather than silently retried forever");
assert.strictEqual(hostile.pending(), false, "a failed interpolator stops rescheduling");

console.log("Tizen frame interpolation tests passed");
