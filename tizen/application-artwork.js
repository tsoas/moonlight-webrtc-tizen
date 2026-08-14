(function (global) {
  "use strict";

  const MAX_ARTWORK_DATA_LENGTH = 12 * 1024 * 1024;
  const SUPPORTED_MIME_TYPES = new Set(["image/jpeg", "image/png", "image/webp"]);

  function validArtwork(message) {
    if (!message || typeof message.appId !== "string" || !message.available
        || typeof message.mimeType !== "string" || typeof message.data !== "string") {
      return false;
    }
    return SUPPORTED_MIME_TYPES.has(message.mimeType)
      && message.data.length > 0
      && message.data.length <= MAX_ARTWORK_DATA_LENGTH
      && message.data.length % 4 === 0
      && /^[A-Za-z0-9+/]*={0,2}$/.test(message.data);
  }

  function ApplicationArtworkLoader(options) {
    this.options = options;
    this.currentIds = new Set();
    this.loaded = new Map();
    this.unavailable = new Set();
    this.pending = [];
    this.pendingIds = new Set();
    this.inFlight = null;
    this.gatewayContext = "";
  }

  ApplicationArtworkLoader.prototype.setGatewayContext = function (gatewayContext) {
    const nextContext = String(gatewayContext || "");
    if (this.gatewayContext === nextContext) {
      return;
    }
    this.gatewayContext = nextContext;
    this.currentIds.clear();
    this.loaded.clear();
    this.unavailable.clear();
    this.pending = [];
    this.pendingIds.clear();
    this.inFlight = null;
  };

  ApplicationArtworkLoader.prototype.setApplications = function (applications) {
    const loader = this;
    this.currentIds = new Set();
    (Array.isArray(applications) ? applications : []).forEach(function (application) {
      const appId = String(application.id || "");
      if (!appId) {
        return;
      }
      loader.currentIds.add(appId);
      if (loader.loaded.has(appId)) {
        loader.options.onArtwork(appId, loader.loaded.get(appId));
      } else if (!loader.unavailable.has(appId)) {
        loader.enqueue(appId);
      }
    });
    this.pump();
  };

  ApplicationArtworkLoader.prototype.enqueue = function (appId) {
    if (this.inFlight !== appId && !this.pendingIds.has(appId)) {
      this.pending.push(appId);
      this.pendingIds.add(appId);
    }
  };

  ApplicationArtworkLoader.prototype.pump = function () {
    if (this.inFlight) {
      return;
    }
    while (this.pending.length > 0) {
      const appId = this.pending.shift();
      this.pendingIds.delete(appId);
      if (!this.currentIds.has(appId) || this.loaded.has(appId) || this.unavailable.has(appId)) {
        continue;
      }
      this.inFlight = appId;
      this.options.requestArtwork(appId);
      return;
    }
  };

  ApplicationArtworkLoader.prototype.handleResponse = function (message) {
    if (!message || typeof message.appId !== "string" || message.appId !== this.inFlight) {
      return false;
    }

    const appId = this.inFlight;
    this.inFlight = null;
    if (validArtwork(message)) {
      const dataUrl = "data:" + message.mimeType + ";base64," + message.data;
      this.loaded.set(appId, dataUrl);
      if (this.currentIds.has(appId)) {
        this.options.onArtwork(appId, dataUrl);
      }
    } else {
      this.unavailable.add(appId);
      if (this.currentIds.has(appId)) {
        this.options.onArtwork(appId, null);
      }
    }
    this.pump();
    return true;
  };

  global.ApplicationArtworkLoader = {
    create: function (options) { return new ApplicationArtworkLoader(options); },
    testing: { validArtwork: validArtwork },
  };
}(window));
