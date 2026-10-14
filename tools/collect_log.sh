#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tools/maintenance.py collect "${1:-192.168.2.198}" "${2:-$HOME/echo_log.txt}" --interval "${3:-5}" --info-every "${INFO_EVERY:-12}"
