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

const pendingRemoteCandidates = [];
const remoteStream = new MediaStream();
videoElement.srcObject = remoteStream;
videoElement.muted = false;
let statisticsErrorReported = false;
let previousDecodedSample = null;
let playbackErrorReported = false;
let rawOfferSdp = "";

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
