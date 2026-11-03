#!/bin/sh
cd "$(dirname "$0")" || exit 1
if command -v python3 >/dev/null 2>&1; then
    python3 get_sysdvr_log.py
else
    printf '%s\n' 'Python 3 not found. Install Python 3.9+ from https://www.python.org/downloads/'
fi
printf '\nPress Enter to exit...'
read dummy
