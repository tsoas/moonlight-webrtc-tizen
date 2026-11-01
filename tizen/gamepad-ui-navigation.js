(function (global) {
  "use strict";

  const POLL_INTERVAL_MS = 1000 / 60;
  // While the stream owns the controller this poller dispatches nothing; it only keeps
  // held-button state fresh so returning to a menu does not fire a stale edge. Backing
  // off eases the pressure on video decode, but only a little: this is also the interval
  // the first press after leaving gameplay waits for, and a quarter of a second of that
  // reads as a frozen menu.
  const GAMEPLAY_POLL_INTERVAL_MS = 50;
  const INITIAL_REPEAT_DELAY_MS = 350;
  const REPEAT_INTERVAL_MS = 120;
  const STICK_THRESHOLD = 0.55;

  function pressed(gamepad, index) {
    const button = gamepad && gamepad.buttons && gamepad.buttons[index];
    return Boolean(button && (button.pressed || Number(button.value) > 0.5));
  }

  function axis(gamepad, index) {
    return gamepad && gamepad.axes && Number.isFinite(gamepad.axes[index])
      ? gamepad.axes[index] : 0;
  }

  function directionFor(gamepad) {
    if (pressed(gamepad, 12) || axis(gamepad, 1) <= -STICK_THRESHOLD) { return "up"; }
    if (pressed(gamepad, 13) || axis(gamepad, 1) >= STICK_THRESHOLD) { return "down"; }
    if (pressed(gamepad, 14) || axis(gamepad, 0) <= -STICK_THRESHOLD) { return "left"; }
    if (pressed(gamepad, 15) || axis(gamepad, 0) >= STICK_THRESHOLD) { return "right"; }
    return null;
  }

  function currentGamepads() {
    return navigator.getGamepads ? (navigator.getGamepads() || []) : [];
  }

  function GamepadUiNavigation(options) {
    this.options = options;
    this.states = new Map();
    this.timer = null;
  }

  GamepadUiNavigation.prototype.start = function () {
    if (this.timer === null) {
      this.schedule();
    }
  };

  GamepadUiNavigation.prototype.stop = function () {
    if (this.timer !== null) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    this.states.clear();
  };

  GamepadUiNavigation.prototype.schedule = function (route) {
    const navigation = this;
    const interval = route === "gameplay" ? GAMEPLAY_POLL_INTERVAL_MS : POLL_INTERVAL_MS;
    this.timer = setTimeout(function () { navigation.poll(); }, interval);
  };

  GamepadUiNavigation.prototype.poll = function () {
    this.timer = null;
    let route = null;
    // navigate/activate/back run application code. If one of them throws, the poller has
    // to keep its own timer alive anyway; otherwise a single bad transition leaves the
    // controller permanently unable to drive the interface.
    try {
      if (!document.hidden) {
        // Read once per poll rather than once per controller.
        route = this.options.route();
        const gamepads = currentGamepads();
        const seen = new Set();
        for (let index = 0; index < gamepads.length; index += 1) {
          const gamepad = gamepads[index];
          if (!gamepad) { continue; }
          seen.add(gamepad.index);
          this.handleGamepad(gamepad, performance.now(), route);
        }
        this.states.forEach(function (_state, index) {
          if (!seen.has(index)) { this.states.delete(index); }
        }, this);
      }
    } catch (error) {
      if (typeof this.options.log === "function") {
        this.options.log("Gamepad UI navigation poll failed: " + String(error));
      }
    } finally {
      this.schedule(route);
    }
  };

  GamepadUiNavigation.prototype.handleGamepad = function (gamepad, now, currentRoute) {
    const route = typeof currentRoute === "string" ? currentRoute : this.options.route();
    let state = this.states.get(gamepad.index);
    if (!state) {
      state = {
        route: route,
        direction: null,
        repeatAt: 0,
        aHeld: false,
        bHeld: false,
        menuHeld: false,
      };
      this.states.set(gamepad.index, state);
    }
    if (state.route !== route) {
      state.route = route;
      state.direction = null;
      state.repeatAt = 0;
    }
    const aPressed = pressed(gamepad, 0);
    const bPressed = pressed(gamepad, 1);
    const menuPressed = pressed(gamepad, 9);
    if (route === "gameplay") {
      state.aHeld = aPressed;
      state.bHeld = bPressed;
      state.menuHeld = menuPressed;
      return;
    }
    const direction = directionFor(gamepad);
    if (!direction) {
      state.direction = null;
      state.repeatAt = 0;
    } else if (direction !== state.direction) {
      this.options.navigate(direction);
      state.direction = direction;
      state.repeatAt = now + INITIAL_REPEAT_DELAY_MS;
    } else if (now >= state.repeatAt) {
      this.options.navigate(direction);
      state.repeatAt = now + REPEAT_INTERVAL_MS;
    }
    if (aPressed && !state.aHeld) { this.options.activate(); }
    if (bPressed && !state.bHeld) { this.options.back(); }
    if (menuPressed && !state.menuHeld && typeof this.options.menu === "function") {
      this.options.menu();
    }
    state.aHeld = aPressed;
    state.bHeld = bPressed;
    state.menuHeld = menuPressed;
  };

  global.GamepadUiNavigation = {
    create: function (options) { return new GamepadUiNavigation(options); },
    testing: { directionFor: directionFor, pressed: pressed, STICK_THRESHOLD: STICK_THRESHOLD },
  };
}(window));
