#!/bin/sh
cd "$(dirname "$0")" || exit 1
if command -v python3 >/dev/null 2>&1; then
    exec python3 get_sysdvr_log.py
fi
printf '%s\n' 'Python 3 not found. Install Python 3.9 or newer.' >&2
exit 1
