# Known issues — 1.0.0

## LG Back key

On the tested LG TV, the dedicated remote-control Back key is intercepted by
webOS and opens the platform close-app dialog before the application can use it
as normal SDL navigation.

Use the remote's **Left** directional key for in-app return/cancel.

## Vendor warnings

Some LG NDL/GStreamer/PowerVR warnings can appear in the diagnostic log during
startup even when streaming works correctly. They are intentionally retained in
the log because they may be useful when diagnosing behavior on other TV models.
