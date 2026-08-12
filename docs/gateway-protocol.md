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

It also sends `capabilities`. Protocol version 1 advertises explicit `videoModes` so the
TV never has to infer a resolution/codec combination. Each mode contains `width`,
`height`, `fps`, `codecs`, `defaultCodec`, `defaultBitrateKbps`, and `experimental`.

| Mode | Codecs | Default | Default bitrate | Experimental |
| --- | --- | --- | ---: | --- |
| 1280x720 @ 60 | H.264, HEVC | H.264 | 12000 kbps | No |
| 1920x1080 @ 60 | H.264, HEVC | H.264 | 20000 kbps | No |
| 2560x1440 @ 60 | H.264, HEVC | HEVC | 30000 kbps | Yes |
| 3840x2160 @ 60 | HEVC | HEVC | 50000 kbps | No |

The selectable bitrates are 10000, 12000, 15000, 20000, 25000, 30000, 40000, and
50000 kbps. HDR remains off, audio remains stereo Opus at 48 kHz, and frame rate remains
fixed at 60 fps. The 1440p mode is experimental because Samsung does not list it in the
official Cloud Gaming resolution table.

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
    "width": 3840,
    "height": 2160,
    "fps": 60,
    "codec": "hevc",
    "bitrateKbps": 50000,
    "hdr": false
  },
  "audio": {"channels": 2}
}
```

`codec` is exactly `"h264"` or `"hevc"`. HEVC means Main Profile 8-bit SDR; Main10,
HDR, Rec.2020, AV1, and silent codec fallback are not part of protocol version 1. The
Gateway validates every field and rejects unsupported settings. Status transitions use
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
