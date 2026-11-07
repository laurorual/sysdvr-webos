# v1.1.2 beta — Homebrew Channel checksum fix

This is a **packaging and distribution fix**. Playback, discovery, codecs,
audio synchronization and both SysDVR / Switch Frame Tap backends are unchanged.

## Fixed

- Resolve *Invalid file checksum* during Homebrew Channel updates by publishing
  a fresh version/tag with one stable IPK and a matching SHA-256 manifest.
- Protect future releases: the publishing workflow **refuses to overwrite an
  existing version or GitHub release asset**, even when manually dispatched.
- Restrict automatic release builds to app sources, build tooling and version
  changes. Editing catalog metadata (`repo.json`), README or log-collector
  scripts no longer silently republishes the same app version.
- Cross-check the manifest's SHA-256 and package size against the IPK before
  publication, and verify GitHub's published asset digest afterwards.
- Upload the release assets before updating the tracked manifest in `main`,
  preventing a tracked manifest from temporarily pointing to an unpublished IPK.

## Why

On November 6, 2026, adding the standalone Homebrew `repo.json` caused
the v1.1.1 workflow to rerun with unchanged version metadata. The workflow
overwrote the v1.1.1 IPK and manifest. Devices with cached metadata for the
original v1.1.1 package could then compare its old SHA-256 against the new
binary and reject the installation.

## Updating

Reopen the Homebrew Channel and refresh repositories to pick up v1.1.2.
Both the custom repository and the official webOSbrew listing use
`releases/latest/download/...` to locate the new manifest.

The app ID remains `io.github.sysdvrwebos.client`, so installed versions
are upgraded in place. The release remains a normal GitHub Release (labeled
beta in its title) so `releases/latest` continues to resolve correctly.
