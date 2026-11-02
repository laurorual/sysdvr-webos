# v1.1.1 beta — Frame Tap 0.7.20 compatibility

This update refreshes the Switch Frame Tap Network backend while preserving the
original SysDVR TCP Bridge backend, app ID, UI and installation workflow.

## Fixed / improved

- Accept the **v0.7.20 viewer-input capability bit (0x80)** on SFTR v2 headers.
  The old client rejected it as an unsupported flag when the Switch's optional
  *Controls from the viewer* feature was enabled. This release accepts the bit
  without sending controller inputs.
- Accept the **v0.7.16 crop bits (0x20/0x40)** on video headers for forward
  compatibility. We deliberately do not opt into client-side cropping (SFTC);
  the Switch remains responsible for preparing the picture for webOS.
- Show the console's **status reason headline** on the waiting screen
  (no game, paused/keyboard, unsupported source, etc.). Record the full
  headline and detail in the diagnostic log.
- Add **host SFTR regression tests** for old and new flag combinations; run
  these tests in GitHub Actions before building the ARM IPK.
- Make release versioning read VERSION/webos/appinfo.json instead of embedding
  1.1.0 in the build and Homebrew manifest scripts.

## Context

Switch Frame Tap v0.7.20 (2026-11-01) also adds optional gamepad input,
upstream performance/color fixes and sender-side keyframe changes. Those
improvements happen on the Switch itself; the webOS app remains video/audio
only. v0.7.20.1-beta1 (2026-11-02) contains additional console-side picture
fixes, with no new receiver protocol requirement identified.

**Known limitations:** 1080p awaits dock-capable Switch testing; instability
on the Switch/source side is outside the receiver's control; controller
forwarding and USB access are not implemented.

Upstream releases:
- https://github.com/TomasUribe/switch-frame-tap/releases/tag/v0.7.20
- https://github.com/TomasUribe/switch-frame-tap/releases/tag/v0.7.20.1-beta1
