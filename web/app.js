const statusElement = document.querySelector("#status");
const logElement = document.querySelector("#log");
const videoElement = document.querySelector("#video");

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
  log(`PeerConnection state: ${state}`);
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
  log(`ICE state: ${peerConnection.iceConnectionState}`);
});

peerConnection.addEventListener("track", (event) => {
  videoElement.srcObject = event.streams[0] ?? new MediaStream([event.track]);
  log("Remote video track received");
});
