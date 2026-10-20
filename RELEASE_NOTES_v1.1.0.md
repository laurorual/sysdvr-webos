# v1.1.0 beta

This beta expands SysDVR WebOS into a dual-backend Nintendo Switch streaming client while keeping the existing app identity and package ID.

## Added

- Switch Frame Tap Network backend (UDP 9951 discovery, TCP 9950, SFTR v2).
- H.264 IDR/P-frame recovery and `SFTK` fresh-keyframe requests.
- Dynamic 720p/1080p decoder reconfiguration for Frame Tap.
- Low-latency PCM S16LE 48 kHz stereo playback for Frame Tap through SDL.
- Bounded audio queue that discards stale PCM after stalls to preserve perceived A/V sync.
- Unified discovery/selector showing SysDVR and Frame Tap as separate choices.
- TCP transport diagnostics including receive-queue depth and `TCP_INFO` fields when available.
- Bounded TCP connect timeout for faster recovery from dead or half-open Frame Tap sessions.

## Changed

- Version bumped to 1.1.0.
- README and architecture documentation now cover both streaming servers.
- Homebrew manifest generator now targets v1.1.0 and the dual-backend description.

## Notes

Switch Frame Tap itself is still early software. During development, intermittent stalls and connection drops were reproduced with its official PC client as well as the webOS client. The webOS backend automatically reconnects, but upstream sender instability can still interrupt playback.

1080p support is implemented but has not yet been tested by the maintainer because the development console is a Switch Lite.
