(function (global) {
  "use strict";

  // localStorage on a Tizen TV is backed by a database the platform flushes on its own
  // schedule, not on setItem. Leaving the app is a kill, not a shutdown, so a value written
  // in the seconds before the user exits can still be sitting in that buffer and is simply
  // gone on the next launch. That is the whole story behind a Gateway that was added, worked
  // for the rest of the session, and had vanished by the morning.
  //
  // tizen.preference writes through to the application's own data directory synchronously,
  // so mirroring every value there gives it a second home that survives the kill. Neither
  // backend is trusted on its own: a read prefers localStorage because it is the fast path,
  // falls back to the mirror when localStorage comes back empty, and repairs localStorage
  // from the mirror so the rest of the run behaves normally.

  function localBackend(storage) {
    const target = storage || global.localStorage;
    if (!target) {
      return null;
    }
    return {
      name: "localStorage",
      read: function (key) {
        const value = target.getItem(key);
        return typeof value === "string" ? value : null;
      },
      write: function (key, value) { target.setItem(key, value); },
      remove: function (key) { target.removeItem(key); },
    };
  }

  function preferenceBackend(preference) {
    // Feature-detected rather than version-gated: the API is absent on desktop browsers,
    // where the tests run, and its availability across TV firmwares is not worth asserting.
    const target = preference
      || (global.tizen && global.tizen.preference)
      || null;
    if (!target || typeof target.setValue !== "function"
      || typeof target.getValue !== "function" || typeof target.exists !== "function") {
      return null;
    }
    return {
      name: "tizen.preference",
      read: function (key) {
        return target.exists(key) ? String(target.getValue(key)) : null;
      },
      write: function (key, value) { target.setValue(key, String(value)); },
      remove: function (key) {
        if (target.exists(key) && typeof target.remove === "function") {
          target.remove(key);
        }
      },
    };
  }

  function DurableStorage(backends, log) {
    this.backends = backends;
    this.log = typeof log === "function" ? log : function () {};
  }

  DurableStorage.prototype.describe = function () {
    return this.backends.length === 0
      ? "none"
      : this.backends.map(function (backend) { return backend.name; }).join(" + ");
  };

  DurableStorage.prototype.getItem = function (key) {
    let value = null;
    let recoveredFrom = null;
    for (let index = 0; index < this.backends.length; index += 1) {
      const backend = this.backends[index];
      try {
        value = backend.read(key);
      } catch (error) {
        this.log("Storage read failed on " + backend.name + ": " + String(error));
        continue;
      }
      if (value !== null) {
        if (index > 0) {
          recoveredFrom = backend.name;
        }
        break;
      }
    }
    if (recoveredFrom !== null) {
      // Only reached when the preferred backend lost the value, which is exactly the
      // failure this module exists for. Say so: it is the evidence that it happened.
      this.log("Recovered " + key + " from " + recoveredFrom);
      this.setItem(key, value);
    }
    return value;
  };

  // Writes to every backend rather than stopping at the first success, because the point
  // is redundancy. Reports which ones took the value so a TV that silently persists
  // nothing is visible in the diagnostics log instead of being mistaken for a lost setting.
  DurableStorage.prototype.setItem = function (key, value) {
    let stored = 0;
    for (let index = 0; index < this.backends.length; index += 1) {
      const backend = this.backends[index];
      try {
        backend.write(key, String(value));
        stored += 1;
      } catch (error) {
        this.log("Storage write failed on " + backend.name + ": " + String(error));
      }
    }
    if (stored === 0) {
      this.log("Storage write failed for " + key + "; the value will not survive a restart");
    }
    return stored > 0;
  };

  DurableStorage.prototype.removeItem = function (key) {
    this.backends.forEach(function (backend) {
      try {
        backend.remove(key);
      } catch (error) {
        this.log("Storage remove failed on " + backend.name + ": " + String(error));
      }
    }, this);
  };

  global.DurableStorage = {
    create: function (options) {
      const settings = options || {};
      const backends = [
        localBackend(settings.storage),
        preferenceBackend(settings.preference),
      ].filter(Boolean);
      return new DurableStorage(backends, settings.log);
    },
    testing: { localBackend: localBackend, preferenceBackend: preferenceBackend },
  };
}(window));
