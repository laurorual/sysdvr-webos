#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEVICE="${1:-${ARES_DEVICE:-}}"
[[ -n "$DEVICE" ]] || { echo "usage: $0 <ares-device-name>"; exit 2; }
IPK="$(find "$ROOT/dist" -maxdepth 1 -type f -name '*.ipk' | head -n1 || true)"
[[ -n "$IPK" ]] || { echo "No IPK; run scripts/build-webos.sh first"; exit 1; }
ares-install -d "$DEVICE" "$IPK"
