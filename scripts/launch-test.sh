#!/usr/bin/env bash
set -euo pipefail

DEVICE="${1:-${ARES_DEVICE:-}}"
HOST="${2:-}"

if [[ -z "${DEVICE}" ]]; then
    echo "usage: $0 <ares-device-name> [manual-switch-ip]"
    exit 2
fi

APP_ID="io.github.sysdvrwebos.client"
APP_DIR="/media/developer/apps/usr/palm/applications/${APP_ID}"
LOG="${APP_DIR}/sysdvr-webos.log"

# The application itself truncates and owns the single diagnostic log.
if [[ -n "${HOST}" ]]; then
    PARAMS="{\"host\":\"${HOST}\",\"log\":\"${LOG}\"}"
else
    PARAMS="{\"log\":\"${LOG}\"}"
fi

ares-launch -d "${DEVICE}" "${APP_ID}" -p "${PARAMS}"

echo
echo "Launched SysDVR."
echo "Diagnostic log:"
echo "  ./scripts/show-log.sh ${DEVICE}"
