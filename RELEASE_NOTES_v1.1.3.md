# v1.1.3 beta — Homebrew catalog package ID migration

This release changes the app's package identity to comply with the webOSbrew
catalog's publisher namespace guidance. Streaming behavior and both streaming
backends are otherwise unchanged.

## Changed

- Change the package ID from `io.github.sysdvrwebos.client` to `io.github.laurorual.sysdvrwebos.client`.
- Update the webOS app metadata, NDL app-ID fallback, IPK filename, Homebrew
  manifest, standalone repository metadata and release workflow to use the
  new ID.
- Keep the official catalog submission and the standalone Homebrew repository
  aligned on the same package identity.

## One-time migration required

webOS and Homebrew Channel identify an installed app by its package ID.
Therefore, Homebrew Channel treats v1.1.3 as a separate application rather
than updating an installation made with v1.1.2 or older.

Users who installed an earlier version through Homebrew Channel must uninstall
the previous **SysDVR** entry first, refresh their repositories, and then
install v1.1.3 from the standalone repository or the official catalog.
Once v1.1.3 is installed, future updates work normally in place.

The v1.1.2 release and its assets remain available and unchanged.
