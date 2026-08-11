const SIGNALING_URL = "ws://192.168.0.69:8000";

const diagnosticsElement = document.getElementById("diagnostics");
const logElement = document.getElementById("log");
const videoElement = document.getElementById("video");
const connectionStateElement = document.getElementById("connection-state");
const iceStateElement = document.getElementById("ice-state");
const statistics = {
  framesReceived: document.getElementById("frames-received"),
  framesDecoded: document.getElementById("frames-decoded"),
  framesDropped: document.getElementById("frames-dropped"),
  packetsReceived: document.getElementById("packets-received"),
  packetsLost: document.getElementById("packets-lost"),
  bytesReceived: document.getElementById("bytes-received"),
};

const pendingRemoteCandidates = [];
let statisticsErrorReported = false;

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
  videoElement.srcObject = event.streams && event.streams[0]
    ? event.streams[0]
    : new MediaStream([event.track]);
  log("Remote video track received");

  const playback = videoElement.play();
  if (playback && typeof playback.catch === "function") {
    playback.catch(function (error) {
      reportError("video.play() failed", error);
    });
  }
};

function statisticValue(report, name) {
  return typeof report[name] === "undefined" ? 0 : report[name];
}

async function updateStatistics() {
  try {
    const reports = await peerConnection.getStats();
    let inboundVideo = null;

    reports.forEach(function (report) {
      const kind = typeof report.kind === "undefined" ? report.mediaType : report.kind;
      if (report.type === "inbound-rtp" && kind === "video" && !report.isRemote) {
        inboundVideo = report;
      }
    });

    if (!inboundVideo) {
      return;
    }

    statistics.framesReceived.textContent = statisticValue(inboundVideo, "framesReceived");
    statistics.framesDecoded.textContent = statisticValue(inboundVideo, "framesDecoded");
    statistics.framesDropped.textContent = statisticValue(inboundVideo, "framesDropped");
    statistics.packetsReceived.textContent = statisticValue(inboundVideo, "packetsReceived");
    statistics.packetsLost.textContent = statisticValue(inboundVideo, "packetsLost");
    statistics.bytesReceived.textContent = statisticValue(inboundVideo, "bytesReceived");
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
