# Publishing to webOS Homebrew

1. Publish the source repository on GitHub.
2. Build the final ARM package with `scripts/build-webos.sh`.
3. Create a GitHub Release tagged `v1.0.0` and upload the generated `_arm.ipk`.
4. Generate the Homebrew manifest:

   ```bash
   ./scripts/gen-webosbrew-manifest.py \
     --source-url https://github.com/YOUR_GITHUB_USERNAME/REPOSITORY_NAME \
     --tag v1.0.0
   ```

5. Upload `dist/io.github.sysdvrwebos.client.manifest.json` to the same GitHub
   Release.
6. Fork `webosbrew/apps-repo`, copy
   `webosbrew/io.github.sysdvrwebos.client.yml.example` to
   `packages/io.github.sysdvrwebos.client.yml`, replace the URL placeholders,
   commit it and open a pull request.
7. In the pull request, disclose AI assistance honestly and describe the manual
   development/testing performed on real hardware.

The app does not require root; it was developed and tested through webOS
Developer Mode.
