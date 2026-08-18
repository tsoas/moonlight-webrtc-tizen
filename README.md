# Moonlight WebRTC

> **Beta release preparation note:** the repository's project licence has not
> yet been declared. See [Licensing status](#licensing-status) before publicly
> redistributing a beta build.

Moonlight WebRTC streams games from a Sunshine host PC to a Samsung Tizen TV.
The TV connects only to the local Moonlight WebRTC Gateway; it never connects
to Sunshine directly.

```text
Sunshine PC -> Moonlight WebRTC Gateway -> WebRTC over LAN -> Samsung Tizen TV
```

The Gateway owns the Moonlight client identity, Sunshine pairing, application
list, artwork retrieval, and streaming session. The TV app is the remote-first
client used to select and play applications.

## Beta requirements

- A Windows PC running Sunshine and reachable from the TV on the same local
  network.
- Administrator permission to install the Windows Gateway.
- A compatible Samsung Tizen TV able to install a custom WGT package.
- [Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung) to install the
  supplied Tizen package. Follow its current instructions for TV developer
  mode, signing, and connection to the TV.

Download the two release assets with their stable names:

- `MoonlightWebRTC-Setup.exe` for Windows.
- `MoonlightWebRTC.wgt` for the Samsung TV.

## Install the Windows Gateway

1. Run `MoonlightWebRTC-Setup.exe` as an administrator.
2. Complete the installer. It installs the Gateway in
   `C:\Program Files\Moonlight WebRTC\`, configures and starts the
   **Moonlight WebRTC Gateway** service, and starts the tray companion for the
   installing interactive user.
3. Open **Moonlight WebRTC** from the Start menu or tray icon.

The installer creates an enabled, application-scoped inbound Windows Firewall
rule for the Gateway executable. It applies on all Windows firewall profiles
but only accepts remote addresses on `LocalSubnet`. Do not disable Windows
Firewall and do not change the Windows network profile merely for this app.

## Connect and pair Sunshine

1. Open the **Sunshine** page in the Windows tray application.
2. Enter the host name or LAN address of the PC running Sunshine and select
   **Test Connection**.
3. Select **Pair**. Enter the displayed PIN in Sunshine when requested.
4. Wait for the page to show that pairing is complete.

Use **Save** to persist the configured Sunshine host. Pairing uses the
Gateway's Moonlight identity and certificate-pinned Sunshine connection; keep
the Gateway data directory intact when you want to retain a pairing.

## Install the Samsung TV app

1. Download `MoonlightWebRTC.wgt` from the beta release.
2. In Apps2Samsung, choose its **Custom WGT** installation option and select
   `MoonlightWebRTC.wgt`.
3. Follow Apps2Samsung's on-screen instructions to select the TV and complete
   its required developer-mode/signing steps.
4. Start **Moonlight WebRTC** on the TV.

This beta path uses the supplied custom WGT with Apps2Samsung. Tizen Studio is
not required for normal beta installation.

## Add the Gateway on the TV

Gateway discovery is not available in this beta. In the TV app, add the
Gateway manually using the Windows PC's LAN IPv4 address. The editor is
remote-friendly: select an octet with Left/Right and change it with Up/Down.
The app uses the Gateway's standard port internally; no port entry is needed.

## Stream a game

1. Confirm the Windows Gateway is running and paired with Sunshine.
2. In the TV app, select the saved Gateway and wait for the application list.
3. Select an application and start the session.
4. Use the TV remote or a supported gamepad to control the app and the
   streaming overlay as appropriate.

Disconnecting ends the local TV-to-Gateway stream only. It does not
necessarily stop the Sunshine application. Use the TV streaming controls when
you intend to stop the host-side session.

## Uninstall, reinstall, and persistent data

The Windows uninstaller removes the service, firewall rule, tray autostart,
Start menu entry, and installed program files. It intentionally preserves the
Gateway's persistent data in:

```text
%PROGRAMDATA%\MoonlightWebRTC
```

This directory contains the Moonlight identity, certificate/private key,
pairing state, and saved Sunshine host configuration. Do not share or publish
its contents. Retaining it lets an uninstall/reinstall or a later upgrade keep
the existing pairing.

## Current beta limitations

- Streaming frame rate is currently fixed at 60 FPS.
- Gateway discovery is not implemented; add the Gateway manually on the TV.
- Wake-on-LAN is not implemented.
- 1440p streaming, including HDR, remains experimental.

## Verify release downloads

The release includes `SHA256SUMS.txt` for the two public assets. In PowerShell:

```powershell
Get-FileHash .\MoonlightWebRTC-Setup.exe -Algorithm SHA256
Get-FileHash .\MoonlightWebRTC.wgt -Algorithm SHA256
```

Compare the displayed hashes with `SHA256SUMS.txt`. Release maintainers can
regenerate that file from a completed local build with:

```powershell
powershell -ExecutionPolicy Bypass -File packaging\release\generate-checksums.ps1
```

## Licensing status

This repository currently has no top-level project licence. It also incorporates
the GPL-3.0-licensed `moonlight-common-c` component. A project licence and a
corresponding distribution-compliance review are required before this project
can be publicly released. This README does not select a licence. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the audited dependency
inventory and notice obligations.
