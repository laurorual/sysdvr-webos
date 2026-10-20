# Architecture

`sysdvr-webos` keeps one webOS UI/session layer and selects one of two streaming backends per discovered entry.

## Discovery

- SysDVR: UDP 19999 (`SysDVR|...`)
- Switch Frame Tap: UDP 9951 (`SFTAP1 ...`)

The source IP of each datagram is treated as authoritative. The session registry keeps the backend identity as part of the device key, so SysDVR and Frame Tap running on the same Switch can coexist as two selector entries.

## SysDVR path

```text
TCP 9911 video ─┐
                 ├─ SysDVR protocol parser ─ NDL DirectMedia shared timeline
TCP 9922 audio ─┘
```

Both streams use SysDVR timestamps and feed the same DirectMedia player.

## Switch Frame Tap path

```text
TCP 9950 / SFTR v2
       ├─ H.264 ─ keyframe/sequence recovery ─ NDL DirectMedia video-only
       └─ PCM 48 kHz stereo ─ bounded SDL audio queue
```

Frame Tap does not currently expose a shared presentation clock comparable to SysDVR. The client therefore keeps audio latency bounded instead of allowing stale PCM to accumulate across transport stalls.

The video backend requests a fresh keyframe with `SFTK` after discontinuities, waits for an IDR before resuming dependent P-frames, and recreates the NDL decoder when the encoded resolution changes.

## Reconnect policy

Transport failures return to the common preferred-console reconnect state. Frame Tap TCP connection attempts use a bounded timeout, preventing a dead sender from blocking the UI for the operating system's full TCP connect timeout.

## Logging

The app uses a single launch log. Frame Tap adds slow-payload timing, receive-queue depth and TCP state/RTT metrics where the platform exposes them.
