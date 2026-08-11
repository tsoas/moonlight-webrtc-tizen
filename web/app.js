const statusElement = document.querySelector("#status");
const logElement = document.querySelector("#log");
const videoElement = document.querySelector("#video");
const statistics = {
  connection: document.querySelector("#stat-connection"),
  ice: document.querySelector("#stat-ice"),
  framesReceived: document.querySelector("#stat-frames-received"),
  framesDecoded: document.querySelector("#stat-frames-decoded"),
  framesDropped: document.querySelector("#stat-frames-dropped"),
  packetsReceived: document.querySelector("#stat-packets-received"),
  packetsLost: document.querySelector("#stat-packets-lost"),
  bytesReceived: document.querySelector("#stat-bytes-received"),
  framesPerSecond: document.querySelector("#stat-frames-per-second"),
};

const peerConnection = new RTCPeerConnection({
  iceServers: [],
});

const pendingRemoteCandidates = [];
const socket = new WebSocket("ws://127.0.0.1:8000");

function log(message) {
  console.log(message);
  logElement.textContent += `${message}\n`;
  logElement.scrollTop = logElement.scrollHeight;
}

function updateConnectionState() {
  const state = peerConnection.connectionState;
  statusElement.textContent = `Connection state: ${state}`;
  statistics.connection.textContent = state;
  log(`PeerConnection state: ${state}`);
}

async function updateVideoStatistics() {
  try {
    const reports = await peerConnection.getStats();
    let inboundVideo;

    reports.forEach((report) => {
      const kind = report.kind ?? report.mediaType;
      if (report.type === "inbound-rtp" && kind === "video" && !report.isRemote) {
        inboundVideo = report;
      }
    });

    if (!inboundVideo) {
      return;
    }

    statistics.framesReceived.textContent = inboundVideo.framesReceived ?? 0;
    statistics.framesDecoded.textContent = inboundVideo.framesDecoded ?? 0;
    statistics.framesDropped.textContent = inboundVideo.framesDropped ?? 0;
    statistics.packetsReceived.textContent = inboundVideo.packetsReceived ?? 0;
    statistics.packetsLost.textContent = inboundVideo.packetsLost ?? 0;
    statistics.bytesReceived.textContent = inboundVideo.bytesReceived ?? 0;
    statistics.framesPerSecond.textContent = inboundVideo.framesPerSecond ?? "n/a";
  } catch (error) {
    console.error("Unable to read WebRTC statistics", error);
  }
}

async function addRemoteCandidate(message) {
  const candidate = {
    candidate: message.candidate,
    sdpMid: message.mid,
  };

  if (!peerConnection.remoteDescription) {
    pendingRemoteCandidates.push(candidate);
    return;
  }

  await peerConnection.addIceCandidate(candidate);
  log(`Received ICE candidate: ${message.candidate}`);
}

async function handleOffer(message) {
  await peerConnection.setRemoteDescription({
    type: "offer",
    sdp: message.sdp,
  });
  log("Received SDP offer");

  for (const candidate of pendingRemoteCandidates.splice(0)) {
    await peerConnection.addIceCandidate(candidate);
    log(`Received ICE candidate: ${candidate.candidate}`);
  }

  const answer = await peerConnection.createAnswer();
  await peerConnection.setLocalDescription(answer);

  socket.send(JSON.stringify({
    type: "answer",
    sdp: peerConnection.localDescription.sdp,
  }));
  log("Sent SDP answer");
}

socket.addEventListener("open", () => {
  log("WebSocket connected");
});

socket.addEventListener("close", () => {
  log("WebSocket disconnected");
});

socket.addEventListener("error", () => {
  log("WebSocket error");
});

socket.addEventListener("message", async (event) => {
  try {
    const message = JSON.parse(event.data);

    if (message.type === "offer") {
      await handleOffer(message);
    } else if (message.type === "candidate") {
      await addRemoteCandidate(message);
    }
  } catch (error) {
    log(`Signaling error: ${error.message}`);
  }
});

peerConnection.addEventListener("icecandidate", (event) => {
  if (!event.candidate) {
    return;
  }

  socket.send(JSON.stringify({
    type: "candidate",
    candidate: event.candidate.candidate,
    mid: event.candidate.sdpMid,
  }));
  log(`Sent ICE candidate: ${event.candidate.candidate}`);
});

peerConnection.addEventListener("connectionstatechange", updateConnectionState);

peerConnection.addEventListener("iceconnectionstatechange", () => {
  const state = peerConnection.iceConnectionState;
  statistics.ice.textContent = state;
  log(`ICE state: ${state}`);
});

peerConnection.addEventListener("track", (event) => {
  videoElement.srcObject = event.streams[0] ?? new MediaStream([event.track]);
  videoElement.play().catch((error) => {
    log(`Video playback error: ${error.message}`);
  });
  log("Remote video track received");
});

setInterval(updateVideoStatistics, 1000);
