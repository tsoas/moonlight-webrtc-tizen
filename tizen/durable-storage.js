(function (global) {
  "use strict";

  // localStorage on a Tizen TV is backed by a database the platform flushes on its own
  // schedule, not on setItem. Leaving the app is a kill, not a shutdown, so a value written
  // in the seconds before the user exits can still be sitting in that buffer and is simply
  // gone on the next launch. That is the whole story behind a Gateway that was added, worked
  // for the rest of the session, and had vanished by the morning.
  //
  // Every value is therefore also written to backends that reach the disk before setItem
  // returns: tizen.preference and a file in the widget's private directory, flushed with
  // sync(). Either may be missing on a given firmware, which is why there are two of them.
  //
  // Reads prefer those durable backends and only fall back to localStorage. Preferring
  // localStorage would be wrong in the other direction: after a kill it does not come back
  // empty, it comes back with the value from an earlier session, which looks valid and
  // silently replaces everything written since. A backend that misses the value, or holds
  // a different one, is repaired from the one that answered.

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

  function fileBackend(filesystem) {
    const target = filesystem
      || (global.tizen && global.tizen.filesystem)
      || null;
    if (!target || typeof target.openFile !== "function"
      || typeof target.pathExists !== "function") {
      return null;
    }
    function pathFor(key) {
      return "wgt-private/" + String(key).replace(/[^A-Za-z0-9._-]/g, "_") + ".json";
    }
    return {
      name: "tizen.filesystem",
      read: function (key) {
        const path = pathFor(key);
        if (!target.pathExists(path)) {
          return null;
        }
        const handle = target.openFile(path, "r");
        let contents;
        try {
          contents = handle.readString();
        } finally {
          handle.close();
        }
        // The value is wrapped so that a file cut short by a kill mid-write fails to parse
        // and is treated as missing, instead of handing a truncated value to the caller.
        try {
          const envelope = JSON.parse(contents);
          return envelope && typeof envelope.value === "string" ? envelope.value : null;
        } catch (error) {
          return null;
        }
      },
      write: function (key, value) {
        const handle = target.openFile(pathFor(key), "w");
        try {
          handle.writeString(JSON.stringify({ value: String(value) }));
          if (typeof handle.sync === "function") {
            handle.sync();
          }
        } finally {
          handle.close();
        }
      },
      remove: function (key) {
        const path = pathFor(key);
        if (target.pathExists(path) && typeof target.deleteFile === "function") {
          target.deleteFile(path);
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
    let source = null;
    const stale = [];
    for (let index = 0; index < this.backends.length; index += 1) {
      const backend = this.backends[index];
      let candidate;
      try {
        candidate = backend.read(key);
      } catch (error) {
        this.log("Storage read failed on " + backend.name + ": " + String(error));
        continue;
      }
      if (source === null) {
        if (candidate !== null) {
          value = candidate;
          source = backend;
        } else {
          stale.push(backend);
        }
      } else if (candidate !== value) {
        stale.push(backend);
      }
    }
    if (source !== null && stale.length > 0) {
      // Only reached when a backend lost or never got the newest value, which is exactly
      // the failure this module exists for. Say so: it is the evidence that it happened.
      this.log("Recovered " + key + " from " + source.name + " into "
        + stale.map(function (backend) { return backend.name; }).join(", "));
      stale.forEach(function (backend) {
        try {
          backend.write(key, value);
        } catch (error) {
          this.log("Storage write failed on " + backend.name + ": " + String(error));
        }
      }, this);
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
      // Ordered by how much a read trusts them: the backends that write through to disk
      // first, localStorage last.
      const backends = [
        preferenceBackend(settings.preference),
        fileBackend(settings.filesystem),
        localBackend(settings.storage),
      ].filter(Boolean);
      return new DurableStorage(backends, settings.log);
    },
    testing: {
      localBackend: localBackend,
      preferenceBackend: preferenceBackend,
      fileBackend: fileBackend,
    },
  };
}(window));
