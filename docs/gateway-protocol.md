# Moonlight WebRTC Gateway protocol

Protocol version 1 is a local JSON protocol carried by the existing signaling WebSocket on
port 8000. Every message contains `"version": 1` and a `type`. The WebSocket transports
configuration, lifecycle state, and WebRTC signaling only. Audio, video, and realtime
gamepad snapshots remain on WebRTC.

## Gateway startup

Opening the WebSocket does not start Sunshine or WebRTC. The Gateway first sends:

```json
{"version":1,"type":"gateway-status","gatewayName":"Moonlight WebRTC Gateway","sunshineDetected":true,"sunshinePaired":true,"sessionActive":false}
```

It also sends `capabilities`. Version 1 advertises only 1280x720 and 1920x1080, 60 fps,
H.264, HDR off, stereo 48 kHz, and bitrates 10000, 12000, 15000, 20000, 25000, or
30000 kbps.

## Applications

Client request:

```json
{"version":1,"type":"get-apps"}
```

Gateway response (entries come directly from Sunshine):

```json
{"version":1,"type":"apps","apps":[{"id":"0","title":"Desktop"}]}
```

## Start a session

```json
{
  "version": 1,
  "type": "start-session",
  "appId": "0",
  "video": {
    "width": 1920,
    "height": 1080,
    "fps": 60,
    "codec": "h264",
    "bitrateKbps": 20000,
    "hdr": false
  },
  "audio": {"channels": 2}
}
```

The Gateway validates every field and rejects unsupported settings. Status transitions use
`session-status` with one of `idle`, `starting`, `connecting-sunshine`,
`starting-moonlight`, `starting-webrtc`, `streaming`, `stopping`, or `error`.

Each started session receives a positive `sessionId`. WebRTC `offer`, `answer`, and
`candidate` messages contain that ID so late signaling from a stopped session can be
ignored.

## Stop a session

```json
{"version":1,"type":"stop-session"}
```

The Gateway neutralizes/removes the controller, stops Moonlight and media, closes the
PeerConnection, keeps the WebSocket open, and returns to `idle`. A new `start-session`
request can then create another PeerConnection without restarting the Gateway.

## Errors

```json
{"version":1,"type":"error","requestType":"start-session","code":"unsupported-settings","message":"Unsupported resolution"}
```

Errors contain no certificate, identity, or pairing secret.
