const SIGNALING_URL = "ws://192.168.0.69:8000";
const GATEWAY_PROTOCOL_VERSION = 1;
const GAMEPAD_PROTOCOL_VERSION = 1;
const GAMEPAD_POLL_INTERVAL_MS = 1000 / 120;
const GAMEPAD_KEEPALIVE_INTERVAL_MS = 250;

const homeScreen = document.getElementById("home-screen");
const streamingScreen = document.getElementById("streaming-screen");
const videoElement = document.getElementById("video");
const diagnosticsElement = document.getElementById("diagnostics");
const logElement = document.getElementById("log");
const playbackPromptElement = document.getElementById("playback-prompt");
const streamOverlay = document.getElementById("stream-overlay");
const streamMenu = document.getElementById("stream-menu");
const appSelect = document.getElementById("app-select");
const resolutionSelect = document.getElementById("resolution-select");
const codecSelect = document.getElementById("codec-select");
const bitrateSelect = document.getElementById("bitrate-select");
const playButton = document.getElementById("play-button");
const continueButton = document.getElementById("continue-button");
const diagnosticsButton = document.getElementById("diagnostics-button");
const stopButton = document.getElementById("stop-button");
const homeMessage = document.getElementById("home-message");
const gatewayStateElement = document.getElementById("gateway-state");
const sunshineStateElement = document.getElementById("sunshine-state");
const sessionStateElement = document.getElementById("session-state");
const connectionStateElement = document.getElementById("connection-state");
const iceStateElement = document.getElementById("ice-state");

const overlay = {
  app: document.getElementById("overlay-app"),
  resolution: document.getElementById("overlay-resolution"),
  fps: document.getElementById("overlay-fps"),
  codec: document.getElementById("overlay-codec"),
  bitrate: document.getElementById("overlay-bitrate"),
  hdr: document.getElementById("overlay-hdr"),
  gamepad: document.getElementById("overlay-gamepad"),
  connection: document.getElementById("overlay-connection"),
};

const trackCounts = {
  remote: document.getElementById("remote-tracks"),
  video: document.getElementById("video-tracks"),
  audio: document.getElementById("audio-tracks"),
};

const statistics = {
  requestedResolution: document.getElementById("requested-resolution"),
  requestedCodec: document.getElementById("requested-codec"),
  requestedBitrate: document.getElementById("requested-bitrate"),
  actualResolution: document.getElementById("actual-resolution"),
  receivedCodec: document.getElementById("received-codec"),
  video: {
    framesReceived: document.getElementById("frames-received"),
    framesDecoded: document.getElementById("frames-decoded"),
    decodedFps: document.getElementById("decoded-fps"),
    framesDropped: document.getElementById("frames-dropped"),
    packetsReceived: document.getElementById("packets-received"),
    packetsLost: document.getElementById("packets-lost"),
    bytesReceived: document.getElementById("bytes-received"),
  },
  audio: {
    packetsReceived: document.getElementById("audio-packets-received"),
    packetsLost: document.getElementById("audio-packets-lost"),
    bytesReceived: document.getElementById("audio-bytes-received"),
    jitter: document.getElementById("audio-jitter"),
  },
};

const gamepadDiagnostics = {
  state: document.getElementById("gamepad-state"),
  id: document.getElementById("gamepad-id"),
  mapping: document.getElementById("gamepad-mapping"),
  controlChannel: document.getElementById("control-channel-state"),
  inputChannel: document.getElementById("input-channel-state"),
  sendRate: document.getElementById("input-send-rate"),
  lastSequence: document.getElementById("last-input-sequence"),
};

const GAMEPAD_BUTTONS = {
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

const remoteStream = new MediaStream();
videoElement.srcObject = remoteStream;
videoElement.muted = false;

let socket = null;
let reconnectTimer = null;
let peerConnection = null;
let currentSessionId = 0;
let pendingRemoteCandidates = [];
let gatewayConnected = false;
let sunshineReady = false;
let appsLoaded = false;
let sessionState = "idle";
let selectedSession = null;
let overlayTimer = null;
let statisticsErrorReported = false;
let previousDecodedSample = null;
let playbackErrorReported = false;
let controlDataChannel = null;
let gamepadDataChannel = null;
let activeGamepadIndex = null;
let gamepadAnnounced = false;
let gamepadPollTimer = null;
let nextGamepadPollTime = 0;
let gamepadSequence = 0;
let lastGamepadFingerprint = "";
let lastFullStateSendTime = 0;
let inputRateWindowStart = performance.now();
let inputMessagesInWindow = 0;
let videoModes = [];
let codecSelectionWasIntentional = false;
let updatingCodecOptions = false;
let lastStatisticsConsoleTime = 0;

function log(message) {
  console.log(message);
  logElement.textContent += message + "\n";
  const lines = logElement.textContent.split("\n");
  if (lines.length > 80) {
    logElement.textContent = lines.slice(lines.length - 80).join("\n");
  }
  logElement.scrollTop = logElement.scrollHeight;
}

function errorMessage(error) {
  if (!error) {
    return "Unknown error";
  }
  return error.message || error.name || String(error);
}

function setHomeMessage(message, isError) {
  homeMessage.textContent = message;
  homeMessage.classList.toggle("error", Boolean(isError));
}

function reportError(context, error) {
  const message = context + ": " + errorMessage(error);
  diagnosticsElement.classList.add("has-error");
  setHomeMessage(message, true);
  console.error(message, error);
  log("ERROR: " + message);
}

function updatePlayAvailability() {
  playButton.disabled = !gatewayConnected || !sunshineReady || !appsLoaded
    || sessionState !== "idle";
}

function sendGatewayMessage(message) {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    throw new Error("Gateway WebSocket is not connected");
  }
  message.version = GATEWAY_PROTOCOL_VERSION;
  socket.send(JSON.stringify(message));
}

function requestApplications() {
  try {
    sendGatewayMessage({ type: "get-apps" });
  } catch (error) {
    reportError("Unable to request Sunshine applications", error);
  }
}

function connectGateway() {
  if (reconnectTimer !== null) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }

  gatewayStateElement.textContent = "Connecting";
  setHomeMessage("Connecting to Moonlight WebRTC Gateway...", false);
  try {
    socket = new WebSocket(SIGNALING_URL);
  } catch (error) {
    reportError("WebSocket creation failed", error);
    reconnectTimer = setTimeout(connectGateway, 2000);
    return;
  }

  socket.addEventListener("open", function () {
    gatewayConnected = true;
    gatewayStateElement.textContent = "Connected";
    setHomeMessage("Connected. Loading Sunshine applications...", false);
    updatePlayAvailability();
    requestApplications();
    log("Gateway WebSocket connected");
  });

  socket.addEventListener("close", function () {
    gatewayConnected = false;
    sunshineReady = false;
    gatewayStateElement.textContent = "Disconnected";
    sunshineStateElement.textContent = "Unavailable";
    sessionState = "idle";
    sessionStateElement.textContent = "Idle";
    closePeerConnection();
    showHome();
    setHomeMessage("Gateway disconnected. Reconnecting...", true);
    updatePlayAvailability();
    log("Gateway WebSocket disconnected");
    reconnectTimer = setTimeout(connectGateway, 2000);
  });

  socket.addEventListener("error", function (event) {
    reportError("WebSocket error", event.error || event);
  });

  socket.addEventListener("message", function (event) {
    handleGatewayMessage(event.data);
  });
}

async function handleGatewayMessage(text) {
  let message;
  try {
    message = JSON.parse(text);
  } catch (error) {
    reportError("Invalid Gateway JSON", error);
    return;
  }
  if (message.version !== GATEWAY_PROTOCOL_VERSION || typeof message.type !== "string") {
    reportError("Unsupported Gateway message", new Error("Protocol version 1 is required"));
    return;
  }

  if (message.type === "gateway-status") {
    handleGatewayStatus(message);
  } else if (message.type === "capabilities") {
    applyCapabilities(message);
  } else if (message.type === "apps") {
    applyApplications(message.apps);
  } else if (message.type === "session-status") {
    handleSessionStatus(message);
  } else if (message.type === "offer") {
    await handleOffer(message);
  } else if (message.type === "candidate") {
    await handleRemoteCandidate(message);
  } else if (message.type === "error") {
    handleGatewayError(message);
  } else {
    log("Ignored Gateway message: " + message.type);
  }
}

function handleGatewayStatus(message) {
  sunshineReady = Boolean(message.sunshineDetected && message.sunshinePaired);
  if (!message.sunshineDetected) {
    sunshineStateElement.textContent = "Not detected";
  } else if (!message.sunshinePaired) {
    sunshineStateElement.textContent = "Connected / Not paired";
  } else {
    sunshineStateElement.textContent = "Connected / Paired";
  }
  updatePlayAvailability();
}

function replaceSelectOptions(select, values, formatter) {
  const previousValue = select.value;
  select.innerHTML = "";
  values.forEach(function (value) {
    const option = document.createElement("option");
    option.value = formatter.value(value);
    option.textContent = formatter.label(value);
    select.appendChild(option);
  });
  const previousStillExists = Array.prototype.some.call(select.options, function (option) {
    return option.value === previousValue;
  });
  if (previousStillExists) {
    select.value = previousValue;
  }
}

function applyCapabilities(message) {
  videoModes = Array.isArray(message.videoModes) ? message.videoModes : [];
  if (videoModes.length > 0) {
    replaceSelectOptions(resolutionSelect, videoModes, {
      value: function (mode) {
        return String(mode.width) + "x" + String(mode.height);
      },
      label: function (mode) {
        const base = String(mode.width) + " × " + String(mode.height);
        if (mode.experimental) {
          return base + " — Experimental";
        }
        return mode.width === 3840 && mode.height === 2160 ? base + " — 4K" : base;
      },
    });
  }
  if (Array.isArray(message.bitratesKbps) && message.bitratesKbps.length > 0) {
    replaceSelectOptions(bitrateSelect, message.bitratesKbps, {
      value: function (bitrate) { return String(bitrate); },
      label: function (bitrate) { return String(bitrate / 1000) + " Mbps"; },
    });
  }
  resolutionSelect.value = "1280x720";
  codecSelectionWasIntentional = false;
  applySelectedVideoMode();
}

function selectedVideoMode() {
  return videoModes.find(function (mode) {
    return String(mode.width) + "x" + String(mode.height) === resolutionSelect.value;
  }) || null;
}

function codecDisplayName(codec) {
  return codec === "hevc" ? "HEVC (H.265)" : "H.264";
}

function applySelectedVideoMode() {
  const mode = selectedVideoMode();
  if (!mode || !Array.isArray(mode.codecs) || mode.codecs.length === 0) {
    return;
  }
  const previousCodec = codecSelect.value;
  const keepIntentionalCodec = codecSelectionWasIntentional
    && mode.codecs.indexOf(previousCodec) >= 0;
  updatingCodecOptions = true;
  replaceSelectOptions(codecSelect, mode.codecs, {
    value: function (codec) { return codec; },
    label: codecDisplayName,
  });
  codecSelect.value = keepIntentionalCodec ? previousCodec : mode.defaultCodec;
  updatingCodecOptions = false;
  bitrateSelect.value = String(mode.defaultBitrateKbps);
  if (mode.experimental) {
    setHomeMessage("2560 × 1440 is experimental on Samsung Tizen.", false);
  } else if (appsLoaded) {
    setHomeMessage("Choose settings, then press PLAY.", false);
  }
}

function applyApplications(applications) {
  appSelect.innerHTML = "";
  if (!Array.isArray(applications) || applications.length === 0) {
    appsLoaded = false;
    appSelect.disabled = true;
    const option = document.createElement("option");
    option.textContent = "No Sunshine applications";
    appSelect.appendChild(option);
    setHomeMessage("Sunshine returned no applications.", true);
    updatePlayAvailability();
    return;
  }

  applications.forEach(function (application) {
    const option = document.createElement("option");
    option.value = String(application.id);
    option.textContent = String(application.title);
    appSelect.appendChild(option);
  });
  const desktopIndex = applications.findIndex(function (application) {
    return String(application.title).toLowerCase() === "desktop";
  });
  appSelect.selectedIndex = desktopIndex >= 0 ? desktopIndex : 0;
  appsLoaded = true;
  appSelect.disabled = false;
  setHomeMessage("Choose settings, then press PLAY.", false);
  updatePlayAvailability();
  focusFirstHomeControl();
}

function handleGatewayError(message) {
  const detail = message.message || "Gateway request failed";
  reportError(message.requestType || "Gateway", new Error(detail));
  if (sessionState !== "streaming") {
    sessionState = "idle";
    sessionStateElement.textContent = "Idle";
    updatePlayAvailability();
  }
}

function readableState(state) {
  return String(state || "unknown").replace(/-/g, " ");
}

function handleSessionStatus(message) {
  sessionState = message.state;
  sessionStateElement.textContent = readableState(message.state);
  if (typeof message.sessionId === "number") {
    currentSessionId = message.sessionId;
  }
  if (message.video) {
    selectedSession = {
      appTitle: selectedSession ? selectedSession.appTitle : "Desktop",
      width: message.video.width,
      height: message.video.height,
      fps: message.video.fps,
      codec: message.video.codec,
      bitrateKbps: message.video.bitrateKbps,
      hdr: message.video.hdr,
    };
    updateStreamOverlay();
  }

  overlay.connection.textContent = readableState(message.state);
  if (message.state === "streaming") {
    showStreaming();
    resumeGamepadInput();
    setHomeMessage("Streaming", false);
  } else if (message.state === "idle") {
    closePeerConnection();
    currentSessionId = 0;
    showHome();
    setHomeMessage("Session stopped. Choose settings to start again.", false);
  } else if (message.state === "error") {
    setHomeMessage(message.message || "Session failed", true);
    if (currentSessionId) {
      try {
        sendGatewayMessage({ type: "stop-session" });
      } catch (error) {
        reportError("Unable to clean up failed session", error);
      }
    }
  } else {
    setHomeMessage("Session: " + readableState(message.state), false);
  }
  updatePlayAvailability();
  log("Session status: " + message.state);
}

function selectedSettings() {
  const dimensions = resolutionSelect.value.split("x");
  return {
    appTitle: appSelect.options[appSelect.selectedIndex].textContent,
    width: Number(dimensions[0]),
    height: Number(dimensions[1]),
    fps: 60,
    codec: codecSelect.value,
    bitrateKbps: Number(bitrateSelect.value),
    hdr: false,
  };
}

function startSelectedSession() {
  if (playButton.disabled) {
    return;
  }
  selectedSession = selectedSettings();
  sessionState = "starting";
  updatePlayAvailability();
  updateStreamOverlay();
  try {
    sendGatewayMessage({
      type: "start-session",
      appId: appSelect.value,
      video: {
        width: selectedSession.width,
        height: selectedSession.height,
        fps: selectedSession.fps,
        codec: selectedSession.codec,
        bitrateKbps: selectedSession.bitrateKbps,
        hdr: selectedSession.hdr,
      },
      audio: { channels: 2 },
    });
    setHomeMessage("Starting " + selectedSession.appTitle + "...", false);
  } catch (error) {
    sessionState = "idle";
    reportError("Unable to start session", error);
    updatePlayAvailability();
  }
}

function stopCurrentSession() {
  hideStreamMenu();
  if (!currentSessionId) {
    showHome();
    return;
  }
  try {
    sendGatewayMessage({ type: "stop-session" });
    sessionState = "stopping";
    overlay.connection.textContent = "Stopping";
    sessionStateElement.textContent = "Stopping";
  } catch (error) {
    reportError("Unable to stop session", error);
  }
}

function createPeerConnection(sessionId) {
  closePeerConnection();
  currentSessionId = sessionId;
  pendingRemoteCandidates = [];
  previousDecodedSample = null;
  statisticsErrorReported = false;

  try {
    peerConnection = new RTCPeerConnection({ iceServers: [] });
  } catch (error) {
    reportError("RTCPeerConnection creation failed", error);
    throw error;
  }

  peerConnection.ondatachannel = function (event) {
    const channel = event.channel;
    if (channel.label === "control") {
      configureControlDataChannel(channel);
    } else if (channel.label === "gamepad") {
      configureGamepadDataChannel(channel);
    } else {
      log("Ignoring unexpected DataChannel: " + channel.label);
      channel.close();
    }
  };

  peerConnection.onicecandidate = function (event) {
    if (!event.candidate || !currentSessionId) {
      return;
    }
    try {
      sendGatewayMessage({
        type: "candidate",
        sessionId: currentSessionId,
        candidate: event.candidate.candidate,
        mid: event.candidate.sdpMid,
      });
    } catch (error) {
      reportError("Sending ICE candidate failed", error);
    }
  };

  peerConnection.onconnectionstatechange = function () {
    if (!peerConnection) {
      return;
    }
    const state = peerConnection.connectionState;
    connectionStateElement.textContent = state;
    overlay.connection.textContent = state;
    log("PeerConnection state: " + state);
    if (state === "disconnected" || state === "failed" || state === "closed") {
      suspendGamepadInput(true);
    }
  };

  peerConnection.oniceconnectionstatechange = function () {
    if (!peerConnection) {
      return;
    }
    iceStateElement.textContent = peerConnection.iceConnectionState;
    log("ICE state: " + peerConnection.iceConnectionState);
  };

  peerConnection.ontrack = function (event) {
    event.track.enabled = !document.hidden;
    const alreadyAdded = remoteStream.getTracks().some(function (track) {
      return track.id === event.track.id;
    });
    if (!alreadyAdded) {
      remoteStream.addTrack(event.track);
    }
    updateTrackCounts();
    event.track.addEventListener("ended", function () {
      const currentTrack = remoteStream.getTrackById(event.track.id);
      if (currentTrack) {
        remoteStream.removeTrack(currentTrack);
        updateTrackCounts();
      }
    });
    startPlayback();
  };
}

async function applyRemoteCandidate(candidate) {
  if (!peerConnection) {
    return false;
  }
  try {
    await peerConnection.addIceCandidate(candidate);
    return true;
  } catch (error) {
    reportError("addIceCandidate failed", error);
    return false;
  }
}

async function handleRemoteCandidate(message) {
  if (!peerConnection || message.sessionId !== currentSessionId) {
    return;
  }
  const candidate = {
    candidate: message.candidate,
    sdpMid: message.mid,
  };
  if (!peerConnection.remoteDescription) {
    pendingRemoteCandidates.push(candidate);
    return;
  }
  await applyRemoteCandidate(candidate);
}

async function handleOffer(message) {
  if (typeof message.sessionId !== "number" || typeof message.sdp !== "string") {
    reportError("Invalid SDP offer", new Error("Missing sessionId or SDP"));
    return;
  }
  if (!peerConnection || currentSessionId !== message.sessionId) {
    createPeerConnection(message.sessionId);
  }

  try {
    await peerConnection.setRemoteDescription({ type: "offer", sdp: message.sdp });
    while (pendingRemoteCandidates.length > 0) {
      await applyRemoteCandidate(pendingRemoteCandidates.shift());
    }
    const answer = await peerConnection.createAnswer();
    await peerConnection.setLocalDescription(answer);
    sendGatewayMessage({
      type: "answer",
      sessionId: message.sessionId,
      sdp: peerConnection.localDescription.sdp,
    });
    log("WebRTC answer sent for session " + String(message.sessionId));
  } catch (error) {
    reportError("WebRTC offer handling failed", error);
  }
}

function closePeerConnection() {
  suspendGamepadInput(true);
  controlDataChannel = null;
  gamepadDataChannel = null;
  gamepadAnnounced = false;
  updateDataChannelState(gamepadDiagnostics.controlChannel, null);
  updateDataChannelState(gamepadDiagnostics.inputChannel, null);
  if (peerConnection) {
    peerConnection.ondatachannel = null;
    peerConnection.ontrack = null;
    peerConnection.onicecandidate = null;
    peerConnection.onconnectionstatechange = null;
    peerConnection.oniceconnectionstatechange = null;
    peerConnection.close();
    peerConnection = null;
  }
  remoteStream.getTracks().forEach(function (track) {
    remoteStream.removeTrack(track);
    track.stop();
  });
  updateTrackCounts();
  videoElement.hidden = true;
  playbackPromptElement.hidden = true;
  connectionStateElement.textContent = "closed";
  iceStateElement.textContent = "closed";
}

async function startPlayback() {
  videoElement.muted = false;
  try {
    await videoElement.play();
    playbackPromptElement.hidden = true;
  } catch (error) {
    playbackPromptElement.hidden = false;
    if (!playbackErrorReported) {
      playbackErrorReported = true;
      log("Audio playback requires pressing OK");
    }
  }
}

function updateTrackCounts() {
  trackCounts.remote.textContent = remoteStream.getTracks().length;
  trackCounts.video.textContent = remoteStream.getVideoTracks().length;
  trackCounts.audio.textContent = remoteStream.getAudioTracks().length;
}

function updateStreamOverlay() {
  if (!selectedSession) {
    return;
  }
  overlay.app.textContent = selectedSession.appTitle;
  overlay.resolution.textContent = String(selectedSession.width) + " × "
    + String(selectedSession.height);
  overlay.fps.textContent = String(selectedSession.fps);
  overlay.codec.textContent = codecDisplayName(selectedSession.codec);
  overlay.bitrate.textContent = String(selectedSession.bitrateKbps / 1000) + " Mbps";
  overlay.hdr.textContent = selectedSession.hdr ? "On" : "Off";
}

function showOverlayTemporarily() {
  if (overlayTimer !== null) {
    clearTimeout(overlayTimer);
  }
  streamOverlay.classList.remove("faded");
  overlayTimer = setTimeout(function () {
    if (streamMenu.hidden) {
      streamOverlay.classList.add("faded");
    }
  }, 5000);
}

function showStreaming() {
  homeScreen.hidden = true;
  streamingScreen.hidden = false;
  videoElement.hidden = false;
  updateStreamOverlay();
  showOverlayTemporarily();
  startPlayback();
}

function showHome() {
  homeScreen.hidden = false;
  streamingScreen.hidden = true;
  streamMenu.hidden = true;
  diagnosticsElement.hidden = true;
  diagnosticsButton.textContent = "SHOW DIAGNOSTICS";
  updatePlayAvailability();
  focusFirstHomeControl();
}

function showStreamMenu() {
  streamMenu.hidden = false;
  streamOverlay.classList.remove("faded");
  continueButton.focus();
}

function hideStreamMenu() {
  streamMenu.hidden = true;
  showOverlayTemporarily();
}

function toggleDiagnostics() {
  diagnosticsElement.hidden = !diagnosticsElement.hidden;
  diagnosticsButton.textContent = diagnosticsElement.hidden
    ? "SHOW DIAGNOSTICS"
    : "HIDE DIAGNOSTICS";
}

function focusableElements(container) {
  return Array.prototype.filter.call(
    container.querySelectorAll(".focusable"),
    function (element) { return !element.disabled && !element.hidden; }
  );
}

function focusFirstHomeControl() {
  if (homeScreen.hidden) {
    return;
  }
  const elements = focusableElements(homeScreen);
  if (elements.length > 0) {
    elements[0].focus();
  }
}

function moveFocus(container, direction) {
  const elements = focusableElements(container);
  if (elements.length === 0) {
    return;
  }
  const currentIndex = elements.indexOf(document.activeElement);
  const nextIndex = currentIndex < 0
    ? 0
    : (currentIndex + direction + elements.length) % elements.length;
  elements[nextIndex].focus();
}

function changeSelect(select, direction) {
  if (!select || select.tagName !== "SELECT" || select.disabled) {
    return;
  }
  const count = select.options.length;
  if (count === 0) {
    return;
  }
  select.selectedIndex = (select.selectedIndex + direction + count) % count;
  select.dispatchEvent(new Event("change"));
}

function exitApplication() {
  try {
    if (window.tizen && tizen.application) {
      tizen.application.getCurrentApplication().exit();
    }
  } catch (error) {
    log("Application exit unavailable: " + errorMessage(error));
  }
}

document.addEventListener("keydown", function (event) {
  const key = event.key;
  const keyCode = event.keyCode;
  const isBack = key === "Backspace" || keyCode === 10009;
  const isEnter = key === "Enter" || keyCode === 13;
  const isUp = key === "ArrowUp" || keyCode === 38;
  const isDown = key === "ArrowDown" || keyCode === 40;
  const isLeft = key === "ArrowLeft" || keyCode === 37;
  const isRight = key === "ArrowRight" || keyCode === 39;

  if (!streamingScreen.hidden) {
    if (isBack) {
      event.preventDefault();
      if (streamMenu.hidden) {
        showStreamMenu();
      } else {
        hideStreamMenu();
      }
      return;
    }
    if (streamMenu.hidden) {
      if (isEnter) {
        event.preventDefault();
        showOverlayTemporarily();
        startPlayback();
      }
      return;
    }
    if (isUp || isDown) {
      event.preventDefault();
      moveFocus(streamMenu, isDown ? 1 : -1);
    } else if (isEnter) {
      event.preventDefault();
      document.activeElement.click();
    }
    return;
  }

  if (isBack) {
    event.preventDefault();
    exitApplication();
  } else if (isUp || isDown) {
    event.preventDefault();
    moveFocus(homeScreen, isDown ? 1 : -1);
  } else if (isLeft || isRight) {
    if (document.activeElement.tagName === "SELECT") {
      event.preventDefault();
      changeSelect(document.activeElement, isRight ? 1 : -1);
    }
  } else if (isEnter && document.activeElement.tagName !== "SELECT") {
    event.preventDefault();
    document.activeElement.click();
  }
});

playButton.addEventListener("click", startSelectedSession);
continueButton.addEventListener("click", hideStreamMenu);
diagnosticsButton.addEventListener("click", toggleDiagnostics);
stopButton.addEventListener("click", stopCurrentSession);
resolutionSelect.addEventListener("change", function () {
  applySelectedVideoMode();
});
codecSelect.addEventListener("change", function () {
  if (!updatingCodecOptions) {
    codecSelectionWasIntentional = true;
  }
});

function updateRemoteTracksForVisibility() {
  const enabled = !document.hidden;
  remoteStream.getTracks().forEach(function (track) {
    track.enabled = enabled;
  });
  if (enabled && sessionState === "streaming") {
    resumeGamepadInput();
    startPlayback();
  } else if (!enabled) {
    suspendGamepadInput(true);
  }
}

document.addEventListener("visibilitychange", updateRemoteTracksForVisibility);

function dataChannelIsOpen(channel) {
  return channel && channel.readyState === "open";
}

function updateDataChannelState(element, channel) {
  element.textContent = channel ? channel.readyState : "unavailable";
}

function currentGamepads() {
  if (!navigator.getGamepads) {
    return [];
  }
  return navigator.getGamepads() || [];
}

function activeGamepad() {
  const gamepads = currentGamepads();
  if (activeGamepadIndex !== null && gamepads[activeGamepadIndex]) {
    return gamepads[activeGamepadIndex];
  }
  for (let index = 0; index < gamepads.length; index += 1) {
    if (gamepads[index]) {
      activeGamepadIndex = gamepads[index].index;
      return gamepads[index];
    }
  }
  return null;
}

function gamepadButton(gamepad, index) {
  return gamepad.buttons && gamepad.buttons[index]
    ? gamepad.buttons[index]
    : { pressed: false, value: 0 };
}

function gamepadButtonPressed(gamepad, index) {
  const button = gamepadButton(gamepad, index);
  return button.pressed || Number(button.value) > 0.5;
}

function gamepadAxis(gamepad, index) {
  if (!gamepad.axes || typeof gamepad.axes[index] !== "number") {
    return 0;
  }
  return gamepad.axes[index];
}

function standardButtonFlags(gamepad) {
  let flags = 0;
  const mappings = [
    [0, GAMEPAD_BUTTONS.A],
    [1, GAMEPAD_BUTTONS.B],
    [2, GAMEPAD_BUTTONS.X],
    [3, GAMEPAD_BUTTONS.Y],
    [4, GAMEPAD_BUTTONS.LB],
    [5, GAMEPAD_BUTTONS.RB],
    [8, GAMEPAD_BUTTONS.BACK],
    [9, GAMEPAD_BUTTONS.START],
    [10, GAMEPAD_BUTTONS.LEFT_STICK],
    [11, GAMEPAD_BUTTONS.RIGHT_STICK],
    [12, GAMEPAD_BUTTONS.DPAD_UP],
    [13, GAMEPAD_BUTTONS.DPAD_DOWN],
    [14, GAMEPAD_BUTTONS.DPAD_LEFT],
    [15, GAMEPAD_BUTTONS.DPAD_RIGHT],
    [16, GAMEPAD_BUTTONS.GUIDE],
  ];
  mappings.forEach(function (mapping) {
    if (gamepadButtonPressed(gamepad, mapping[0])) {
      flags |= mapping[1];
    }
  });
  return flags;
}

function gamepadState(gamepad) {
  return {
    buttons: standardButtonFlags(gamepad),
    leftTrigger: Number(gamepadButton(gamepad, 6).value) || 0,
    rightTrigger: Number(gamepadButton(gamepad, 7).value) || 0,
    leftStickX: gamepadAxis(gamepad, 0),
    leftStickY: gamepadAxis(gamepad, 1),
    rightStickX: gamepadAxis(gamepad, 2),
    rightStickY: gamepadAxis(gamepad, 3),
  };
}

function neutralGamepadState() {
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

function recordInputMessage(now) {
  inputMessagesInWindow += 1;
  const elapsed = now - inputRateWindowStart;
  if (elapsed >= 1000) {
    gamepadDiagnostics.sendRate.textContent = (
      inputMessagesInWindow * 1000 / elapsed
    ).toFixed(1);
    inputMessagesInWindow = 0;
    inputRateWindowStart = now;
  }
}

function sendCompleteGamepadState(state, now) {
  if (!dataChannelIsOpen(gamepadDataChannel) || !gamepadAnnounced) {
    return false;
  }
  gamepadSequence += 1;
  gamepadDataChannel.send(JSON.stringify({
    v: GAMEPAD_PROTOCOL_VERSION,
    type: "gamepad-state",
    seq: gamepadSequence,
    timestampMs: now,
    buttons: state.buttons,
    leftTrigger: state.leftTrigger,
    rightTrigger: state.rightTrigger,
    leftStickX: state.leftStickX,
    leftStickY: state.leftStickY,
    rightStickX: state.rightStickX,
    rightStickY: state.rightStickY,
  }));
  lastFullStateSendTime = now;
  gamepadDiagnostics.lastSequence.textContent = String(gamepadSequence);
  recordInputMessage(now);
  return true;
}

function announceGamepad(gamepad) {
  if (!gamepad || !dataChannelIsOpen(controlDataChannel) || document.hidden
      || sessionState !== "streaming") {
    return false;
  }
  controlDataChannel.send(JSON.stringify({
    v: GAMEPAD_PROTOCOL_VERSION,
    type: "gamepad-connected",
    index: gamepad.index,
    id: gamepad.id || "Unknown gamepad",
    mapping: gamepad.mapping || "",
    buttons: gamepad.buttons ? gamepad.buttons.length : 0,
    axes: gamepad.axes ? gamepad.axes.length : 0,
  }));
  gamepadAnnounced = true;
  log("Gamepad connected: " + (gamepad.id || "Unknown gamepad"));
  return true;
}

function notifyGamepadDisconnected() {
  if (gamepadAnnounced && dataChannelIsOpen(controlDataChannel)) {
    controlDataChannel.send(JSON.stringify({
      v: GAMEPAD_PROTOCOL_VERSION,
      type: "gamepad-disconnected",
    }));
  }
  gamepadAnnounced = false;
}

function updateGamepadDiagnostics(gamepad) {
  const connected = Boolean(gamepad);
  gamepadDiagnostics.state.textContent = connected ? "CONNECTED" : "DISCONNECTED";
  gamepadDiagnostics.id.textContent = connected ? (gamepad.id || "Unknown gamepad") : "-";
  gamepadDiagnostics.mapping.textContent = connected ? (gamepad.mapping || "none") : "-";
  overlay.gamepad.textContent = connected ? "Connected" : "Disconnected";
}

function stopGamepadPolling() {
  if (gamepadPollTimer !== null) {
    clearTimeout(gamepadPollTimer);
    gamepadPollTimer = null;
  }
}

function scheduleGamepadPoll() {
  if (document.hidden || gamepadPollTimer !== null || sessionState !== "streaming") {
    return;
  }
  const delay = Math.max(0, nextGamepadPollTime - performance.now());
  gamepadPollTimer = setTimeout(pollGamepad, delay);
}

function pollGamepad() {
  gamepadPollTimer = null;
  if (document.hidden || sessionState !== "streaming") {
    return;
  }
  const gamepad = activeGamepad();
  if (!gamepad) {
    if (activeGamepadIndex !== null) {
      sendCompleteGamepadState(neutralGamepadState(), performance.now());
      notifyGamepadDisconnected();
      activeGamepadIndex = null;
      updateGamepadDiagnostics(null);
      log("Gamepad disconnected");
    }
    return;
  }

  updateGamepadDiagnostics(gamepad);
  if (!gamepadAnnounced) {
    announceGamepad(gamepad);
  }
  const now = performance.now();
  const state = gamepadState(gamepad);
  const fingerprint = JSON.stringify(state);
  if (fingerprint !== lastGamepadFingerprint
      || now - lastFullStateSendTime >= GAMEPAD_KEEPALIVE_INTERVAL_MS) {
    if (sendCompleteGamepadState(state, now)) {
      lastGamepadFingerprint = fingerprint;
    }
  }
  nextGamepadPollTime += GAMEPAD_POLL_INTERVAL_MS;
  if (nextGamepadPollTime < now - GAMEPAD_POLL_INTERVAL_MS * 4) {
    nextGamepadPollTime = now + GAMEPAD_POLL_INTERVAL_MS;
  }
  scheduleGamepadPoll();
}

function resumeGamepadInput() {
  const gamepad = activeGamepad();
  updateGamepadDiagnostics(gamepad);
  if (!gamepad || sessionState !== "streaming") {
    return;
  }
  if (!gamepadAnnounced) {
    announceGamepad(gamepad);
  }
  lastGamepadFingerprint = "";
  nextGamepadPollTime = performance.now();
  scheduleGamepadPoll();
}

function suspendGamepadInput(notifyGateway) {
  stopGamepadPolling();
  if (gamepadAnnounced) {
    sendCompleteGamepadState(neutralGamepadState(), performance.now());
    if (notifyGateway) {
      notifyGamepadDisconnected();
    }
  }
  lastGamepadFingerprint = "";
}

function configureControlDataChannel(channel) {
  controlDataChannel = channel;
  updateDataChannelState(gamepadDiagnostics.controlChannel, channel);
  channel.onopen = function () {
    updateDataChannelState(gamepadDiagnostics.controlChannel, channel);
    log("Control DataChannel opened");
    resumeGamepadInput();
  };
  channel.onclose = function () {
    updateDataChannelState(gamepadDiagnostics.controlChannel, channel);
    suspendGamepadInput(false);
    gamepadAnnounced = false;
    log("Control DataChannel closed");
  };
  channel.onerror = function (event) {
    reportError("Control DataChannel error", event.error || event);
  };
}

function configureGamepadDataChannel(channel) {
  gamepadDataChannel = channel;
  updateDataChannelState(gamepadDiagnostics.inputChannel, channel);
  channel.onopen = function () {
    updateDataChannelState(gamepadDiagnostics.inputChannel, channel);
    log("Gamepad DataChannel opened");
    lastGamepadFingerprint = "";
    resumeGamepadInput();
  };
  channel.onclose = function () {
    updateDataChannelState(gamepadDiagnostics.inputChannel, channel);
    suspendGamepadInput(true);
    log("Gamepad DataChannel closed");
  };
  channel.onerror = function (event) {
    reportError("Gamepad DataChannel error", event.error || event);
  };
}

window.addEventListener("gamepadconnected", function (event) {
  if (activeGamepadIndex === null) {
    activeGamepadIndex = event.gamepad.index;
  }
  if (activeGamepadIndex !== event.gamepad.index) {
    return;
  }
  updateGamepadDiagnostics(event.gamepad);
  resumeGamepadInput();
});

window.addEventListener("gamepaddisconnected", function (event) {
  if (activeGamepadIndex !== event.gamepad.index) {
    return;
  }
  sendCompleteGamepadState(neutralGamepadState(), performance.now());
  notifyGamepadDisconnected();
  stopGamepadPolling();
  activeGamepadIndex = null;
  updateGamepadDiagnostics(null);
  log("Gamepad disconnected");
});

function statisticValue(report, name) {
  return typeof report[name] === "undefined" ? 0 : report[name];
}

async function updateStatistics() {
  if (!peerConnection) {
    return;
  }
  try {
    const reports = await peerConnection.getStats();
    let inboundVideo = null;
    let inboundAudio = null;
    const codecReports = {};
    reports.forEach(function (report) {
      const kind = typeof report.kind === "undefined" ? report.mediaType : report.kind;
      if (report.type === "inbound-rtp" && kind === "video" && !report.isRemote) {
        inboundVideo = report;
      } else if (report.type === "inbound-rtp" && kind === "audio" && !report.isRemote) {
        inboundAudio = report;
      } else if (report.type === "codec" && report.id) {
        codecReports[report.id] = report;
      }
    });

    statistics.actualResolution.textContent = String(videoElement.videoWidth || 0)
      + " × " + String(videoElement.videoHeight || 0);
    if (selectedSession) {
      statistics.requestedResolution.textContent = String(selectedSession.width)
        + " × " + String(selectedSession.height);
      statistics.requestedCodec.textContent = codecDisplayName(selectedSession.codec);
      statistics.requestedBitrate.textContent = String(selectedSession.bitrateKbps / 1000)
        + " Mbps";
    }

    if (inboundVideo) {
      const framesDecoded = statisticValue(inboundVideo, "framesDecoded");
      const sampleTime = typeof inboundVideo.timestamp === "number"
        ? inboundVideo.timestamp
        : performance.now();
      statistics.video.framesReceived.textContent = statisticValue(inboundVideo, "framesReceived");
      statistics.video.framesDecoded.textContent = framesDecoded;
      statistics.video.framesDropped.textContent = statisticValue(inboundVideo, "framesDropped");
      statistics.video.packetsReceived.textContent = statisticValue(inboundVideo, "packetsReceived");
      statistics.video.packetsLost.textContent = statisticValue(inboundVideo, "packetsLost");
      statistics.video.bytesReceived.textContent = statisticValue(inboundVideo, "bytesReceived");
      const codecReport = inboundVideo.codecId ? codecReports[inboundVideo.codecId] : null;
      const receivedCodec = codecReport && codecReport.mimeType
        ? String(codecReport.mimeType)
        : "-";
      statistics.receivedCodec.textContent = receivedCodec;
      if (previousDecodedSample && sampleTime > previousDecodedSample.time) {
        const elapsedSeconds = (sampleTime - previousDecodedSample.time) / 1000;
        statistics.video.decodedFps.textContent = (
          (framesDecoded - previousDecodedSample.frames) / elapsedSeconds
        ).toFixed(1);
      }
      previousDecodedSample = { time: sampleTime, frames: framesDecoded };

      if (sampleTime - lastStatisticsConsoleTime >= 5000) {
        lastStatisticsConsoleTime = sampleTime;
        console.log("Stream diagnostics: " + JSON.stringify({
          requestedWidth: selectedSession ? selectedSession.width : 0,
          requestedHeight: selectedSession ? selectedSession.height : 0,
          requestedCodec: selectedSession ? selectedSession.codec : "unknown",
          requestedBitrateKbps: selectedSession ? selectedSession.bitrateKbps : 0,
          actualWidth: videoElement.videoWidth || 0,
          actualHeight: videoElement.videoHeight || 0,
          receivedCodec: receivedCodec,
          decodedFps: statistics.video.decodedFps.textContent,
          framesReceived: statisticValue(inboundVideo, "framesReceived"),
          framesDecoded: framesDecoded,
          framesDropped: statisticValue(inboundVideo, "framesDropped"),
          packetsReceived: statisticValue(inboundVideo, "packetsReceived"),
          packetsLost: statisticValue(inboundVideo, "packetsLost"),
          bytesReceived: statisticValue(inboundVideo, "bytesReceived"),
          audioPacketsReceived: inboundAudio
            ? statisticValue(inboundAudio, "packetsReceived") : 0,
          audioPacketsLost: inboundAudio ? statisticValue(inboundAudio, "packetsLost") : 0,
          audioJitter: inboundAudio ? statisticValue(inboundAudio, "jitter") : 0,
          gamepadConnected: activeGamepad() !== null,
        }));
      }
    }

    if (inboundAudio) {
      statistics.audio.packetsReceived.textContent = statisticValue(inboundAudio, "packetsReceived");
      statistics.audio.packetsLost.textContent = statisticValue(inboundAudio, "packetsLost");
      statistics.audio.bytesReceived.textContent = statisticValue(inboundAudio, "bytesReceived");
      statistics.audio.jitter.textContent = Number(
        statisticValue(inboundAudio, "jitter")
      ).toFixed(6);
    }
  } catch (error) {
    if (!statisticsErrorReported) {
      statisticsErrorReported = true;
      reportError("getStats failed", error);
    }
  }
}

window.addEventListener("unhandledrejection", function (event) {
  reportError("Unhandled promise rejection", event.reason);
});

setInterval(updateStatistics, 1000);
updateTrackCounts();
updateGamepadDiagnostics(activeGamepad());
showHome();
connectGateway();
