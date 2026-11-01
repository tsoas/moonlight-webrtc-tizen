(function (global) {
  "use strict";

  const MODULE_SCRIPT_URL = "wasm/wake-on-lan.js";
  const MODULE_FACTORY = "createWakeOnLanModule";
  const RESULT_TIMEOUT_MS = 5000;

  // Returns the canonical "AA:BB:CC:DD:EE:FF" form, or null for anything that cannot identify
  // one network adapter: malformed text, the all-zero address, and group (multicast or
  // broadcast) addresses, which no NIC answers to with a magic packet.
  function normalizeMacAddress(value) {
    if (typeof value !== "string") {
      return null;
    }
    const match = /^([0-9A-Fa-f]{2})([:-])([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})$/
      .exec(value.trim());
    if (!match) {
      return null;
    }
    const octets = [match[1], match[3], match[4], match[5], match[6], match[7]].map(function (octet) {
      return octet.toUpperCase();
    });
    const bytes = octets.map(function (octet) { return parseInt(octet, 16); });
    if (bytes.every(function (byte) { return byte === 0; }) || (bytes[0] & 1) === 1) {
      return null;
    }
    return octets.join(":");
  }

  function macAddressBytes(macAddress) {
    const normalized = normalizeMacAddress(macAddress);
    return normalized
      ? new Uint8Array(normalized.split(":").map(function (octet) { return parseInt(octet, 16); }))
      : null;
  }

  // The magic packet itself is assembled in WebAssembly; this mirror exists so the layout the
  // TV sends can be checked without a TV.
  function buildMagicPacket(macAddress) {
    const mac = macAddressBytes(macAddress);
    if (!mac) {
      return null;
    }
    const packet = new Uint8Array(6 + 16 * 6);
    packet.fill(0xff, 0, 6);
    for (let repetition = 1; repetition <= 16; repetition += 1) {
      packet.set(mac, repetition * 6);
    }
    return packet;
  }

  function WakeOnLanSender(options) {
    this.options = options || {};
    this.document = this.options.document || global.document;
    this.scriptUrl = this.options.scriptUrl || MODULE_SCRIPT_URL;
    this.log = typeof this.options.log === "function" ? this.options.log : function () {};
    this.modulePromise = null;
    this.nextRequestId = 1;
    this.pending = new Map();
  }

  // Tizen exposes sockets to WebAssembly only on Samsung TVs; the module also needs threads.
  WakeOnLanSender.prototype.isSupported = function () {
    return typeof global.tizentvwasm !== "undefined"
      && typeof global.WebAssembly !== "undefined"
      && typeof global.SharedArrayBuffer !== "undefined";
  };

  WakeOnLanSender.prototype.handleResult = function (requestId, sent, errorCode) {
    const request = this.pending.get(requestId);
    if (!request) {
      return;
    }
    this.pending.delete(requestId);
    clearTimeout(request.timer);
    if (sent > 0) {
      request.resolve({ sent: sent });
    } else {
      request.reject(new Error("The Wake-on-LAN packet could not be sent (error " + String(errorCode) + ")"));
    }
  };

  WakeOnLanSender.prototype.loadModule = function () {
    if (this.modulePromise) {
      return this.modulePromise;
    }
    const sender = this;
    this.modulePromise = new Promise(function (resolve, reject) {
      const script = sender.document.createElement("script");
      script.src = sender.scriptUrl;
      script.async = true;
      script.onerror = function () {
        reject(new Error("The Wake-on-LAN module could not be loaded"));
      };
      script.onload = function () {
        const factory = global[MODULE_FACTORY];
        if (typeof factory !== "function") {
          reject(new Error("The Wake-on-LAN module is invalid"));
          return;
        }
        // The factory installs the runtime on the object it is given. That object becomes a
        // thenable resolving to itself, which loops forever if passed to resolve(), so it is
        // wrapped, and onRuntimeInitialized is the readiness signal.
        const runtime = {
          onWakeResult: function (requestId, sent, errorCode) {
            sender.handleResult(requestId, sent, errorCode);
          },
          onRuntimeInitialized: function () { resolve({ runtime: runtime }); },
          onAbort: function (reason) {
            reject(new Error("The Wake-on-LAN module stopped: " + String(reason)));
          },
          print: function (text) { sender.log("Wake-on-LAN: " + text); },
          printErr: function (text) { sender.log("Wake-on-LAN: " + text); },
        };
        factory(runtime);
      };
      sender.document.head.appendChild(script);
    }).catch(function (error) {
      // Allow a later attempt to reload the module after a transient failure.
      sender.modulePromise = null;
      throw error;
    });
    return this.modulePromise;
  };

  // Resolves once at least one magic packet has left the TV. The unicast address is the
  // Gateway's last known IPv4 address and is optional.
  WakeOnLanSender.prototype.wake = function (macAddress, unicastAddress) {
    const mac = macAddressBytes(macAddress);
    if (!mac) {
      return Promise.reject(new Error("The Gateway MAC address is not valid"));
    }
    if (!this.isSupported()) {
      return Promise.reject(new Error("Wake-on-LAN requires a Samsung TV with WebAssembly sockets"));
    }
    const sender = this;
    return this.loadModule().then(function (loaded) {
      return new Promise(function (resolve, reject) {
        const requestId = sender.nextRequestId;
        sender.nextRequestId += 1;
        const timer = setTimeout(function () {
          sender.pending.delete(requestId);
          reject(new Error("The Wake-on-LAN request timed out"));
        }, RESULT_TIMEOUT_MS);
        sender.pending.set(requestId, { resolve: resolve, reject: reject, timer: timer });
        const status = loaded.runtime.ccall("wol_send", "number", ["number", "array", "string"],
          [requestId, mac, typeof unicastAddress === "string" ? unicastAddress : ""]);
        if (status !== 0) {
          sender.handleResult(requestId, 0, status);
        }
      });
    });
  };

  global.WakeOnLan = {
    MODULE_SCRIPT_URL: MODULE_SCRIPT_URL,
    normalizeMacAddress: normalizeMacAddress,
    create: function (options) { return new WakeOnLanSender(options); },
    testing: { macAddressBytes: macAddressBytes, buildMagicPacket: buildMagicPacket },
  };
}(window));
