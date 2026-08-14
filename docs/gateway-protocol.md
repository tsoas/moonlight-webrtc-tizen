# Moonlight WebRTC Gateway protocol

Protocol version 1 is a local JSON protocol carried by the existing signaling WebSocket on
port 8000. Every message contains `"version": 1` and a `type`. The WebSocket transports
configuration, lifecycle state, and WebRTC signaling only. Audio, video, and realtime
gamepad snapshots remain on WebRTC.

## Local Windows service IPC

The Windows tray companion is a separate interactive-user process. It does not connect to
the signaling WebSocket and cannot control streaming. It requests read-only Gateway status
over the local named pipe `\\.\pipe\MoonlightWebRTCGateway`; the pipe rejects remote clients.

Its protocol is independently versioned. Opening the read-only pipe requests one status
snapshot; the service replies with a little-endian 32-bit JSON-byte length followed by the
versioned JSON response. For example:

```json
{"version":1,"type":"status","serviceRunning":true,"sunshineConnected":true,"sunshinePaired":true,"sunshineHost":"Sunshine-PC","runningApplicationId":"7","sessionActive":false,"connectedTvClients":0}
```

Unavailable fields are omitted. The endpoint exposes no private key, certificate, pairing
material, credential, token, or control command. Its protected ACL grants full access only
to LocalService, SYSTEM, and Administrators. Local standard users receive only the standard
named-pipe read mask: an open requests a snapshot, but no user-session process writes to the
LocalService-owned endpoint. The DACL excludes remote callers.

### Local Windows management IPC

Configuration commands use a separate, tray-owned named pipe:
`\\.\pipe\MoonlightWebRTCGateway.Management`. This does not change the status pipe,
which remains read-only. The tray creates the management pipe with
`PIPE_REJECT_REMOTE_CLIENTS` and a protected DACL granting access only to its current
logon SID and LocalService. It impersonates each connecting client and accepts only the
LocalService SID. The service is the pipe client and verifies the server PID belongs to
the active interactive session and is neither Session 0 nor a service identity before it
accepts any command.

Messages are a bounded (16 KiB) little-endian 32-bit length followed by JSON. Version 1
accepts only `set-host` and `test` commands; every response is a structured `result` with
the requested command, `ok`, a stable code, and a user-safe message. The service remains
the sole writer of ProgramData and pairing material. `set-host` writes only
`sunshine-host.txt`; `test` first validates Sunshine's protocol response and, when paired,
performs an authenticated request using the existing pinned certificate. It does not pair,
unpair, or mutate Sunshine state.

## Gateway startup

Opening the WebSocket does not start Sunshine or WebRTC. The Gateway first sends:

```json
{"version":1,"type":"gateway-status","gatewayName":"Moonlight WebRTC Gateway","sunshineDetected":true,"sunshinePaired":true,"sessionActive":false}
```

It also sends `capabilities`. Protocol version 1 advertises explicit `videoModes` so the
TV never has to infer a resolution/codec combination. Each mode contains `width`,
`height`, `fps`, `codecs`, `defaultCodec`, `defaultBitrateKbps`, `experimental`,
`hdrSupported`, and `hdrExperimental`.

| Mode | Codecs | HDR | Default | Default bitrate | Experimental |
| --- | --- | --- | --- | ---: | --- |
| 1280x720 @ 60 | H.264, HEVC | Off | H.264 | 12000 kbps | No |
| 1920x1080 @ 60 | H.264, HEVC | Off, On | H.264 | 20000 kbps | HDR only |
| 2560x1440 @ 60 | H.264, HEVC | Off, On | HEVC | 30000 kbps | Yes |
| 3840x2160 @ 60 | HEVC | Off, On | HEVC | 50000 kbps | HDR only |

The selectable bitrates are 10000, 12000, 15000, 20000, 25000, 30000, 40000, and
50000 kbps. HDR defaults to off and is experimental when explicitly selected with HEVC.
Audio remains stereo Opus at 48 kHz and frame rate remains fixed at 60 fps. The entire
1440p mode, including HDR, is experimental because Samsung does not list it in the
official Cloud Gaming resolution table.

## Applications

Client request:

```json
{"version":1,"type":"get-apps"}
```

Gateway response (entries come directly from Sunshine):

```json
{"version":1,"type":"apps","apps":[{"id":"0","title":"Desktop","artworkAvailable":false,"running":false}]}
```

`id` remains Sunshine's exact opaque application ID as returned by `/applist`; the Gateway
does not derive it from application order or convert it before an artwork request. `running` is derived from Sunshine's
authenticated `serverinfo` current application while the server reports an active stream.
`artworkAvailable` reports only the Gateway's current in-memory cache state, so the
application list stays small and renders without waiting for image downloads.

Artwork is fetched lazily through the same Gateway WebSocket. The TV requests one asset at a
time:

```json
{"version":1,"type":"get-app-artwork","appId":"0"}
```

The Gateway retrieves Sunshine's authenticated GameStream `appasset` resource with
`appid`, `AssetType=2`, and `AssetIdx=0`, using the existing Moonlight identity and pinned
Sunshine certificate. It validates JPEG, PNG, or WebP media before returning a display-only
response:

```json
{"version":1,"type":"app-artwork","appId":"0","available":true,"mimeType":"image/jpeg","data":"...base64..."}
```

Missing, malformed, or unavailable artwork returns `"available":false`. This never fails
the application list; the TV keeps its normal fallback card. The Gateway caches successful
and unavailable results in memory by Sunshine host identity plus app ID. The TV never connects
to Sunshine or receives a Sunshine URL.

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
    "hdr": true
  },
  "audio": {"channels": 2}
}
```

`codec` is exactly `"h264"` or `"hevc"`. HEVC with `hdr: false` selects Main Profile
8-bit SDR with Rec.709. HEVC with `hdr: true` selects Main10 HDR with Rec.2020 and is
accepted only at 1920x1080, 2560x1440, or 3840x2160. H.264 HDR and 720p HDR are rejected.
There is no silent codec or SDR fallback. AV1 is not part of protocol version 1. The
Gateway validates every field and rejects unsupported settings. Status transitions use
`session-status` with one of `idle`, `starting`, `connecting-sunshine`,
`starting-moonlight`, `starting-webrtc`, `streaming`, `stopping`, or `error`.

For HDR, the H.265 SDP format parameters explicitly request Main10 (`profile-id=2`),
Main tier, and level 4.1 at 1080p60, level 5.0 at 1440p60, or level 5.1 at 4K60. The
Gateway rejects the session if the Tizen answer does not preserve that profile instead
of sending Main10 under a Main 8-bit negotiation.

The Gateway offers the standard WebRTC RTP color-space extension and records whether
Tizen negotiates it. It intentionally does not transmit the extension on this Samsung
runtime: physical validation showed zero decoded frames when the negotiated BT.2020/PQ
extension bytes were present, while the identical Main10 stream decoded at 60 fps when
relying on its HEVC VUI/SEI metadata. The encoded access units remain unchanged.

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

This is a local client disconnect only. It does not call Sunshine `/cancel`, so a running
Sunshine application remains available for a later `start-session`; the Gateway selects the
Moonlight-compatible `/resume` request when `serverinfo` reports that the same app is active.

## Stop or switch a running Sunshine application

The TV asks the Gateway to stop the host-side application with:

```json
{"version":1,"type":"stop-host-session"}
```

The Gateway first closes any local stream through the normal cleanup path, then uses its paired,
certificate-pinned GameStream HTTPS client to call Sunshine `/cancel`. It polls authenticated
`serverinfo` until `currentgame` is no longer active before reporting `host-session-status` with
`"state":"stopped"`.

Switching is one ordered Gateway operation. The Gateway validates the target, cancels the current
application, verifies Sunshine is idle, and only then starts the existing launch flow:

```json
{
  "version":1,
  "type":"switch-session",
  "appId":"7",
  "video":{"width":1920,"height":1080,"fps":60,"codec":"hevc","bitrateKbps":20000,"hdr":false},
  "audio":{"channels":2}
}
```

`host-session-status` carries the small UI transition state (`stopping`, `switching`, `starting`,
`stopped`, or `failed`) and optional running/target app IDs. The TV never contacts Sunshine
directly.

## Errors

```json
{"version":1,"type":"error","requestType":"start-session","code":"unsupported-settings","message":"Unsupported resolution"}
```

Errors contain no certificate, identity, or pairing secret.
