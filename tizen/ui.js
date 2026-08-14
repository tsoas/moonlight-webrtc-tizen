(function (global) {
  "use strict";

  function isFocusable(element) {
    return Boolean(element) && !element.disabled && element.getAttribute("aria-disabled") !== "true"
      && !element.hidden && element.offsetParent !== null;
  }

  function focusableElements(container) {
    if (!container) {
      return [];
    }
    return Array.prototype.filter.call(container.querySelectorAll(".focusable"), isFocusable);
  }

  function nextFocusableIndex(length, currentIndex, direction) {
    if (length <= 0) {
      return -1;
    }
    if (currentIndex < 0) {
      return 0;
    }
    return (currentIndex + direction + length) % length;
  }

  function nextNavigationZone(currentZone, direction, hasTopControls) {
    if (currentZone === "content" && direction === "up" && hasTopControls) {
      return "top";
    }
    if (currentZone === "top" && direction === "down") {
      return "content";
    }
    return currentZone;
  }

  function readableGatewayAddress(url) {
    try {
      return new URL(url).hostname || "-";
    } catch (error) {
      return "-";
    }
  }

  function restoreArtworkFallback(card, art, image) {
    if (image.parentNode === art) {
      art.removeChild(image);
    }
    art.classList.remove("has-artwork");
    art.classList.add("fallback");
    if (!art.firstChild || art.firstChild.nodeType !== 3) {
      art.insertBefore(
        document.createTextNode(String(card.dataset.applicationTitle || "?").slice(0, 1).toUpperCase()),
        art.firstChild
      );
    }
  }

  function createArtworkImage(card, art, artworkDataUrl) {
    const image = document.createElement("img");
    image.src = artworkDataUrl;
    image.alt = "";
    image.addEventListener("error", function () {
      restoreArtworkFallback(card, art, image);
    });
    return image;
  }

  function TizenUi(options) {
    this.options = options;
    this.currentView = "gateway";
    this.previousView = "gateway";
    this.selectedCategory = "video";
    this.gatewayScreen = document.getElementById("gateway-screen");
    this.applicationsScreen = document.getElementById("applications-screen");
    this.settingsScreen = document.getElementById("settings-screen");
    this.gatewayCard = document.getElementById("gateway-card");
    this.gatewayName = document.getElementById("gateway-name");
    this.gatewayAddress = document.getElementById("gateway-address");
    this.gatewayStatus = document.getElementById("gateway-card-status");
    this.gatewayStatusDot = document.getElementById("gateway-status-dot");
    this.applicationsGrid = document.getElementById("applications-grid");
    this.topTitle = document.getElementById("top-title");
    this.topBackButton = document.getElementById("top-back-button");
    this.settingsButton = document.getElementById("settings-button");
    this.hintPrimary = document.getElementById("hint-primary");
    this.hintBack = document.getElementById("hint-back");
    this.toastRegion = document.getElementById("toast-region");
    this.inputControllerName = document.getElementById("input-controller-name");
    this.connectionGatewayName = document.getElementById("connection-gateway-name");
    this.connectionGatewayAddress = document.getElementById("connection-gateway-address");
    this.connectionSunshineState = document.getElementById("connection-sunshine-state");
    this.connectionWebRtcState = document.getElementById("connection-webrtc-state");
    this.categoryButtons = Array.prototype.slice.call(
      document.querySelectorAll("[data-category]")
    );
    this.panels = Array.prototype.slice.call(
      document.querySelectorAll("[data-settings-panel]")
    );
    this.bindEvents();
  }

  TizenUi.prototype.bindEvents = function () {
    const ui = this;
    this.gatewayCard.addEventListener("click", function () {
      if (!ui.gatewayCard.disabled) {
        ui.showApplications();
      }
    });
    this.settingsButton.addEventListener("click", function () { ui.showSettings(); });
    this.topBackButton.addEventListener("click", function () { ui.goBack(); });
    this.categoryButtons.forEach(function (button) {
      button.addEventListener("click", function () { ui.selectSettingsCategory(button.dataset.category); });
    });
  };

  TizenUi.prototype.setGateway = function (gateway) {
    const name = gateway.name || "Moonlight Gateway";
    const state = gateway.state || "Connecting";
    this.gatewayName.textContent = name;
    this.gatewayAddress.textContent = gateway.address || "-";
    this.gatewayStatus.textContent = state;
    this.gatewayStatusDot.classList.toggle("is-connected", state === "Connected");
    this.gatewayStatusDot.classList.toggle("is-error", state === "Disconnected" || state === "Unavailable");
    this.gatewayCard.disabled = !gateway.selectable;
    this.connectionGatewayName.querySelector("strong").textContent = name;
    this.connectionGatewayAddress.querySelector("strong").textContent = gateway.address || "-";
    this.ensureGatewayFocus();
  };

  TizenUi.prototype.ensureGatewayFocus = function () {
    if (this.currentView !== "gateway" || !isFocusable(this.gatewayCard)) {
      return;
    }
    const active = document.activeElement;
    const contentElements = focusableElements(this.gatewayScreen);
    const topElements = this.topBarFocusableElements();
    if (contentElements.indexOf(active) < 0 && topElements.indexOf(active) < 0) {
      setTimeout(function () { document.getElementById("gateway-card").focus(); }, 0);
    }
  };

  TizenUi.prototype.setSunshineState = function (state) {
    this.connectionSunshineState.querySelector("strong").textContent = state;
  };

  TizenUi.prototype.setWebRtcState = function (state) {
    this.connectionWebRtcState.querySelector("strong").textContent = state;
  };

  TizenUi.prototype.setControllerName = function (name) {
    this.inputControllerName.querySelector("strong").textContent = name || "Not connected";
  };

  TizenUi.prototype.renderApplications = function (applications, selectedId) {
    const ui = this;
    this.applicationsGrid.innerHTML = "";
    applications.forEach(function (application) {
      const card = document.createElement("button");
      card.type = "button";
      card.className = "application-card focusable";
      card.dataset.applicationId = String(application.id);
      card.dataset.applicationTitle = String(application.title || "?");
      card.setAttribute("aria-label", "Launch " + String(application.title));
      const art = document.createElement("span");
      art.className = "application-art fallback";
      if (application.artworkDataUrl) {
        const image = createArtworkImage(card, art, application.artworkDataUrl);
        art.classList.remove("fallback");
        art.classList.add("has-artwork");
        art.appendChild(image);
      } else {
        art.textContent = String(application.title || "?").slice(0, 1).toUpperCase();
      }
      const running = document.createElement("span");
      running.className = "application-running";
      running.textContent = "\u25B6 Running";
      running.hidden = !application.running;
      art.appendChild(running);
      const name = document.createElement("span");
      name.className = "application-name";
      name.textContent = String(application.title || "Untitled application");
      card.appendChild(art);
      card.appendChild(name);
      card.addEventListener("click", function () {
        ui.options.onApplicationSelected(String(application.id));
      });
      ui.applicationsGrid.appendChild(card);
    });
    const selected = this.applicationsGrid.querySelector(
      '[data-application-id="' + String(selectedId) + '"]'
    );
    if (selected) {
      selected.dataset.defaultFocus = "true";
    }
    if (this.currentView === "applications") {
      this.focusDefault("applications");
    }
  };

  TizenUi.prototype.applicationCard = function (appId) {
    return Array.prototype.find.call(
      this.applicationsGrid.querySelectorAll("[data-application-id]"),
      function (card) { return card.dataset.applicationId === String(appId); }
    ) || null;
  };

  TizenUi.prototype.updateApplicationArtwork = function (appId, artworkDataUrl) {
    if (!artworkDataUrl) {
      return;
    }
    const card = this.applicationCard(appId);
    if (!card) {
      return;
    }
    const art = card.querySelector(".application-art");
    const existingImage = art.querySelector("img");
    if (existingImage) {
      existingImage.src = artworkDataUrl;
      return;
    }
    art.classList.remove("fallback");
    art.classList.add("has-artwork");
    const fallback = art.firstChild;
    if (fallback) {
      art.removeChild(fallback);
    }
    const image = createArtworkImage(card, art, artworkDataUrl);
    art.insertBefore(image, art.firstChild);
  };

  TizenUi.prototype.setRunningApplication = function (runningAppId) {
    Array.prototype.forEach.call(
      this.applicationsGrid.querySelectorAll("[data-application-id]"),
      function (card) {
        const running = card.dataset.applicationId === String(runningAppId || "");
        card.classList.toggle("is-running", running);
        const badge = card.querySelector(".application-running");
        if (badge) {
          badge.hidden = !running;
        }
      }
    );
  };

  TizenUi.prototype.showView = function (view) {
    this.currentView = view;
    this.gatewayScreen.hidden = view !== "gateway";
    this.applicationsScreen.hidden = view !== "applications";
    this.settingsScreen.hidden = view !== "settings";
    this.topBackButton.hidden = view === "gateway";
    this.settingsButton.hidden = view === "settings";
    this.topTitle.textContent = view === "settings"
      ? "Settings"
      : view === "applications" ? (this.gatewayName.textContent || "Gateway") : "Moonlight WebRTC Client";
    this.hintPrimary.textContent = view === "applications" ? "Launch" : "Select";
    this.hintBack.textContent = view === "gateway" ? "Exit" : "Back";
    this.focusDefault(view);
  };

  TizenUi.prototype.showGateway = function () { this.showView("gateway"); };

  TizenUi.prototype.showApplications = function () {
    if (this.gatewayCard.disabled) {
      return;
    }
    this.showView("applications");
  };

  TizenUi.prototype.showSettings = function () {
    this.previousView = this.currentView === "settings" ? this.previousView : this.currentView;
    this.showView("settings");
  };

  TizenUi.prototype.goBack = function () {
    if (this.currentView === "settings") {
      this.showView(this.previousView || "gateway");
      return true;
    }
    if (this.currentView === "applications") {
      this.showView("gateway");
      return true;
    }
    return false;
  };

  TizenUi.prototype.selectSettingsCategory = function (category) {
    this.selectedCategory = category;
    this.categoryButtons.forEach(function (button) {
      button.classList.toggle("is-selected", button.dataset.category === category);
    });
    this.panels.forEach(function (panel) {
      panel.hidden = panel.dataset.settingsPanel !== category;
    });
  };

  TizenUi.prototype.focusDefault = function (view) {
    const container = view === "gateway" ? this.gatewayScreen
      : view === "applications" ? this.applicationsScreen : this.settingsScreen;
    const preferred = view === "gateway" && isFocusable(this.gatewayCard)
      ? this.gatewayCard
      : container.querySelector("[data-default-focus='true']");
    const elements = focusableElements(container);
    const target = preferred && isFocusable(preferred) ? preferred : elements[0];
    if (target) {
      setTimeout(function () { target.focus(); }, 0);
    }
  };

  TizenUi.prototype.contentContainer = function () {
    return this.currentView === "gateway" ? this.gatewayScreen
      : this.currentView === "applications" ? this.applicationsScreen : this.settingsScreen;
  };

  TizenUi.prototype.topBarFocusableElements = function () {
    return Array.prototype.filter.call(
      [this.topBackButton, this.settingsButton],
      isFocusable
    );
  };

  TizenUi.prototype.settingsCategoryElements = function () {
    return this.categoryButtons.filter(isFocusable);
  };

  TizenUi.prototype.settingsPanelElements = function () {
    const panel = document.querySelector('[data-settings-panel="' + this.selectedCategory + '"]');
    return focusableElements(panel);
  };

  TizenUi.prototype.focusSettingsCategory = function (direction) {
    const categories = this.settingsCategoryElements();
    const active = document.activeElement;
    const index = categories.indexOf(active);
    if (index < 0) {
      return false;
    }
    if (direction === "up") {
      if (index === 0) {
        if (isFocusable(this.topBackButton)) {
          this.topBackButton.focus();
          return true;
        }
        return false;
      }
      this.selectSettingsCategory(categories[index - 1].dataset.category);
      categories[index - 1].focus();
      return true;
    }
    if (direction === "down") {
      if (index + 1 < categories.length) {
        this.selectSettingsCategory(categories[index + 1].dataset.category);
        categories[index + 1].focus();
        return true;
      }
      return false;
    }
    if (direction === "right") {
      this.selectSettingsCategory(active.dataset.category);
      const panelElements = this.settingsPanelElements();
      if (panelElements[0]) {
        panelElements[0].focus();
        return true;
      }
    }
    return false;
  };

  TizenUi.prototype.focusSettingsPanel = function (direction) {
    const elements = this.settingsPanelElements();
    const active = document.activeElement;
    const index = elements.indexOf(active);
    if (index < 0) {
      return false;
    }
    if (direction === "left") {
      const selectedButton = document.querySelector('[data-category="' + this.selectedCategory + '"]');
      if (selectedButton && isFocusable(selectedButton)) {
        selectedButton.focus();
        return true;
      }
      return false;
    }
    if (direction === "up" && index > 0) {
      elements[index - 1].focus();
      return true;
    }
    if (direction === "down" && index + 1 < elements.length) {
      elements[index + 1].focus();
      return true;
    }
    return false;
  };

  TizenUi.prototype.focusByDirection = function (direction) {
    const container = this.contentContainer();
    const elements = focusableElements(container);
    const active = document.activeElement;
    const topElements = this.topBarFocusableElements();
    const activeInTopBar = topElements.indexOf(active) >= 0;

    if (this.currentView === "settings") {
      if (activeInTopBar) {
        if (direction === "down") {
          this.focusDefault("settings");
          return true;
        }
        if (direction === "left" || direction === "right") {
          const topIndex = topElements.indexOf(active);
          const nextTop = nextFocusableIndex(topElements.length, topIndex, direction === "right" ? 1 : -1);
          topElements[nextTop].focus();
          return true;
        }
        return false;
      }
      if (this.settingsCategoryElements().indexOf(active) >= 0) {
        return this.focusSettingsCategory(direction);
      }
      if (this.settingsPanelElements().indexOf(active) >= 0) {
        return this.focusSettingsPanel(direction);
      }
      this.focusDefault("settings");
      return true;
    }
    const zone = activeInTopBar ? "top" : "content";
    const nextZone = nextNavigationZone(zone, direction, topElements.length > 0);

    if (nextZone === "top" && zone !== "top") {
      const topTarget = this.settingsButton.hidden ? topElements[0] : this.settingsButton;
      if (topTarget) {
        topTarget.focus();
        return true;
      }
    }
    if (nextZone === "content" && zone === "top") {
      this.focusDefault(this.currentView);
      return elements.length > 0;
    }

    if (activeInTopBar && (direction === "left" || direction === "right")) {
      if (topElements.length === 0) {
        return false;
      }
      const index = topElements.indexOf(active);
      const next = nextFocusableIndex(topElements.length, index, direction === "right" ? 1 : -1);
      topElements[next].focus();
      return true;
    }
    if (elements.length === 0) {
      return false;
    }
    if (direction === "up" || direction === "down") {
      const index = elements.indexOf(active);
      const next = nextFocusableIndex(elements.length, index, direction === "down" ? 1 : -1);
      elements[next].focus();
      return true;
    }
    if (this.currentView === "applications" && (direction === "left" || direction === "right")) {
      const index = elements.indexOf(active);
      const next = nextFocusableIndex(elements.length, index, direction === "right" ? 1 : -1);
      elements[next].focus();
      return true;
    }
    return false;
  };

  TizenUi.prototype.showToast = function (title, message, isError) {
    const toast = document.createElement("div");
    toast.className = "toast" + (isError ? " is-error" : "");
    const content = document.createElement("div");
    content.className = "toast-content";
    const heading = document.createElement("span");
    heading.className = "toast-title";
    heading.textContent = title;
    content.appendChild(heading);
    if (message) {
      const detail = document.createElement("span");
      detail.className = "toast-message";
      detail.textContent = message;
      content.appendChild(detail);
    }
    toast.appendChild(content);
    this.toastRegion.appendChild(toast);
    setTimeout(function () { toast.remove(); }, 3600);
  };

  global.TizenUi = {
    create: function (options) { return new TizenUi(options); },
    testing: {
      isFocusable: isFocusable,
      nextFocusableIndex: nextFocusableIndex,
      nextNavigationZone: nextNavigationZone,
      readableGatewayAddress: readableGatewayAddress,
    },
  };
}(window));
