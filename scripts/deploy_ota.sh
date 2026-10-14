#!/usr/bin/env bash
# Build and verify the exact confirmed image; --no-bump supports same-version OTA.
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-}" in
 --log) cat deploy-log.jsonl; exit 0 ;;
 --no-bump) ;;
 "") python3 - <<'BUMP'
from pathlib import Path
import re
p = Path('CMakeLists.txt')
s = p.read_text()
s, n = re.subn(r'set\(PROJECT_VER "([0-9]+)"\)', lambda m: 'set(PROJECT_VER "' + str(int(m[1]) + 1) + '")', s)
if n != 1: raise SystemExit('cannot determine firmware version')
p.write_text(s)
BUMP
 ;;
 *) echo "usage: $0 [--no-bump|--log]" >&2; exit 2 ;;
esac
export IDF_PATH="${IDF_PATH:-/Volumes/repo/esp/esp-idf}"
source "$IDF_PATH/export.sh" >/dev/null 2>&1
idf.py build
exec python3 tools/maintenance.py deploy --project echo \
 --bin build/echo.bin --secrets secrets/secrets.h \
 --prefix-define OTA_URL_PREFIX --key-define OTA_KEY \
 --board "${BOARD_IP:-192.168.2.198}" --host "${FW_HOST:-192.168.2.7}" \
 --user "${FW_USER:-mathias}" --directory "${FW_DIR:-~/vegena-ota/firmware/echo}"
