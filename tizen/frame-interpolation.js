(function (global) {
  "use strict";

  // Client-side frame generation, of the cheap kind: a temporal crossfade between the last
  // two decoded frames, presented on a canvas laid over the video.
  //
  // Three things are worth knowing before reading further, because they bound what this can
  // possibly achieve on a TV.
  //
  // 1. It needs the pixels. A Samsung TV decodes WebRTC video on a hardware overlay plane,
  //    and on many firmwares drawImage(video) from that plane yields black or is blocked
  //    outright. probe() checks for exactly that and refuses to run rather than covering the
  //    game with a black rectangle. That check is the honest answer to "is this feasible
  //    here at all", and it can only be answered on the TV itself.
  // 2. It costs a frame of latency. Interpolation is not causal: the midpoint between frames
  //    N and N+1 cannot be shown until N+1 has been decoded, so the real frame is held back
  //    while its synthetic predecessor is on screen. There is no version of this that does
  //    not pay that; the only question is whether the smoothness is worth it.
  // 3. It only has somewhere to put the extra frames when the stream runs below the panel's
  //    refresh rate. A 60 fps stream on a 60 Hz panel presented through requestAnimationFrame
  //    has no free slot, so the module reports "no headroom" instead of pretending: all it
  //    would be doing there is adding the latency from point 2 for nothing.
  //
  // Motion-compensated interpolation - the thing that actually deserves the name, and what
  // Lossless Scaling does - needs a per-pixel motion field. Estimating one at 1080p inside
  // a TV browser is far out of reach, so this blends instead. On real motion that reads as
  // a brief ghost rather than a sharp intermediate frame. It is the honest ceiling here.

  const PROBE_ATTEMPTS = 45;
  const MIN_SOURCE_INTERVAL_MS = 4;
  const MAX_SOURCE_INTERVAL_MS = 250;
  const INTERVAL_SMOOTHING = 0.1;
  const SAMPLE_WINDOW_MS = 1000;
  // A crossfade below this weight is invisible, and skipping the second composite is the
  // difference between one and two full-resolution draws on a GPU that has none to spare.
  const MIN_BLEND_ALPHA = 0.03;
  const HEADROOM_RATIO = 1.35;

  function surface(width, height) {
    const canvas = global.document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    return { canvas: canvas, context: canvas.getContext("2d"), time: 0 };
  }

  function FrameInterpolation(options) {
    this.options = options || {};
    this.video = this.options.video;
    this.canvas = this.options.canvas;
    this.log = typeof this.options.log === "function" ? this.options.log : function () {};
    this.requestFrame = this.options.requestFrame
      || function (callback) { return global.requestAnimationFrame(callback); };
    this.cancelFrame = this.options.cancelFrame
      || function (handle) { global.cancelAnimationFrame(handle); };
    this.context = null;
    this.previous = null;
    this.current = null;
    this.enabled = false;
    this.handle = null;
    this.reason = "off";
    this.probeAttempts = 0;
    this.probed = false;
    this.lastSourceTime = -1;
    this.sourceIntervalMs = 0;
    this.sourceFrames = 0;
    this.outputFrames = 0;
    this.sampleStart = 0;
    this.sourceFps = 0;
    this.outputFps = 0;
    this.displayIntervalMs = 0;
    this.lastStepTime = 0;
  }

  FrameInterpolation.prototype.setEnabled = function (enabled) {
    const wanted = Boolean(enabled);
    if (wanted === this.enabled) {
      return;
    }
    this.enabled = wanted;
    if (wanted) {
      this.start();
    } else {
      this.stop("off");
    }
  };

  FrameInterpolation.prototype.start = function () {
    if (this.handle !== null || !this.video || !this.canvas) {
      return;
    }
    this.probed = false;
    this.probeAttempts = 0;
    this.previous = null;
    this.current = null;
    this.lastSourceTime = -1;
    this.sourceIntervalMs = 0;
    this.sourceFrames = 0;
    this.outputFrames = 0;
    this.sampleStart = 0;
    this.lastStepTime = 0;
    this.reason = "starting";
    this.schedule();
  };

  // Leaves the video element visible again on every exit path, including the failure ones.
  // A half-torn-down interpolator that hid the video and stopped drawing would look exactly
  // like the app having crashed.
  FrameInterpolation.prototype.stop = function (reason) {
    if (this.handle !== null) {
      this.cancelFrame(this.handle);
      this.handle = null;
    }
    this.previous = null;
    this.current = null;
    this.context = null;
    this.reason = reason || "off";
    this.sourceFps = 0;
    this.outputFps = 0;
    if (this.canvas) {
      this.canvas.hidden = true;
    }
    if (this.video) {
      this.video.style.visibility = "";
    }
  };

  FrameInterpolation.prototype.schedule = function () {
    const interpolation = this;
    this.handle = this.requestFrame(function (timestamp) {
      interpolation.step(timestamp);
    });
  };

  FrameInterpolation.prototype.status = function () {
    return {
      active: this.reason === "active",
      reason: this.reason,
      sourceFps: this.sourceFps,
      outputFps: this.outputFps,
      addedLatencyMs: this.reason === "active" ? this.sourceIntervalMs : 0,
    };
  };

  // Distinguishes "the TV will not give us the pixels" from "the video has not started yet"
  // by retrying: a stream genuinely begins on black frames, so a single black readback
  // proves nothing. Only a run of them does.
  FrameInterpolation.prototype.probe = function () {
    const video = this.video;
    if (!video.videoWidth || !video.videoHeight) {
      return "waiting";
    }
    const test = surface(8, 8);
    try {
      test.context.drawImage(video, 0, 0, 8, 8);
    } catch (error) {
      this.log("Frame interpolation: the TV refuses to draw video frames (" + String(error) + ")");
      return "draw-blocked";
    }
    let pixels;
    try {
      pixels = test.context.getImageData(0, 0, 8, 8).data;
    } catch (error) {
      this.log("Frame interpolation: video pixels are not readable (" + String(error) + ")");
      return "readback-blocked";
    }
    for (let index = 0; index < pixels.length; index += 4) {
      if (pixels[index] || pixels[index + 1] || pixels[index + 2]) {
        return "ok";
      }
    }
    return "black";
  };

  FrameInterpolation.prototype.resize = function (width, height) {
    if (this.context && this.canvas.width === width && this.canvas.height === height) {
      return;
    }
    this.canvas.width = width;
    this.canvas.height = height;
    this.context = this.canvas.getContext("2d");
    this.previous = surface(width, height);
    this.current = surface(width, height);
    this.lastSourceTime = -1;
  };

  FrameInterpolation.prototype.captureSource = function (timestamp) {
    const rotated = this.previous;
    this.previous = this.current;
    this.current = rotated;
    this.current.context.drawImage(this.video, 0, 0, this.canvas.width, this.canvas.height);
    if (this.previous.time > 0) {
      const delta = timestamp - this.previous.time;
      if (delta >= MIN_SOURCE_INTERVAL_MS && delta <= MAX_SOURCE_INTERVAL_MS) {
        this.sourceIntervalMs = this.sourceIntervalMs === 0
          ? delta
          : this.sourceIntervalMs + (delta - this.sourceIntervalMs) * INTERVAL_SMOOTHING;
      }
    }
    this.current.time = timestamp;
    this.sourceFrames += 1;
  };

  // Presents a timeline delayed by one source interval, so the phase between the two stored
  // frames is always in the past and always interpolated rather than guessed forward.
  FrameInterpolation.prototype.render = function (timestamp) {
    const width = this.canvas.width;
    const height = this.canvas.height;
    const context = this.context;
    if (!this.previous || this.previous.time === 0 || this.sourceIntervalMs === 0) {
      context.globalAlpha = 1;
      context.drawImage(this.current.canvas, 0, 0, width, height);
      return;
    }
    const phase = (timestamp - this.sourceIntervalMs - this.previous.time)
      / this.sourceIntervalMs;
    const alpha = phase < 0 ? 0 : (phase > 1 ? 1 : phase);
    context.globalAlpha = 1;
    context.drawImage(this.previous.canvas, 0, 0, width, height);
    if (alpha > MIN_BLEND_ALPHA) {
      context.globalAlpha = alpha;
      context.drawImage(this.current.canvas, 0, 0, width, height);
      context.globalAlpha = 1;
    }
  };

  FrameInterpolation.prototype.sample = function (timestamp) {
    if (this.sampleStart === 0) {
      this.sampleStart = timestamp;
      return;
    }
    const elapsed = timestamp - this.sampleStart;
    if (elapsed < SAMPLE_WINDOW_MS) {
      return;
    }
    this.sourceFps = Math.round((this.sourceFrames * 1000) / elapsed);
    this.outputFps = Math.round((this.outputFrames * 1000) / elapsed);
    this.sourceFrames = 0;
    this.outputFrames = 0;
    this.sampleStart = timestamp;
    // Reported rather than enforced. Running without headroom buys nothing and costs a
    // frame of latency, but it is a legitimate thing to want to see for yourself.
    if (this.sourceFps > 0 && this.outputFps > 0
      && this.outputFps < this.sourceFps * HEADROOM_RATIO) {
      this.reason = "no-headroom";
    } else if (this.probed) {
      this.reason = "active";
    }
  };

  FrameInterpolation.prototype.step = function (timestamp) {
    this.handle = null;
    // The presentation loop runs at panel refresh on the same thread as everything else.
    // An exception escaping it would stop the canvas updating while the video underneath
    // stays hidden - a frozen picture with the audio still playing, which is precisely the
    // failure this codebase has already been bitten by once.
    try {
      if (!this.enabled) {
        return;
      }
      if (!this.probed) {
        const verdict = this.probe();
        if (verdict === "ok") {
          this.probed = true;
        } else if (verdict === "waiting" || verdict === "black") {
          this.probeAttempts += 1;
          if (this.probeAttempts >= PROBE_ATTEMPTS) {
            this.log("Frame interpolation unavailable: the TV composites video on an overlay"
              + " the app cannot read. Nothing was changed on screen.");
            this.enabled = false;
            this.stop("unsupported");
            return;
          }
          this.reason = "probing";
          return;
        } else {
          this.enabled = false;
          this.stop("unsupported");
          return;
        }
      }
      if (!this.video.videoWidth) {
        return;
      }
      this.resize(this.video.videoWidth, this.video.videoHeight);
      const sourceTime = this.video.currentTime;
      if (sourceTime !== this.lastSourceTime) {
        this.lastSourceTime = sourceTime;
        this.captureSource(timestamp);
      }
      if (this.current.time === 0) {
        return;
      }
      this.render(timestamp);
      this.outputFrames += 1;
      // Reported as running from the first presented frame. sample() takes over a second
      // later, once there is enough of a measurement to say whether it is doing any good.
      if (this.reason === "starting" || this.reason === "probing") {
        this.reason = "active";
      }
      this.canvas.hidden = false;
      this.video.style.visibility = "hidden";
      this.sample(timestamp);
    } catch (error) {
      this.log("Frame interpolation failed, falling back to direct video: " + String(error));
      this.enabled = false;
      this.stop("failed");
      return;
    } finally {
      if (this.enabled && this.handle === null) {
        this.schedule();
      }
    }
  };

  global.FrameInterpolation = {
    create: function (options) { return new FrameInterpolation(options); },
    testing: { surface: surface, HEADROOM_RATIO: HEADROOM_RATIO },
  };
}(window));
