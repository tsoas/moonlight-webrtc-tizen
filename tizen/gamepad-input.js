(function (global) {
  "use strict";

  const PROTOCOL_VERSION = 2;
  const MAX_CONTROLLERS = 16;
  const POLL_INTERVAL_MS = 1000 / 120;
  const KEEPALIVE_INTERVAL_MS = 250;
  // Mirror BrightCraft's Moonlight Tizen implementation. Sunshine renews the
  // state when it changes, including a zero-magnitude state to stop rumble.
  const RUMBLE_DURATION_MS = 5000;

  const BUTTONS = {
    A: 1 << 0,
    B: 1 << 1,
    X: 1 << 2,
    Y: 1 << 3,
    LB: 1 << 4,
    RB: 1 << 5,
    BACK: 1 << 6,
    START: 1 << 7,
    LEFT_STICK: 1 << 8,
    RIGHT_STICK: 1 << 9,
    DPAD_UP: 1 << 10,
    DPAD_DOWN: 1 << 11,
    DPAD_LEFT: 1 << 12,
    DPAD_RIGHT: 1 << 13,
    GUIDE: 1 << 14,
  };

  function channelIsOpen(channel) {
    return channel && channel.readyState === "open";
  }

  function currentGamepads() {
    if (!navigator.getGamepads) {
      return [];
    }
    return navigator.getGamepads() || [];
  }

  function button(gamepad, index) {
    return gamepad.buttons && gamepad.buttons[index]
      ? gamepad.buttons[index]
      : { pressed: false, value: 0 };
  }

  function buttonPressed(gamepad, index) {
    const value = button(gamepad, index);
    return value.pressed || Number(value.value) > 0.5;
  }

  function axis(gamepad, index) {
    if (!gamepad.axes || typeof gamepad.axes[index] !== "number") {
      return 0;
    }
    return gamepad.axes[index];
  }

  function standardButtonFlags(gamepad) {
    let flags = 0;
    [
      [0, BUTTONS.A], [1, BUTTONS.B], [2, BUTTONS.X], [3, BUTTONS.Y],
      [4, BUTTONS.LB], [5, BUTTONS.RB], [8, BUTTONS.BACK], [9, BUTTONS.START],
      [10, BUTTONS.LEFT_STICK], [11, BUTTONS.RIGHT_STICK],
      [12, BUTTONS.DPAD_UP], [13, BUTTONS.DPAD_DOWN],
      [14, BUTTONS.DPAD_LEFT], [15, BUTTONS.DPAD_RIGHT], [16, BUTTONS.GUIDE],
    ].forEach(function (mapping) {
      if (buttonPressed(gamepad, mapping[0])) {
        flags |= mapping[1];
      }
    });
    return flags;
  }

  function completeState(gamepad) {
    return {
      buttons: standardButtonFlags(gamepad),
      leftTrigger: Number(button(gamepad, 6).value) || 0,
      rightTrigger: Number(button(gamepad, 7).value) || 0,
      leftStickX: axis(gamepad, 0),
      leftStickY: axis(gamepad, 1),
      rightStickX: axis(gamepad, 2),
      rightStickY: axis(gamepad, 3),
    };
  }

  function neutralState() {
    return {
      buttons: 0,
      leftTrigger: 0,
      rightTrigger: 0,
      leftStickX: 0,
      leftStickY: 0,
      rightStickX: 0,
      rightStickY: 0,
    };
  }

  function effectsFor(gamepad) {
    const actuator = gamepad && gamepad.vibrationActuator;
    if (!actuator || typeof actuator.playEffect !== "function") {
      return { actuator: null, names: [], dualRumble: false, triggerRumble: false };
    }
    const names = actuator.effects && typeof actuator.effects.length === "number"
      ? Array.prototype.slice.call(actuator.effects)
      : [];
    return {
      actuator: actuator,
      names: names,
      dualRumble: names.length === 0 || names.indexOf("dual-rumble") >= 0,
      triggerRumble: names.indexOf("trigger-rumble") >= 0,
    };
  }

  function clampMagnitude(value) {
    const number = Number(value);
    return Number.isFinite(number) ? Math.max(0, Math.min(1, number)) : 0;
  }

  function GamepadInputManager(options) {
    this.options = options;
    this.recordsByIndex = new Map();
    this.recordsById = new Map();
    this.nextControllerId = 1;
    this.pollTimer = null;
    this.nextPollTime = 0;
    this.sessionActive = false;
  }

  GamepadInputManager.prototype.gamepadAtIndex = function (browserIndex) {
    const gamepads = currentGamepads();
    for (let index = 0; index < gamepads.length; index += 1) {
      if (gamepads[index] && gamepads[index].index === browserIndex) {
        return gamepads[index];
      }
    }
    return null;
  };

  GamepadInputManager.prototype.register = function (gamepad) {
    if (!gamepad || this.recordsByIndex.has(gamepad.index)) {
      return this.recordsByIndex.get(gamepad ? gamepad.index : -1) || null;
    }
    if (this.recordsByIndex.size >= MAX_CONTROLLERS) {
      this.options.log("Gamepad ignored: maximum 16 controllers reached");
      return null;
    }
    const haptics = effectsFor(gamepad);
    const record = {
      browserIndex: gamepad.index,
      controllerId: this.nextControllerId,
      id: gamepad.id || "Unknown gamepad",
      mapping: gamepad.mapping || "",
      buttonCount: gamepad.buttons ? gamepad.buttons.length : 0,
      axisCount: gamepad.axes ? gamepad.axes.length : 0,
      dualRumble: haptics.dualRumble,
      triggerRumble: haptics.triggerRumble,
      effects: haptics.names,
      announced: false,
      moonlightSlot: null,
      sequence: 0,
      lastFingerprint: "",
      lastSendTime: 0,
      rateWindowStart: performance.now(),
      messagesInWindow: 0,
      messagesPerSecond: 0,
      gatewayMessagesPerSecond: 0,
      sequenceGaps: 0,
      staleStates: 0,
      mouseMode: false,
      stopShortcutHeld: false,
      rumble: {
        strongMagnitude: 0,
        weakMagnitude: 0,
        leftTrigger: 0,
        rightTrigger: 0,
        status: "idle",
      },
    };
    this.nextControllerId += 1;
    this.recordsByIndex.set(gamepad.index, record);
    this.recordsById.set(record.controllerId, record);
    this.options.log("Gamepad detected: controller=" + String(record.controllerId)
      + " index=" + String(record.browserIndex) + " id=" + record.id);
    this.options.log("Gamepad haptics: controller=" + String(record.controllerId)
      + " dual-rumble=" + String(record.dualRumble)
      + " trigger-rumble=" + String(record.triggerRumble)
      + " effects=" + (record.effects.length ? record.effects.join(",") : "unreported"));
    this.announce(record);
    this.updateDiagnostics();
    return record;
  };

  GamepadInputManager.prototype.enumerate = function () {
    const seen = new Map();
    const gamepads = currentGamepads();
    for (let index = 0; index < gamepads.length; index += 1) {
      const gamepad = gamepads[index];
      if (gamepad) {
        seen.set(gamepad.index, true);
        this.register(gamepad);
      }
    }
    const missing = [];
    this.recordsByIndex.forEach(function (record, browserIndex) {
      if (!seen.has(browserIndex)) {
        missing.push(browserIndex);
      }
    });
    for (let index = 0; index < missing.length; index += 1) {
      this.unregister(missing[index], true);
    }
  };

  GamepadInputManager.prototype.announce = function (record) {
    const channel = this.options.controlChannel();
    if (!record || record.announced || !channelIsOpen(channel)
        || document.hidden || !this.options.isStreaming()) {
      return false;
    }
    channel.send(JSON.stringify({
      v: PROTOCOL_VERSION,
      type: "gamepad-connected",
      controllerId: record.controllerId,
      // Keep the legacy field for the currently deployed single-controller bridge.
      index: record.browserIndex,
      browserIndex: record.browserIndex,
      id: record.id,
      mapping: record.mapping,
      buttons: record.buttonCount,
      axes: record.axisCount,
      dualRumble: record.dualRumble,
      triggerRumble: record.triggerRumble,
    }));
    record.announced = true;
    return true;
  };

  GamepadInputManager.prototype.sendState = function (record, state, now) {
    const channel = this.options.gamepadChannel();
    if (!record || !record.announced || !channelIsOpen(channel)) {
      return false;
    }
    record.sequence += 1;
    channel.send(JSON.stringify({
      v: PROTOCOL_VERSION,
      type: "gamepad-state",
      controllerId: record.controllerId,
      seq: record.sequence,
      timestampMs: now,
      buttons: state.buttons,
      leftTrigger: state.leftTrigger,
      rightTrigger: state.rightTrigger,
      leftStickX: state.leftStickX,
      leftStickY: state.leftStickY,
      rightStickX: state.rightStickX,
      rightStickY: state.rightStickY,
    }));
    record.lastSendTime = now;
    record.messagesInWindow += 1;
    const elapsed = now - record.rateWindowStart;
    if (elapsed >= 1000) {
      record.messagesPerSecond = record.messagesInWindow * 1000 / elapsed;
      record.messagesInWindow = 0;
      record.rateWindowStart = now;
    }
    return true;
  };

  GamepadInputManager.prototype.stopRumble = function (record) {
    if (!record) {
      return;
    }
    record.rumble.strongMagnitude = 0;
    record.rumble.weakMagnitude = 0;
    record.rumble.leftTrigger = 0;
    record.rumble.rightTrigger = 0;
    record.rumble.status = "stopped";
    if (record.moonlightSlot !== null) {
      this.applyRumble(record, record.moonlightSlot);
    }
  };

  GamepadInputManager.prototype.applyRumble = function (record, logicalControllerNumber) {
    if (!record) {
      return;
    }
    const desired = record.rumble;
    // Get a fresh Gamepad instance for every Moonlight rumble update.
    const gamepad = this.gamepadAtIndex(record.browserIndex);
    const actuator = gamepad && gamepad.vibrationActuator;
    const details = "logical=" + String(logicalControllerNumber)
      + " physical=" + String(record.browserIndex)
      + " id=" + (gamepad ? String(gamepad.id || record.id) : "missing")
      + " actuator=" + String(Boolean(actuator))
      + " strong=" + String(desired.strongMagnitude)
      + " weak=" + String(desired.weakMagnitude);
    if (!actuator || typeof actuator.playEffect !== "function") {
      record.rumble.status = "no-actuator";
      this.options.log("Rumble skipped: " + details);
      this.updateDiagnostics();
      return;
    }
    const parameters = {
      duration: RUMBLE_DURATION_MS,
      startDelay: 0,
      strongMagnitude: desired.strongMagnitude,
      weakMagnitude: desired.weakMagnitude,
    };
    const manager = this;
    try {
      const result = actuator.playEffect("dual-rumble", parameters);
      record.rumble.status = "sent";
      this.options.log("Rumble playEffect: " + details + " result=" + String(result));
      if (result && typeof result.then === "function") {
        result.then(function () {
          record.rumble.status = "accepted";
          manager.options.log("Rumble accepted: " + details);
          manager.updateDiagnostics();
        }, function (error) {
          record.rumble.status = "rejected";
          manager.options.log("Rumble rejected: " + details + " error=" + String(error));
          manager.updateDiagnostics();
        });
      }
      this.updateDiagnostics();
    } catch (error) {
      record.rumble.status = "failed";
      this.options.log("Rumble failed: " + details + " error=" + String(error));
      this.updateDiagnostics();
    }
  };

  GamepadInputManager.prototype.handleRumble = function (message) {
    const logicalControllerNumber = Number(message.controllerSlot);
    const controllerId = Number(message.controllerId);
    const recordById = Number.isFinite(controllerId)
      ? this.recordsById.get(controllerId) : null;
    const record = recordById || Array.from(this.recordsById.values()).find(function (candidate) {
      return candidate.moonlightSlot === logicalControllerNumber;
    });
    if (!record || (!record.dualRumble && !record.triggerRumble)) {
      this.options.log("Rumble ignored: logical=" + String(logicalControllerNumber)
        + " controllerId=" + String(message.controllerId));
      return;
    }
    record.rumble.strongMagnitude = clampMagnitude(message.strongMagnitude);
    record.rumble.weakMagnitude = clampMagnitude(message.weakMagnitude);
    record.rumble.leftTrigger = record.triggerRumble
      ? clampMagnitude(message.leftTrigger) : 0;
    record.rumble.rightTrigger = record.triggerRumble
      ? clampMagnitude(message.rightTrigger) : 0;
    const resolvedControllerNumber = Number.isFinite(logicalControllerNumber)
      ? logicalControllerNumber : record.moonlightSlot;
    this.applyRumble(record, resolvedControllerNumber);
  };

  GamepadInputManager.prototype.handleControlMessage = function (data) {
    if (typeof data !== "string") {
      return;
    }
    let message;
    try {
      message = JSON.parse(data);
    } catch (error) {
      this.options.log("Ignored malformed control DataChannel message");
      return;
    }
    if (message.v !== PROTOCOL_VERSION && message.v !== 1) {
      return;
    }
    const record = this.recordsById.get(Number(message.controllerId));
    if (message.type === "rumble") {
      this.handleRumble(message);
    } else if (message.type === "controller-status" && record) {
      record.moonlightSlot = Number(message.controllerSlot);
      this.updateDiagnostics();
    } else if (message.type === "input-diagnostics" && record) {
      record.moonlightSlot = Number(message.controllerSlot);
      record.gatewayMessagesPerSecond = Number(message.messagesPerSecond) || 0;
      record.sequenceGaps = Number(message.sequenceGaps) || 0;
      record.staleStates = Number(message.staleStates) || 0;
      this.updateDiagnostics();
    } else if (message.type === "mouse-mode" && record) {
      const active = Boolean(message.active);
      const changed = record.mouseMode !== active;
      record.mouseMode = active;
      this.updateMouseOverlay();
      this.updateDiagnostics();
      if (changed && typeof this.options.onMouseModeChanged === "function") {
        this.options.onMouseModeChanged(record, active);
      }
    }
  };

  GamepadInputManager.prototype.observeStopShortcut = function (record, state) {
    const required = BUTTONS.LB | BUTTONS.RB | BUTTONS.BACK | BUTTONS.START;
    const pressed = (state.buttons & required) === required;
    if (!pressed) {
      record.stopShortcutHeld = false;
      return;
    }
    if (record.stopShortcutHeld) {
      return;
    }
    record.stopShortcutHeld = true;
    if (typeof this.options.onStopShortcut === "function") {
      this.options.onStopShortcut(record);
    }
  };

  GamepadInputManager.prototype.unregister = function (browserIndex, notifyGateway) {
    const record = this.recordsByIndex.get(browserIndex);
    if (!record) {
      return;
    }
    if (record.announced) {
      this.sendState(record, neutralState(), performance.now());
      const channel = this.options.controlChannel();
      if (notifyGateway && channelIsOpen(channel)) {
        channel.send(JSON.stringify({
          v: PROTOCOL_VERSION,
          type: "gamepad-disconnected",
          controllerId: record.controllerId,
        }));
      }
    }
    this.stopRumble(record);
    this.recordsByIndex.delete(browserIndex);
    this.recordsById.delete(record.controllerId);
    this.options.log("Gamepad disconnected: controller=" + String(record.controllerId));
    this.updateMouseOverlay();
    this.updateDiagnostics();
  };

  GamepadInputManager.prototype.poll = function () {
    this.pollTimer = null;
    if (document.hidden || !this.options.isStreaming()) {
      return;
    }
    this.enumerate();
    const now = performance.now();
    const manager = this;
    this.recordsByIndex.forEach(function (record) {
      const gamepad = manager.gamepadAtIndex(record.browserIndex);
      if (!gamepad) {
        return;
      }
      manager.announce(record);
      const state = completeState(gamepad);
      manager.observeStopShortcut(record, state);
      const fingerprint = JSON.stringify(state);
      if (fingerprint !== record.lastFingerprint
          || now - record.lastSendTime >= KEEPALIVE_INTERVAL_MS) {
        if (manager.sendState(record, state, now)) {
          record.lastFingerprint = fingerprint;
        }
      }
    });
    this.updateDiagnostics();
    this.nextPollTime += POLL_INTERVAL_MS;
    if (this.nextPollTime < now - POLL_INTERVAL_MS * 4) {
      this.nextPollTime = now + POLL_INTERVAL_MS;
    }
    this.schedulePoll();
  };

  GamepadInputManager.prototype.schedulePoll = function () {
    if (document.hidden || this.pollTimer !== null || !this.options.isStreaming()) {
      return;
    }
    const manager = this;
    const delay = Math.max(0, this.nextPollTime - performance.now());
    this.pollTimer = setTimeout(function () { manager.poll(); }, delay);
  };

  GamepadInputManager.prototype.resume = function () {
    this.enumerate();
    if (!this.options.isStreaming()) {
      this.updateDiagnostics();
      return;
    }
    const manager = this;
    this.recordsByIndex.forEach(function (record) {
      manager.announce(record);
      record.lastFingerprint = "";
    });
    this.nextPollTime = performance.now();
    this.schedulePoll();
  };

  GamepadInputManager.prototype.pauseForUi = function () {
    if (this.pollTimer !== null) {
      clearTimeout(this.pollTimer);
      this.pollTimer = null;
    }
    const now = performance.now();
    const neutral = neutralState();
    this.recordsByIndex.forEach(function (record) {
      if (record.announced) {
        this.sendState(record, neutral, now);
      }
    }, this);
    this.updateDiagnostics();
  };

  GamepadInputManager.prototype.suspend = function (notifyGateway) {
    if (this.pollTimer !== null) {
      clearTimeout(this.pollTimer);
      this.pollTimer = null;
    }
    const indexes = Array.from(this.recordsByIndex.keys());
    for (let index = 0; index < indexes.length; index += 1) {
      this.unregister(indexes[index], notifyGateway);
    }
    this.updateMouseOverlay();
    this.updateDiagnostics();
  };

  GamepadInputManager.prototype.beginSession = function () {
    this.suspend(false);
    this.nextControllerId = 1;
    this.resume();
  };

  GamepadInputManager.prototype.updateMouseOverlay = function () {
    const active = [];
    this.recordsById.forEach(function (record) {
      if (record.mouseMode) {
        const number = record.moonlightSlot === null
          ? record.controllerId
          : record.moonlightSlot + 1;
        active.push(number);
      }
    });
    const element = this.options.mouseOverlay;
    if (!element) {
      return;
    }
    if (active.length === 0) {
      element.hidden = true;
      element.textContent = "";
      return;
    }
    element.textContent = active.length === 1
      ? "Mouse mode active — Controller " + String(active[0])
        + "\nHold Start to return to gamepad"
      : "Mouse mode active — Controllers " + active.join(", ")
        + "\nHold Start to return to gamepad";
    element.hidden = false;
  };

  GamepadInputManager.prototype.updateDiagnostics = function () {
    const records = Array.from(this.recordsById.values()).sort(function (left, right) {
      return left.controllerId - right.controllerId;
    });
    const diagnostics = this.options.diagnostics;
    diagnostics.state.textContent = records.length > 0
      ? String(records.length) + " CONNECTED" : "DISCONNECTED";
    diagnostics.id.textContent = records.length > 0 ? records[0].id : "-";
    diagnostics.mapping.textContent = records.length > 0
      ? (records[0].mapping || "none") : "-";
    diagnostics.sendRate.textContent = records.length > 0
      ? records.map(function (record) { return record.messagesPerSecond.toFixed(1); }).join(" / ")
      : "0.0";
    diagnostics.lastSequence.textContent = records.length > 0
      ? records.map(function (record) { return String(record.sequence); }).join(" / ")
      : "-";
    this.options.overlay.textContent = records.length > 0
      ? String(records.length) + " connected" : "Disconnected";
    if (diagnostics.controllers) {
      diagnostics.controllers.textContent = records.map(function (record) {
        const slot = record.moonlightSlot === null ? "pending" : String(record.moonlightSlot);
        return "Controller " + String(record.controllerId)
          + " | slot " + slot
          + " | seq " + String(record.sequence)
          + " | tx/rx " + record.messagesPerSecond.toFixed(1)
          + "/" + record.gatewayMessagesPerSecond.toFixed(1) + " msg/s"
          + " | gaps " + String(record.sequenceGaps)
          + " | stale " + String(record.staleStates)
          + " | rumble " + (record.dualRumble ? "yes" : "no")
          + " (" + record.rumble.status + ")"
          + " | mouse " + (record.mouseMode ? "active" : "off");
      }).join("\n");
    }
  };

  GamepadInputManager.prototype.firstGamepad = function () {
    const first = this.recordsByIndex.keys().next();
    return first.done ? null : this.gamepadAtIndex(first.value);
  };

  GamepadInputManager.prototype.connectedCount = function () {
    return this.recordsByIndex.size;
  };

  global.GamepadInputManager = GamepadInputManager;
}(window));
