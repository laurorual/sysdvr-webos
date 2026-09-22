Runtime icon assets are raw RGBA8888 files generated from the corresponding
PNG artwork under assets/source/.

Dimensions:
  icon_left.rgba    240x120
  icon_ok.rgba      180x100
  icon_gamepad.rgba 240x180

The native client loads these directly through SDL2, avoiding a runtime
dependency on SDL2_image.
