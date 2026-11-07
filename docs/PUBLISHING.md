# Publishing to webOS Homebrew

Releases are built and published by [GitHub Actions](../.github/workflows/release.yml).

## Integrity and immutable release policy

**Never overwrite or rebuild an existing version/tag.** The Homebrew Channel
can cache package metadata; replacing the IPK at the same URL with different
bytes invalidates the checksum users previously received.

1. Bump the same version in `VERSION`, `CMakeLists.txt` and
   `webos/appinfo.json`. For a packaging-only fix, increment the patch version.
2. Add `RELEASE_NOTES_vX.Y.Z.md` with the changelog.
3. Push to `main`. Source changes automatically trigger the release workflow.
4. The workflow rejects an already-published tag, executes host protocol
   tests, builds the ARM IPK, and creates the Homebrew manifest from **that
   exact file**.
5. It publishes the new GitHub release **without using `--clobber`**.
   It checks the published GitHub asset SHA-256 against the locally built
   package before committing the generated manifest to `main`.
6. The community catalog and independent `repo.json` both point to
   `releases/latest/download/io.github.sysdvrwebos.client.manifest.json`.
   The release should **not** be marked GitHub Pre-release while that endpoint
   is used; use `beta` in the release title/notes instead.
7. If a release build fails before publication, fix the build and re-run it.
   If a version was already published, **bump the version** instead of
   overwriting its assets.

The app does not require root; it was developed and tested using webOS
Developer Mode.

## Troubleshooting: "Invalid file checksum"

Compare the current release IPK digest to `ipkHash.sha256` inside the
release's manifest. If they match, the TV may have cached a manifest from
an older build under the same tag. Refresh repositories or close/reopen the
Homebrew Channel, and publish a new version if the previous tag was
overwritten. Never disable checksum verification or replace the hash with
an unverified value.
