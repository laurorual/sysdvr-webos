# Known issues

- LG's dedicated Back button can be intercepted by webOS before SDL receives it. Use **Left** inside the app.
- Switch Frame Tap is still early software. During development, intermittent stalls and connection drops were reproduced with its official PC client too. The webOS client reconnects automatically, but cannot repair a stalled upstream sender.
- Frame Tap v0.7.20 advertises optional viewer controller input. The webOS client accepts the capability flag but does not transmit controller actions yet.
- Frame Tap crop flags are accepted for forward compatibility. The webOS client does not send SFTC and relies on the Switch to perform picture cropping/scaling.
- Switch Frame Tap 1080p decoding is implemented but has not yet been validated on the maintainer's Switch Lite; broader testing requires a dock-capable Switch.
- Some LG firmware logs harmless NDL/GStreamer warnings such as unsupported OMX properties or failed real-time scheduler requests.
