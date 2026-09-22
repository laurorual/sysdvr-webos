#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN="${WEBOS_TOOLCHAIN_FILE:-/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake}"
BUILD="$ROOT/build-webos"
PKG="$ROOT/build-webos-package"
DIST="$ROOT/dist"

[[ -f "$TOOLCHAIN" ]] || {
  echo "Toolchain not found: $TOOLCHAIN"
  exit 1
}

# Always configure from a clean build tree to avoid stale toolchain state.
rm -rf "$BUILD"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DSYSDVR_BUILD_WEBOS=ON \
  -DSYSDVR_BUILD_HOST_TEST=OFF \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD" --target sysdvr-webos

rm -rf "$PKG"
mkdir -p "$PKG" "$DIST"

cp "$BUILD/sysdvr-webos" "$PKG/"
cp \
  "$ROOT/webos/appinfo.json" \
  "$ROOT/webos/icon.png" \
  "$ROOT/webos/switch_ip.txt" \
  "$PKG/"

if [[ -d "$ROOT/webos/assets" ]]; then
  cp -R "$ROOT/webos/assets" "$PKG/assets"
fi

chmod +x "$PKG/sysdvr-webos"

if command -v ares-package >/dev/null 2>&1; then
  rm -f "$DIST"/*.ipk
  ares-package -A arm "$PKG" -o "$DIST"
  echo
  echo "IPK created under $DIST"
else
  echo
  echo "Built OK. ares-package not found; staged package is in $PKG"
fi
