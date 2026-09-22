# Architecture — milestone 0.5

## Layers

```text
UI (SDL2 + SDL2_ttf)
        |
Session Manager
        |
Discovery / preferred-console policy
        |
A/V streaming session
        |
SysDVR protocol + NDL DirectMedia
```

The 0.5 work deliberately does not change packet handling or NDL feed logic.

## Session states

Conceptually:

```text
BROWSE
  |
  | OK
  v
CONNECTING
  |
  v
STREAMING
  | \
  |  \ Back
  |   -> BROWSE
  |
  | remote close
  v
WAIT_PREFERRED
  |
  | matching serial/IP appears
  v
RECONNECTING
  |
  v
STREAMING
```

## Discovery registry

A console is keyed by serial when available, otherwise by source IP.

Normal discovery entries expire after 7 seconds without a fresh UDP broadcast.
Manual-IP entries never expire.

## Preferred console

The first user-selected device is remembered in memory by:

1. serial, if advertised;
2. source IP as fallback.

Sleep/wake reconnection only accepts that preferred device.

The preference is not persisted across app launches yet.

## Rendering

The SDL renderer uses a 1920x1080 logical coordinate space so UI layout is
stable across TV output modes.

Before NDL playback begins, the UI renderer is cleared to transparent so the
hardware video plane becomes visible beneath the SDL window.

After the NDL session unloads, the UI is drawn opaque again.


## Back-navigation note

On the tested LG TV, the dedicated remote Back key is handled by webOS before
our SDL layer receives a usable action, which causes the platform close popup.
Therefore the app currently binds its internal return/cancel behavior to the
LEFT d-pad direction. The UI still uses a generic return-arrow icon rather than
a literal keycap label.


## RC1 UI assets

Icon PNGs are stored as source artwork but converted ahead of time to raw
RGBA8888 runtime resources. This avoids adding an SDL2_image dependency to the
native client while retaining alpha transparency and high-quality scaling.

`app_ui_init()` resolves the executable directory, loads the three runtime
assets and creates SDL textures. Failure is non-fatal because vector fallbacks
remain available.

## RC1 robustness review

- discovery parsing uses explicit `|` field splitting;
- stale-device pruning preserves selection identity by serial/IP;
- RC builds use a clean CMake directory;
- the streaming/NDL implementation remains frozen from the validated baseline.
