(function (global) {
  "use strict";

  const DEFAULT_OCTETS = Object.freeze([192, 168, 0, 0]);

  function isOctet(value) {
    return Number.isInteger(value) && value >= 0 && value <= 255;
  }

  function parse(host) {
    const octets = String(host || "").split(".").map(Number);
    return octets.length === 4 && octets.every(isOctet) ? octets : DEFAULT_OCTETS.slice();
  }

  function adjust(value, direction) {
    const normalized = isOctet(value) ? value : 0;
    return (normalized + (direction >= 0 ? 1 : -1) + 256) % 256;
  }

  function format(octets) {
    return Array.isArray(octets) && octets.length === 4 && octets.every(isOctet)
      ? octets.join(".") : DEFAULT_OCTETS.join(".");
  }

  global.GatewayIpv4 = {
    DEFAULT_OCTETS: DEFAULT_OCTETS.slice(),
    parse: parse,
    adjust: adjust,
    format: format,
  };
}(window));
