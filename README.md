# SysDVR WebOS

A native Nintendo Switch streaming client for **LG webOS TVs**.

The app now supports two streaming backends:

- **SysDVR** in TCP Bridge mode (720p30 H.264 + PCM audio)
- **Switch Frame Tap** in Network mode (H.264 + PCM audio, up to 720p60/1080p60 depending on the console/game)

Both backends use the LG hardware H.264 decoder through NDL DirectMedia. Switch Frame Tap audio uses a bounded low-latency SDL queue so stale PCM is discarded after stalls instead of drifting behind the video.

> This is an unofficial community project and is not affiliated with Nintendo,
> LG, SysDVR, or Switch Frame Tap.

## Features

- H.264 hardware-decoded video through NDL DirectMedia
- PCM stereo audio at 48 kHz
- automatic LAN discovery for both SysDVR and Switch Frame Tap
- unified console/server-selection UI
- automatic reconnect after sleep, server restart, or transient transport failure
- multiple-console discovery
- detailed diagnostic logging and TCP transport metrics
- no FFmpeg required on the TV

### SysDVR backend

- Uses TCP Bridge ports 9911 (video) and 9922 (audio)
- Protocol 02/03 support
- Shared NDL audio/video timeline for timestamp-based A/V sync
- 720p30 capture from the Switch capture service

### Switch Frame Tap backend (beta)

- Uses UDP 9951 discovery + TCP 9950 streaming
- SFTR v2 parser
- H.264 IDR/P-frame recovery with fresh-keyframe requests (`SFTK`)
- dynamic 720p/1080p decoder reconfiguration
- PCM S16LE stereo 48 kHz through SDL audio
- bounded audio latency after transport stalls
- transport diagnostics for slow payloads, receive-queue depth and TCP state
- bounded TCP connect timeout so failed reconnect attempts do not hang for many seconds

Switch Frame Tap itself is still under active development. During testing, transport stalls were reproduced with its official PC client as well as this webOS client, so current stream instability can originate on the Switch/server side rather than the TV client.

## Requirements

- LG TV with **webOS 5+**
- Developer Mode or a compatible homebrew environment
- Nintendo Switch on the same local network
- One of:
  - SysDVR configured for **TCP Bridge**, or
  - Switch Frame Tap configured for **Network** streaming
- 5 GHz Wi-Fi or Ethernet is recommended

A Switch Lite can use Switch Frame Tap at 720p. 1080p support is implemented in the webOS client but still needs broader real-hardware testing with dock-capable consoles.

## Controls

| Remote input | Action |
| --- | --- |
| Up / Down | Select a discovered server |
| OK | Connect |
| Left | Return / cancel |
| Left while streaming | Return to server selector |

The dedicated LG **Back** button may be intercepted by webOS and show the
system close-app dialog, so the application uses **Left** as its in-app return
action.

## Discovery and server selection

SysDVR and Switch Frame Tap are discovered at the same time. If both servers
are available on the same Switch, they appear as separate selectable entries so
you can choose the backend explicitly.

A manual IP supplied through `launch-test.sh` is currently treated as a
**SysDVR** target. Switch Frame Tap is normally found automatically through its
UDP beacon.

## Build

Set the webOS native toolchain path and build:

```bash
WEBOS_TOOLCHAIN_FILE="/path/to/toolchainfile.cmake" \
./scripts/build-webos.sh
```

The generated `.ipk` will be placed under `dist/` when `ares-package` is
available.

## Install

```bash
./scripts/install-webos.sh tv
```

Replace `tv` with the name configured in your `ares` device list.

## Run

```bash
./scripts/launch-test.sh tv
```

A manual SysDVR IP can optionally be supplied:

```bash
./scripts/launch-test.sh tv 192.168.1.50
```

Normally this is unnecessary because the app discovers supported servers automatically.

## Diagnostic log

```bash
./scripts/show-log.sh tv
```

To save the whole log instead of printing it in the terminal:

```bash
./scripts/show-log.sh tv > sysdvr-webos.log
```

The app keeps **one log file only**:

```text
sysdvr-webos.log
```

It is replaced on every app launch and contains timestamped information about:

- application/session state and selected backend
- discovered SysDVR and Switch Frame Tap servers
- remote-control inputs
- TCP connection/handshake timing
- audio/video packet progress
- SysDVR A/V timestamps
- Frame Tap sequence/keyframe recovery
- network gaps and slow payload reads
- receive-queue/TCP transport diagnostics when available
- DirectMedia backpressure/timing
- reconnects and session shutdowns
- relevant webOS/GStreamer/NDL warnings

To avoid unnecessary storage use, persistent capture stops after **60 minutes**
or **4 MiB**, whichever comes first. Streaming continues normally after logging
stops.

## Release status

**v1.1.0 is a beta release** because Switch Frame Tap support is new and the
upstream sysmodule is still evolving. SysDVR remains the mature/stable backend.

## Credits

- [exelix11](https://github.com/exelix11) for SysDVR.
- [TomasUribe](https://github.com/TomasUribe) for Switch Frame Tap and its public SFTR protocol/client implementation.
- The webOSbrew community for the native webOS toolchain and Homebrew ecosystem.
- ChatGPT for implementation, review and debugging assistance during the porting work.
