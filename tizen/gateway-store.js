(function (global) {
  "use strict";

  const STORAGE_KEY = "moonlight-webrtc.client.gateways.v1";
  const DEFAULT_PORT = 8000;

  function isIpv4(host) {
    const octets = typeof host === "string" ? host.split(".") : [];
    return octets.length === 4 && octets.every(function (octet) {
      return /^(0|[1-9][0-9]{0,2})$/.test(octet)
        && Number(octet) >= 0 && Number(octet) <= 255;
    });
  }

  function normalize(candidate) {
    if (!candidate || typeof candidate !== "object" || !isIpv4(candidate.host)) {
      return null;
    }
    const port = Number.isInteger(candidate.port) && candidate.port > 0 && candidate.port <= 65535
      ? candidate.port : DEFAULT_PORT;
    const host = candidate.host;
    const name = typeof candidate.name === "string" && candidate.name.trim()
      ? candidate.name.trim().slice(0, 96) : "Gateway " + host;
    return { id: host + ":" + String(port), host: host, port: port, name: name };
  }

  function normalizeList(candidate) {
    const seen = new Set();
    return (Array.isArray(candidate) ? candidate : []).reduce(function (gateways, entry) {
      const gateway = normalize(entry);
      if (gateway && !seen.has(gateway.id)) {
        seen.add(gateway.id);
        gateways.push(gateway);
      }
      return gateways;
    }, []);
  }

  function GatewayStore(storage) {
    this.storage = storage || global.localStorage;
    this.gateways = [];
  }

  GatewayStore.prototype.load = function () {
    try {
      const serialized = this.storage && this.storage.getItem(STORAGE_KEY);
      this.gateways = serialized ? normalizeList(JSON.parse(serialized)) : [];
    } catch (error) {
      this.gateways = [];
    }
    return this.list();
  };

  GatewayStore.prototype.list = function () {
    return this.gateways.map(function (gateway) { return Object.assign({}, gateway); });
  };

  GatewayStore.prototype.find = function (id) {
    const gateway = this.gateways.find(function (entry) { return entry.id === String(id); });
    return gateway ? Object.assign({}, gateway) : null;
  };

  GatewayStore.prototype.save = function () {
    try {
      if (this.storage) {
        this.storage.setItem(STORAGE_KEY, JSON.stringify(this.gateways));
      }
    } catch (error) {
      // The TV may not expose persistent storage. The in-memory list remains usable.
    }
    return this.list();
  };

  GatewayStore.prototype.upsert = function (candidate) {
    const gateway = normalize(candidate);
    if (!gateway) {
      return null;
    }
    const index = this.gateways.findIndex(function (entry) { return entry.id === gateway.id; });
    if (index >= 0) {
      this.gateways[index] = gateway;
    } else {
      this.gateways.push(gateway);
    }
    this.save();
    return Object.assign({}, gateway);
  };

  GatewayStore.prototype.replace = function (previousId, candidate) {
    const gateway = normalize(candidate);
    const index = this.gateways.findIndex(function (entry) { return entry.id === String(previousId); });
    if (!gateway || index < 0) {
      return null;
    }
    const duplicateIndex = this.gateways.findIndex(function (entry) {
      return entry.id === gateway.id && entry.id !== String(previousId);
    });
    if (duplicateIndex >= 0) {
      return null;
    }
    this.gateways[index] = gateway;
    this.save();
    return Object.assign({}, gateway);
  };

  GatewayStore.prototype.remove = function (id) {
    const index = this.gateways.findIndex(function (entry) { return entry.id === String(id); });
    if (index < 0) {
      return false;
    }
    this.gateways.splice(index, 1);
    this.save();
    return true;
  };

  global.GatewayStore = {
    STORAGE_KEY: STORAGE_KEY,
    DEFAULT_PORT: DEFAULT_PORT,
    create: function (storage) { return new GatewayStore(storage); },
    testing: { isIpv4: isIpv4, normalize: normalize, normalizeList: normalizeList },
  };
}(window));
