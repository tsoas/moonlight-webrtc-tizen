(function (global) {
  "use strict";

  const STORAGE_KEY = "moonlight-webrtc.client.preferences.v1";
  const DEFAULTS = Object.freeze({
    resolution: null,
    codec: null,
    hdr: false,
    bitrateKbps: null,
  });

  function copyDefaults() {
    return {
      resolution: DEFAULTS.resolution,
      codec: DEFAULTS.codec,
      hdr: DEFAULTS.hdr,
      bitrateKbps: DEFAULTS.bitrateKbps,
    };
  }

  function normalize(candidate) {
    const values = copyDefaults();
    if (!candidate || typeof candidate !== "object") {
      return values;
    }
    if (typeof candidate.resolution === "string") {
      values.resolution = candidate.resolution;
    }
    if (typeof candidate.codec === "string") {
      values.codec = candidate.codec;
    }
    if (typeof candidate.hdr === "boolean") {
      values.hdr = candidate.hdr;
    }
    if (Number.isInteger(candidate.bitrateKbps) && candidate.bitrateKbps > 0) {
      values.bitrateKbps = candidate.bitrateKbps;
    }
    return values;
  }

  function resolveSupported(value, allowedValues, fallbackValue) {
    return Array.isArray(allowedValues) && allowedValues.some(function (allowedValue) {
      return String(allowedValue) === String(value);
    }) ? value : fallbackValue;
  }

  function ClientPreferences(storage) {
    this.storage = storage || global.localStorage;
    this.values = copyDefaults();
  }

  ClientPreferences.prototype.load = function () {
    try {
      const serialized = this.storage && this.storage.getItem(STORAGE_KEY);
      this.values = serialized ? normalize(JSON.parse(serialized)) : copyDefaults();
    } catch (error) {
      this.values = copyDefaults();
    }
    return this.snapshot();
  };

  ClientPreferences.prototype.snapshot = function () {
    return normalize(this.values);
  };

  ClientPreferences.prototype.replace = function (values) {
    this.values = normalize(values);
    try {
      if (this.storage) {
        this.storage.setItem(STORAGE_KEY, JSON.stringify(this.values));
      }
    } catch (error) {
      // Storage can be disabled by the TV runtime. In-memory preferences still work.
    }
    return this.snapshot();
  };

  ClientPreferences.prototype.update = function (key, value) {
    const values = this.snapshot();
    values[key] = value;
    return this.replace(values);
  };

  ClientPreferences.prototype.clear = function () {
    this.values = copyDefaults();
    try {
      if (this.storage) {
        this.storage.removeItem(STORAGE_KEY);
      }
    } catch (error) {
      // Keep the in-memory defaults when persistent storage is unavailable.
    }
    return this.snapshot();
  };

  global.ClientPreferences = {
    STORAGE_KEY: STORAGE_KEY,
    create: function (storage) { return new ClientPreferences(storage); },
    resolveSupported: resolveSupported,
    testing: {
      defaults: copyDefaults,
      normalize: normalize,
      resolveSupported: resolveSupported,
    },
  };
}(window));
