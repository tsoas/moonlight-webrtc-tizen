const SIGNALING_URL = "ws://192.168.0.69:8000";

const diagnosticsElement = document.getElementById("diagnostics");
const logElement = document.getElementById("log");
const videoElement = document.getElementById("video");
const playbackPromptElement = document.getElementById("playback-prompt");
const connectionStateElement = document.getElementById("connection-state");
const iceStateElement = document.getElementById("ice-state");
const trackCounts = {
  remote: document.getElementById("remote-tracks"),
  video: document.getElementById("video-tracks"),
  audio: document.getElementById("audio-tracks"),
};
const statistics = {
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

const pendingRemoteCandidates = [];
const remoteStream = new MediaStream();
videoElement.srcObject = remoteStream;
videoElement.muted = false;
let statisticsErrorReported = false;
let previousDecodedSample = null;
let playbackErrorReported = false;
let rawOfferSdp = "";
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

const GAMEPAD_PROTOCOL_VERSION = 1;
const GAMEPAD_POLL_INTERVAL_MS = 1000 / 120;
const GAMEPAD_KEEPALIVE_INTERVAL_MS = 250;
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

function log(message) {
  console.log(message);
  logElement.textContent += message + "\n";
  logElement.scrollTop = logElement.scrollHeight;
}

function errorMessage(error) {
  if (!error) {
    return "Unknown error";
  }
  return error.message || error.name || String(error);
}

function reportError(context, error) {
  const message = "ERROR: " + context + ": " + errorMessage(error);
  diagnosticsElement.classList.add("has-error");
  console.error(message, error);
  logElement.textContent += message + "\n";
  logElement.scrollTop = logElement.scrollHeight;
}

function updateTrackCounts() {
  trackCounts.remote.textContent = remoteStream.getTracks().length;
  trackCounts.video.textContent = remoteStream.getVideoTracks().length;
  trackCounts.audio.textContent = remoteStream.getAudioTracks().length;
}

function updateRemoteTracksForVisibility() {
  const enabled = !document.hidden;
  remoteStream.getTracks().forEach(function (track) {
    track.enabled = enabled;
  });
  log(enabled
    ? "Application foregrounded - media tracks enabled"
    : "Application backgrounded - media tracks disabled");

  if (enabled) {
    resumeGamepadInput();
  } else {
    suspendGamepadInput(true);
  }
}

document.addEventListener("visibilitychange", updateRemoteTracksForVisibility);

async function startPlayback() {
  videoElement.muted = false;

  try {
    await videoElement.play();
    playbackPromptElement.hidden = true;
    if (playbackErrorReported) {
      log("Playback started after user action");
    }
  } catch (error) {
    playbackPromptElement.hidden = false;
    if (!playbackErrorReported) {
      playbackErrorReported = true;
      reportError("video.play() requires pressing OK", error);
    }
  }
}

document.addEventListener("keydown", function (event) {
  if (event.key === "Enter" || event.keyCode === 13) {
    startPlayback();
  }
});

let peerConnection;
try {
  peerConnection = new RTCPeerConnection({
    iceServers: [],
  });
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

let socket;
try {
  socket = new WebSocket(SIGNALING_URL);
} catch (error) {
  reportError("WebSocket creation failed", error);
  throw error;
}

async function applyRemoteCandidate(candidate) {
  try {
    await peerConnection.addIceCandidate(candidate);
    log("Received ICE candidate: " + candidate.candidate);
    return true;
  } catch (error) {
    reportError("addIceCandidate failed", error);
    return false;
  }
}

async function handleRemoteCandidate(message) {
  const candidate = {
    candidate: message.candidate,
    sdpMid: message.mid,
  };

  if (!peerConnection.remoteDescription) {
    pendingRemoteCandidates.push(candidate);
    log("Queued ICE candidate: " + candidate.candidate);
    return;
  }

  await applyRemoteCandidate(candidate);
}

async function handleOffer(message) {
  rawOfferSdp = message.sdp;
  try {
    await peerConnection.setRemoteDescription({
      type: "offer",
      sdp: message.sdp,
    });
    log("Received SDP offer");
  } catch (error) {
    reportError("setRemoteDescription failed", error);
    return;
  }

  while (pendingRemoteCandidates.length > 0) {
    await applyRemoteCandidate(pendingRemoteCandidates.shift());
  }

  let answer;
  try {
    answer = await peerConnection.createAnswer();
  } catch (error) {
    reportError("createAnswer failed", error);
    return;
  }

  try {
    await peerConnection.setLocalDescription(answer);
  } catch (error) {
    reportError("setLocalDescription failed", error);
    return;
  }

  try {
    socket.send(JSON.stringify({
      type: "answer",
      sdp: peerConnection.localDescription.sdp,
    }));
    log("Sent SDP answer");
  } catch (error) {
    reportError("Sending SDP answer failed", error);
  }
}

socket.addEventListener("open", function () {
  log("WebSocket connected");
});

socket.addEventListener("close", function () {
  suspendGamepadInput(true);
  log("WebSocket disconnected");
});

socket.addEventListener("error", function (event) {
  reportError("WebSocket error", event.error || event);
});

socket.addEventListener("message", async function (event) {
  let message;
  try {
    message = JSON.parse(event.data);
  } catch (error) {
    reportError("Invalid signaling JSON", error);
    return;
  }

  if (message.type === "offer") {
    await handleOffer(message);
  } else if (message.type === "candidate") {
    await handleRemoteCandidate(message);
  }
});

peerConnection.onicecandidate = function (event) {
  if (!event.candidate) {
    return;
  }

  try {
    socket.send(JSON.stringify({
      type: "candidate",
      candidate: event.candidate.candidate,
      mid: event.candidate.sdpMid,
    }));
    log("Sent ICE candidate: " + event.candidate.candidate);
  } catch (error) {
    reportError("Sending ICE candidate failed", error);
  }
};

peerConnection.onconnectionstatechange = function () {
  const state = peerConnection.connectionState;
  connectionStateElement.textContent = state;
  log("PeerConnection state: " + state);
  if (state === "disconnected" || state === "failed" || state === "closed") {
    suspendGamepadInput(true);
  }
};

peerConnection.oniceconnectionstatechange = function () {
  const state = peerConnection.iceConnectionState;
  iceStateElement.textContent = state;
  log("ICE state: " + state);
};

peerConnection.ontrack = function (event) {
  event.track.enabled = !document.hidden;
  const alreadyAdded = remoteStream.getTracks().some(function (track) {
    return track.id === event.track.id;
  });

  if (!alreadyAdded) {
    remoteStream.addTrack(event.track);
  }

  const sourceStreamId = event.streams && event.streams[0]
    ? event.streams[0].id
    : "none";
  log("Remote " + event.track.kind + " track received in stream: " + sourceStreamId);
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
  if (!gamepad || !dataChannelIsOpen(controlDataChannel) || document.hidden) {
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
  gamepadDiagnostics.state.textContent = gamepad ? "CONNECTED" : "DISCONNECTED";
  gamepadDiagnostics.id.textContent = gamepad ? (gamepad.id || "Unknown gamepad") : "-";
  gamepadDiagnostics.mapping.textContent = gamepad ? (gamepad.mapping || "none") : "-";
}

function stopGamepadPolling() {
  if (gamepadPollTimer !== null) {
    clearTimeout(gamepadPollTimer);
    gamepadPollTimer = null;
  }
}

function scheduleGamepadPoll() {
  if (document.hidden || gamepadPollTimer !== null) {
    return;
  }
  const delay = Math.max(0, nextGamepadPollTime - performance.now());
  gamepadPollTimer = setTimeout(pollGamepad, delay);
}

function pollGamepad() {
  gamepadPollTimer = null;
  if (document.hidden) {
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
  if (!gamepad) {
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
  log("Control DataChannel received: ordered=" + channel.ordered
    + ", maxRetransmits=" + channel.maxRetransmits);
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
  log("Gamepad DataChannel received: ordered=" + channel.ordered
    + ", maxRetransmits=" + channel.maxRetransmits);
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
  try {
    const reports = await peerConnection.getStats();
    let inboundVideo = null;
    let inboundAudio = null;

    reports.forEach(function (report) {
      const kind = typeof report.kind === "undefined" ? report.mediaType : report.kind;
      if (report.type === "inbound-rtp" && kind === "video" && !report.isRemote) {
        inboundVideo = report;
      } else if (report.type === "inbound-rtp" && kind === "audio" && !report.isRemote) {
        inboundAudio = report;
      }
    });

    if (inboundVideo) {
      const framesDecoded = statisticValue(inboundVideo, "framesDecoded");
      const sampleTime = typeof inboundVideo.timestamp === "number"
        ? inboundVideo.timestamp
        : performance.now();

      statistics.video.framesReceived.textContent = statisticValue(
        inboundVideo,
        "framesReceived"
      );
      statistics.video.framesDecoded.textContent = framesDecoded;
      statistics.video.framesDropped.textContent = statisticValue(
        inboundVideo,
        "framesDropped"
      );
      statistics.video.packetsReceived.textContent = statisticValue(
        inboundVideo,
        "packetsReceived"
      );
      statistics.video.packetsLost.textContent = statisticValue(inboundVideo, "packetsLost");
      statistics.video.bytesReceived.textContent = statisticValue(
        inboundVideo,
        "bytesReceived"
      );

      if (previousDecodedSample && sampleTime > previousDecodedSample.time) {
        const elapsedSeconds = (sampleTime - previousDecodedSample.time) / 1000;
        const decodedFps = (framesDecoded - previousDecodedSample.frames) / elapsedSeconds;
        statistics.video.decodedFps.textContent = decodedFps.toFixed(1);
      }
      previousDecodedSample = {
        time: sampleTime,
        frames: framesDecoded,
      };
    }

    if (inboundAudio) {
      statistics.audio.packetsReceived.textContent = statisticValue(
        inboundAudio,
        "packetsReceived"
      );
      statistics.audio.packetsLost.textContent = statisticValue(inboundAudio, "packetsLost");
      statistics.audio.bytesReceived.textContent = statisticValue(
        inboundAudio,
        "bytesReceived"
      );
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
