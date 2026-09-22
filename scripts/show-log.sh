#!/usr/bin/env bash
set -euo pipefail

DEVICE="${1:-${ARES_DEVICE:-}}"

if [[ -z "${DEVICE}" ]]; then
    echo "usage: $0 <ares-device-name>"
    exit 2
fi

APP_ID="io.github.sysdvrwebos.client"
LOG="/media/developer/apps/usr/palm/applications/${APP_ID}/sysdvr-webos.log"

ares-shell -d "${DEVICE}" --run \
    "if [ -f '${LOG}' ]; then cat '${LOG}'; else echo 'No SysDVR log found yet: ${LOG}'; fi"
