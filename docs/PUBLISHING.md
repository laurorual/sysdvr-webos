# Publishing to webOS Homebrew

1. Build the final ARM package with `scripts/build-webos.sh`.
2. Create a GitHub release tagged `v1.1.0` and title it `v1.1.0 beta` and upload the generated `_arm.ipk`.
3. Generate the Homebrew manifest:

   ```bash
   ./scripts/gen-webosbrew-manifest.py \
     --source-url https://github.com/laurorual/sysdvr-webos \
     --tag v1.1.0
   ```

4. Upload `dist/io.github.sysdvrwebos.client.manifest.json` to the same GitHub release.
5. The webOSbrew package entry may continue to use the `releases/latest/download/...manifest.json` URL.
6. Re-run the apps-repo checks after the release assets are available.

The manifest must be generated from the exact IPK uploaded to the release because `ipkHash` and `ipkSize` are validated by webOSbrew.

The app does not require root; it was developed and tested through webOS Developer Mode.

> Note: the GitHub release itself should remain a normal release (not marked as a GitHub prerelease) so the existing `releases/latest/download/...` Homebrew manifest URL resolves to v1.1.0. The beta status is communicated in the release title/notes.
