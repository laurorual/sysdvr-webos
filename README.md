# SysDVR WebOS

A native **SysDVR client for LG webOS TVs**.

It receives video and audio directly from a Nintendo Switch running SysDVR in
**TCP Bridge** mode and sends the H.264 stream to the LG hardware decoder using
NDL DirectMedia.

> This is an unofficial community project and is not affiliated with Nintendo,
> LG, or SysDVR.

## Features

- H.264 hardware-decoded video
- PCM stereo audio at 48 kHz
- automatic SysDVR discovery over UDP
- console-selection UI
- automatic reconnect after Switch sleep/wake
- multiple-console discovery
- low-latency TCP Bridge streaming
- detailed diagnostic logging
- no FFmpeg required on the TV

## Requirements

- LG TV with **webOS 5+**
- Developer Mode enabled
- Nintendo Switch running SysDVR
- SysDVR configured for **TCP Bridge**
- Switch and TV on the same local network
- 5 GHz Wi-Fi or Ethernet is recommended

## Controls

| Remote input | Action |
| --- | --- |
| Up / Down | Select console |
| OK | Connect |
| Left | Return / cancel |
| Left while streaming | Return to console selector |

The dedicated LG **Back** button may be intercepted by webOS and show the
system close-app dialog, so the application uses **Left** as its in-app return
action.

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

A manual Switch IP can optionally be supplied:

```bash
./scripts/launch-test.sh tv 192.168.1.50
```

Normally this is unnecessary because the app discovers SysDVR automatically.

## Diagnostic log

```bash
./scripts/show-log.sh tv
```

The app keeps **one log file only**:

```text
sysdvr-webos.log
```

It is replaced on every app launch and contains timestamped information about:

- application/session state
- discovered consoles
- remote-control inputs
- TCP connection and handshake timing
- audio/video packet progress
- A/V timestamps
- network gaps
- DirectMedia backpressure/timing
- reconnects and session shutdowns
- relevant webOS/GStreamer/NDL warnings

To avoid unnecessary storage use, persistent capture stops after **60 minutes**
or **4 MiB**, whichever comes first. Streaming continues normally after logging
stops.

## Credits

- [exelix11](https://github.com/exelix11) for making this awesome app and making it easy to port.
- ChatGPT for making the port process and diagnose it way easier.
