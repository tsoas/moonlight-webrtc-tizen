# Moonlight WebRTC Gateway protocol

Protocol version 2 is a local JSON protocol carried by the existing signaling WebSocket on
port 8000. Every message contains `"version": 2` and a `type`. Version 2 made TV
authentication mandatory; the Gateway rejects version 1 messages, and the TV reports a
version 1 Gateway as out of date instead of using it. The WebSocket transports
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
{"version":1,"type":"status","serviceRunning":true,"sunshineConnected":true,"sunshinePaired":true,"sunshineHost":"192.168.1.20:27786","sunshineName":"Sunshine-PC","runningApplicationId":"7","runningApplicationName":"Desktop","sessionActive":false,"connectedTvClients":0,"pairedTvClients":1}
```

`sunshineHost` is the configured address the Gateway dials, including a custom port when
one was set. `sunshineName` is the name Sunshine reports for itself; it is a label and is
not necessarily resolvable, so it is never fed back into the address field.

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
accepts only `set-host`, `test`, `pair`, `pair-status`, `unpair`, `pair-tv`, `pair-tv-status`,
and `unpair-tvs` commands; every response is a structured `result` with
the requested command, `ok`, a stable code, and a user-safe message. The service remains
the sole writer of ProgramData and pairing material. `set-host` writes only
`sunshine-host.txt`; `test` first validates Sunshine's protocol response and, when paired,
performs an authenticated request using the existing pinned certificate. `pair` returns a
new four-digit PIN only over this authenticated local pipe, starts the existing Moonlight
pairing flow asynchronously, and is followed with `pair-status`; the PIN is never logged
or persisted. Sunshine's `unpair` request is only available for cancelling an in-progress
pairing (a completed pairing returns HTTP 404), so `unpair` accurately performs local trust
removal only: it removes the matching host entry from `paired-hosts.json`. It never deletes
the Gateway identity or ProgramData directory, and does not claim Sunshine-side revocation.

`pair-tv` opens a two-minute TV pairing window and returns its four-digit PIN over the same
authenticated pipe; the tray shows it and polls `pair-tv-status` (`tv-pairing-waiting`,
`tv-paired`, `tv-pairing-expired`, `tv-pairing-locked`, or `tv-pairing-idle`). `unpair-tvs`
removes every paired TV and closes the connected TV, so a lost TV is cut off at once.

## TV authentication

Every TV must pair once before the Gateway serves it. An unauthenticated connection can only
authenticate or pair: any other request is answered with a `not-authenticated` error, and the
connection does not displace the TV that is streaming. Up to eight unauthenticated
connections wait at a time; the oldest is dropped beyond that.

On every connection the Gateway first sends a single-use 256-bit nonce:

```json
{"version":2,"type":"auth-required","nonce":"<64 hex digits>","macAddress":"2C:F0:5D:7B:E6:D0","sunshineAvailable":true}
```

`macAddress` is the Wake-on-LAN address described below. It is offered before authentication
because the LAN already sees it through ARP, and it lets the TV's reachability probe (which
never authenticates, so that it cannot displace a streaming TV) learn how to wake the PC.

`sunshineAvailable` tells the same probe whether the Gateway can reach a paired Sunshine, so the
TV can show a Gateway that is running but cannot stream as "Sunshine unavailable" rather than
Online. The Gateway refreshes it in the background every five seconds instead of checking per
connection, so it never delays this message; it is omitted until the first check completes, and
a TV treats a missing field as available.

The TV keeps that probe connection open for as long as it shows the Gateway, without answering
`auth-required`, and the Gateway pushes every change of the same value on it:

```json
{"version":2,"type":"sunshine-availability","sunshineAvailable":false}
```

The message goes to every connection, so an authenticated TV may receive it too and can ignore
it: the Gateway also sends that TV a fresh `gateway-status`, followed by the `apps` list when
Sunshine has become available. When the probe connection drops, the TV marks the Gateway
offline and reconnects every five seconds.

A paired TV answers with its client ID and an HMAC-SHA256, keyed with its secret, over the
ASCII label `moonlight-webrtc-tv-auth-v1:` followed by the nonce in lower-case hex:

```json
{"version":2,"type":"authenticate","clientId":"<32 hex digits>","proof":"<64 hex digits>"}
```

The Gateway compares the proof in constant time and replies with `authenticated`, then the
normal startup messages below. The secret never crosses the network again, and a captured
proof cannot be replayed against another nonce. An unknown client or wrong proof returns an
`error` with `requestType` `authenticate` and code `authentication-failed`; the TV then
forgets its credentials and asks to pair.

### Pairing a TV

Pairing is opened on the PC: **TVs › Pair TV** in the tray (or `--pair-tv` in console mode,
which prints the PIN to the console, never to the log). This shows a four-digit PIN that is
valid for two minutes and three attempts; after that the window closes and must be opened
again, so the 10,000 PINs cannot be guessed from the network. The TV sends:

```json
{"version":2,"type":"pair-client","pin":"0421","clientName":"Samsung TV"}
```

On success the Gateway issues a random 128-bit client ID and 256-bit secret, stores them in
`tv-clients.json` in the service data directory (beside the Gateway identity and protected by
the same ACL), and replies:

```json
{"version":2,"type":"paired","clientId":"<32 hex digits>","clientSecret":"<64 hex digits>"}
```

followed by the normal startup messages. Failures return an `error` with `requestType`
`pair-client` and code `pairing-not-open`, `incorrect-pin`, `too-many-attempts`, or
`pairing-failed`. The Gateway keeps at most 32 paired TVs, dropping the oldest.

### Pairing with Sunshine's built-in server

[Sunshine-Web-RTC](https://github.com/mlopezsegura/Sunshine-Web-RTC) serves this protocol from
inside Sunshine and pairs TVs the way Moonlight pairs: the TV shows the PIN and the user enters
it in Sunshine's Web UI. It says so in its greeting with `"pairing":"client-pin"`; a server
without the field (this Gateway) shows the PIN itself and expects `pair-client`.

An unpaired TV picks a random four-digit PIN, shows it, and sends:

```json
{"version":2,"type":"request-pairing","pin":"0421","clientName":"Samsung TV"}
```

Sunshine lists the TV under **PIN** beside Moonlight clients waiting to pair, by name and address
and under an unguessable request ID, and keeps the request for five minutes. When the user
selects the TV and enters the PIN, Sunshine replies with `paired` as above and the name typed
in the Web UI becomes the TV's name. A wrong PIN ends the request. Otherwise the request fails
with an `error` whose `requestType` is `request-pairing` and whose code is one of:

- `incorrect-pin`;
- `pairing-cancelled`, when the request is declined in the Web UI;
- `pairing-expired`;
- `pairing-failed`.

After any of these the TV can send a new request. The TV app does so at once with a new PIN,
except after a cancellation, which waits until the user chooses **New PIN**. Because only a
signed-in user can approve the PIN, there is no attempt limit to guess against. Sunshine
answers `pair-client` with the error code `unsupported-pairing`, which tells an older TV app to
update.

Paired TVs appear in Sunshine's client list under **Troubleshooting**, next to Moonlight clients, and
can be disabled or unpaired there. Unpairing a TV removes its credentials, so its next `authenticate`
fails with `authentication-failed` and the TV pairs again. Disabling a TV keeps it paired, but
`authenticate` fails with code `client-disabled`. In that case the TV keeps its credentials and
reports that it is disabled, and it works again once the TV is re-enabled.

The WebSocket itself is not encrypted, so the secret is visible to a passive observer on the
LAN during that one pairing exchange. Authentication stops other devices on the network from
using or interrupting the Gateway; it does not make the stream confidential.

The test media source (`--source=test`) has no data directory and does not authenticate.

## Gateway startup

Opening the WebSocket does not start Sunshine or WebRTC. Once the TV is authenticated, the Gateway sends:

```json
{"version":2,"type":"gateway-status","gatewayName":"Sunshine-PC","sunshineDetected":true,"sunshinePaired":true,"sessionActive":false,"macAddress":"2C:F0:5D:7B:E6:D0"}
```

`macAddress` is the Wake-on-LAN address of the Gateway PC: the MAC address of the local
adapter Windows routes to this TV through, in upper-case colon form. It is omitted when that
adapter has no Ethernet-style address (loopback, some virtual adapters). The TV stores it with
the saved Gateway and later wakes the PC itself: the Gateway cannot, since it is asleep too.

### Wake-on-LAN

A Tizen web application cannot send UDP from JavaScript, so the TV sends the magic packet from
a small WebAssembly module (`tizen/wasm/wake-on-lan.c`) built with Samsung's Emscripten fork,
whose Tizen Sockets extension provides POSIX sockets. Those sockets are refused on the browser
main thread, so each request runs on its own pthread. One request sends the standard 102-byte
magic packet to UDP port 9 at the IPv4 limited broadcast `255.255.255.255`, at the Gateway's
last known IPv4 address, and at the IPv6 all-nodes group `ff02::1`; it succeeds when at least
one of those sends does.

Selecting an offline Gateway whose address is known wakes it, as does **Wake PC** in its
menu. The TV then keeps reconnecting, with 5-second attempts, for up to two minutes while the
PC boots, and opens the application library once `gateway-status` arrives.

It also sends `capabilities`. Protocol version 2 advertises explicit `videoModes` so the
TV never has to infer a resolution/codec combination. Each mode contains `width`,
`height`, `fps`, `codecs`, `hdrCodecs`, `defaultCodec`, `defaultBitrateKbps`, `experimental`,
`hdrSupported`, and `hdrExperimental`. `hdrCodecs` lists the codecs of `codecs` that may be
combined with HDR in that mode; a TV that does not find it assumes HEVC only.

| Mode | Codecs | HDR | Default | Default bitrate | Experimental |
| --- | --- | --- | --- | ---: | --- |
| 1280x720 @ 60 | H.264, HEVC, AV1 | Off | H.264 | 12000 kbps | No |
| 1920x1080 @ 60 | H.264, HEVC, AV1 | Off, On | H.264 | 20000 kbps | HDR only |
| 2560x1440 @ 60 | H.264, HEVC, AV1 | Off, On | HEVC | 30000 kbps | Yes |
| 3840x2160 @ 60 | HEVC, AV1 | Off, On | HEVC | 50000 kbps | HDR only |

AV1 is listed only when Sunshine advertises an AV1 Main 8-bit encoder (`ServerCodecModeSupport`
bit `SCM_AV1_MAIN8`), which needs a GPU that can encode it. The Gateway learns this when it
reaches Sunshine, so `capabilities` is sent again when Sunshine becomes available, and
whenever Sunshine's advertised encoders change while it stays available (for example after
Sunshine restarts and its hardware encoder passes the startup probe it failed before). The TV
further hides AV1 when `RTCRtpReceiver.getCapabilities("video")` does not list `video/AV1`.

The selectable bitrates are 10000, 12000, 15000, 20000, 25000, 30000, 40000, 50000,
60000, 80000, and 100000 kbps. HDR defaults to off and is experimental when explicitly selected with HEVC or AV1. AV1 HDR
is listed in `hdrCodecs` only when Sunshine also advertises AV1 Main10 (`SCM_AV1_MAIN10`).
Audio remains stereo Opus at 48 kHz and frame rate remains fixed at 60 fps. The entire
1440p mode, including HDR, is experimental because Samsung does not list it in the
official Cloud Gaming resolution table.

## Applications

Client request:

```json
{"version":2,"type":"get-apps"}
```

Gateway response (entries come directly from Sunshine):

```json
{"version":2,"type":"apps","apps":[{"id":"0","title":"Desktop","artworkAvailable":false,"running":false}]}
```

`id` remains Sunshine's exact opaque application ID as returned by `/applist`; the Gateway
does not derive it from application order or convert it before an artwork request. `running` is derived from Sunshine's
authenticated `serverinfo` current application while the server reports an active stream.
`artworkAvailable` reports only the Gateway's current in-memory cache state, so the
application list stays small and renders without waiting for image downloads.

Artwork is fetched lazily through the same Gateway WebSocket. The TV requests one asset at a
time:

```json
{"version":2,"type":"get-app-artwork","appId":"0"}
```

The Gateway retrieves Sunshine's authenticated GameStream `appasset` resource with
`appid`, `AssetType=2`, and `AssetIdx=0`, using the existing Moonlight identity and pinned
Sunshine certificate. It validates JPEG, PNG, or WebP media before returning a display-only
response:

```json
{"version":2,"type":"app-artwork","appId":"0","available":true,"mimeType":"image/jpeg","data":"...base64..."}
```

Missing, malformed, or unavailable artwork returns `"available":false`. This never fails
the application list; the TV keeps its normal fallback card. The Gateway caches successful
and unavailable results in memory by Sunshine host identity plus app ID. The TV never connects
to Sunshine or receives a Sunshine URL.

## Start a session

```json
{
  "version": 2,
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

`codec` is exactly `"h264"`, `"hevc"`, or `"av1"`. HEVC with `hdr: false` selects Main Profile
8-bit SDR with Rec.709. HEVC with `hdr: true` selects Main10 HDR with Rec.2020 and is
accepted only at 1920x1080, 2560x1440, or 3840x2160. H.264 HDR and 720p HDR are rejected.
AV1 with `hdr: false` selects Main 8-bit SDR with Rec.709; AV1 with `hdr: true` selects Main
10-bit HDR with Rec.2020, at the same resolutions as HEVC HDR. AV1 is sent over RTP per
RFC 9628 (`AV1/90000`) with no profile in the SDP, since the Main profile covers 10-bit. The
Gateway checks the first AV1 sequence header for Main, 10-bit, 4:2:0, as it checks the HEVC
SPS. A session asking for AV1, or AV1 HDR, fails when Sunshine does not advertise it.
There is no silent codec or SDR fallback. The
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
{"version":2,"type":"stop-session"}
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
{"version":2,"type":"stop-host-session"}
```

The Gateway first closes any local stream through the normal cleanup path, then uses its paired,
certificate-pinned GameStream HTTPS client to call Sunshine `/cancel`. It polls authenticated
`serverinfo` until `currentgame` is no longer active before reporting `host-session-status` with
`"state":"stopped"`.

Switching is one ordered Gateway operation. The Gateway validates the target, cancels the current
application, verifies Sunshine is idle, and only then starts the existing launch flow:

```json
{
  "version":2,
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
{"version":2,"type":"error","requestType":"start-session","code":"unsupported-settings","message":"Unsupported resolution"}
```

Errors contain no certificate, identity, or pairing secret.
