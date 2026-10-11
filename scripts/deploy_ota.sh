#!/usr/bin/env bash
# Wireless OTA deploy for the echo board (ESP32-C5, Waveshare touch-LCD-2.8).
#
# Build -> publish the .bin to the nginx host -> trigger the board to pull+
# flash it -> watch /info until the firmware version flips (or rolls back).
# Works from a firewall-locked Mac because the BOARD downloads from the nginx
# host (same subnet), so nothing ever connects back to the Mac.
#
# WHY A SCRIPT (2026-10-10): doing this by hand against a drifting README is
# exactly what caused a day of "OTA fails" confusion. This script reads the
# real values from secrets.h, verifies the served copy byte-for-byte, and
# proves the result from /info instead of trusting "OTA queued".
#
# Usage:
#     scripts/deploy_ota.sh              # auto-bump PROJECT_VER, build, deploy
#     scripts/deploy_ota.sh --no-bump    # deploy the current version as-is
#     BOARD_IP=192.168.2.198 scripts/deploy_ota.sh   # override board IP
#     scripts/deploy_ota.sh --log        # print the deployment ledger (metrics+result)
#
# Prereqs (one-time): nginx serves ~/vegena-ota/firmware/echo/echo.bin on the
# FW_HOST (port 8091), restart:unless-stopped. secrets/secrets.h holds the
# real OTA_KEY + OTA_URL_PREFIX (this script parses them so they can't drift).
set -euo pipefail

cd "$(dirname "$0")/.."     # repo root (scripts/ is one below)

# ---- Config (overridable via env) ---------------------------------------
BOARD_IP="${BOARD_IP:-192.168.2.198}"   # fixed IP on the trusted VLAN
FW_HOST="${FW_HOST:-192.168.2.7}"       # nginx host (debian.local)
FW_PORT="${FW_PORT:-8091}"
FW_USER="${FW_USER:-mathias}"
FW_DIR="${FW_DIR:-~/vegena-ota/firmware/echo}"
BIN="build/echo.bin"
IDF_PATH_DEFAULT="/Volumes/repo/esp/esp-idf"   # v6.1-dev — supports esp32c5
BUMP=1
[ "${1:-}" = "--no-bump" ] && BUMP=0

# ---- --log: print the deployment ledger and exit -------------------------
# Pulls the authoritative server-side ledger (falls back to the local copy),
# newest last. Shows time, version transition, size, result, and the board's
# rssi/heap at deploy time so OTA failures can be correlated at a glance.
if [ "${1:-}" = "--log" ]; then
    FW_HOST="${FW_HOST:-192.168.2.7}"; FW_USER="${FW_USER:-mathias}"
    FW_DIR="${FW_DIR:-~/vegena-ota/firmware/echo}"
    raw=$(ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "${FW_USER}@${FW_HOST}" \
          "cat ${FW_DIR}/deploy-log.jsonl 2>/dev/null" 2>/dev/null)
    [ -z "$raw" ] && raw=$(cat deploy-log.jsonl 2>/dev/null)
    [ -z "$raw" ] && { echo "no deployment ledger found (server or local)"; exit 0; }
    printf "%-20s %-10s %-9s %-26s %-6s %s\n" "TIME(UTC)" "VER" "SIZE" "RESULT" "RSSI" "HEAPMIN/DMA"
    echo "$raw" | while IFS= read -r l; do
        g() { echo "$l" | grep -o "\"$1\":[^,}]*" | head -1 | cut -d: -f2- | tr -d '"'; }
        printf "%-20s %-10s %-9s %-26s %-6s %s\n" \
            "$(g ts)" "$(g from_ver)->$(g to_ver)" "$(g size)" "$(g result)" \
            "$(g board_rssi)" "$(g board_heap_min)/$(g board_dma_free)"
    done
    exit 0
fi

# ---- Parse the authoritative values straight from secrets.h -------------
# so the script can never drift from what the firmware actually enforces.
SECRETS="secrets/secrets.h"
[ -f "$SECRETS" ] || { echo "ABORT: $SECRETS not found"; exit 1; }
OTA_KEY=$(grep -E '^#define[[:space:]]+OTA_KEY' "$SECRETS" | sed -E 's/.*"([^"]*)".*/\1/')
OTA_PREFIX=$(grep -E '^#define[[:space:]]+OTA_URL_PREFIX[[:space:]]' "$SECRETS" | sed -E 's/.*"([^"]*)".*/\1/')
[ -n "$OTA_KEY" ]    || { echo "ABORT: could not read OTA_KEY from $SECRETS"; exit 1; }
[ -n "$OTA_PREFIX" ] || { echo "ABORT: could not read OTA_URL_PREFIX from $SECRETS"; exit 1; }
FW_URL="${OTA_PREFIX}echo.bin"   # the trigger URL MUST match the allowlist prefix exactly

# ---- Deployment ledger ---------------------------------------------------
# Record EVERY deploy outcome (success/warning/abort) with image metrics +
# timestamp, so OTA behaviour is queryable instead of lost to the terminal.
# Written to BOTH a local file (repo) and a server-side file next to the
# firmware (authoritative, survives the Mac). This is what tonight's "which
# images failed and why" question needed. Fields: ISO8601 UTC time, version,
# size, sha256, result, and the board's /info snapshot (rssi/heap) captured at
# deploy time so a failure can be correlated with link/heap conditions.
LOCAL_LEDGER="deploy-log.jsonl"
SERVER_LEDGER="${FW_DIR}/deploy-log.jsonl"
# V08: numeric JSON fields default to the literal null (valid JSON for a %s
# numeric slot); string fields default to a safe placeholder. An abort BEFORE
# the build (so before SIZE/SHA exist) must still append parseable JSON.
DEPLOY_SIZE="null"; DEPLOY_SHA="unknown"; DEPLOY_RESULT="started"; DEPLOY_NEW_VER="unknown"; DEPLOY_CUR_VER="unknown"

record_result() {
    local saved_status=$?        # V08: preserve the exit status that triggered the trap
    local result="$1"
    local ts; ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    local elapsed=$(( $(date +%s) - ${DEPLOY_START_EPOCH:-$(date +%s)} ))
    # Snapshot the board's full state at the moment of recording (best-effort).
    local info; info=$(curl -s --max-time 4 "http://${BOARD_IP}/info" 2>/dev/null || echo "")
    # V08: tolerant field extractor — grep finding nothing returns 1, which under
    # `set -e`/pipefail would otherwise abort the trap mid-record and drop EXACTLY
    # the failed-deploy ledger line we need. The `|| true` + :-default make every
    # extraction best-effort; missing numerics become the literal null.
    g() { echo "$info" | grep -o "\"$1\":[^,}]*" 2>/dev/null | head -1 | cut -d: -f2- | tr -d '"' || true; }
    local rssi; rssi=$(g rssi); rssi=${rssi:-null}
    local heapfree; heapfree=$(g heap_free); heapfree=${heapfree:-null}
    local heapmin; heapmin=$(g heap_min); heapmin=${heapmin:-null}
    local dmafree; dmafree=$(g dma_free); dmafree=${dmafree:-null}
    local dmalarge; dmalarge=$(g dma_largest); dmalarge=${dmalarge:-null}
    local intfree; intfree=$(g int_free); intfree=${intfree:-null}
    local intlarge; intlarge=$(g int_largest); intlarge=${intlarge:-null}
    local uptime; uptime=$(g uptime_s); uptime=${uptime:-null}
    local boardfw; boardfw=$(g fw); boardfw=${boardfw:-unknown}
    local imgstate; imgstate=$(g img_state); imgstate=${imgstate:-unknown}
    local bootid; bootid=$(g boot_id); bootid=${bootid:-null}
    local appsha; appsha=$(g app_sha); appsha=${appsha:-unknown}
    # V08: escape BACKSLASH first, then quotes, then drop control chars, so the
    # embedded raw /info can never produce invalid JSON. (Order matters: a quote
    # fix before a backslash fix would double-escape.)
    local info_esc; info_esc=$(printf '%s' "$info" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | tr -d '\000-\037')
    local line
    line=$(printf '{"ts":"%s","host":"%s","from_ver":"%s","to_ver":"%s","board_fw":"%s","img_state":"%s","boot_id":%s,"app_sha":"%s","size":%s,"sha256":"%s","result":"%s","elapsed_s":%s,"board_rssi":%s,"board_heap_free":%s,"board_heap_min":%s,"board_dma_free":%s,"board_dma_largest":%s,"board_int_free":%s,"board_int_largest":%s,"board_uptime_s":%s,"raw_info":"%s"}' \
        "$ts" "$(hostname -s)" "$DEPLOY_CUR_VER" "$DEPLOY_NEW_VER" "$boardfw" "$imgstate" "$bootid" "$appsha" "${DEPLOY_SIZE:-null}" "$DEPLOY_SHA" "$result" "$elapsed" \
        "$rssi" "$heapfree" "$heapmin" "$dmafree" "$dmalarge" "$intfree" "$intlarge" "$uptime" "$info_esc")
    echo "$line" >> "$LOCAL_LEDGER" 2>/dev/null || true
    ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "${FW_USER}@${FW_HOST}" \
        "printf '%s\n' $(printf '%q' "$line") >> ${SERVER_LEDGER}" 2>/dev/null || true
    echo "      [ledger] ${result}  v${DEPLOY_NEW_VER}  ${DEPLOY_SIZE}B  rssi=${rssi} int_free=${intfree} dma_free=${dmafree} elapsed=${elapsed}s"
    return "$saved_status"   # V08: preserve the exit code that triggered the trap
}
DEPLOY_START_EPOCH=$(date +%s)
trap 'record_result "$DEPLOY_RESULT"' EXIT

# ---- 0. Bump PROJECT_VER so the result is visible/provable --------------
CUR_VER=$(grep -E 'set\(PROJECT_VER' CMakeLists.txt | sed -E 's/.*"([0-9]+)".*/\1/')
if [ "$BUMP" = "1" ]; then
    NEW_VER=$((CUR_VER + 1))
    sed -i '' "s/set(PROJECT_VER \"${CUR_VER}\")/set(PROJECT_VER \"${NEW_VER}\")/" CMakeLists.txt
    echo "[0/5] version ${CUR_VER} -> ${NEW_VER}"
else
    NEW_VER="$CUR_VER"
    echo "[0/5] version ${CUR_VER} (no bump)"
fi
DEPLOY_CUR_VER="$CUR_VER"; DEPLOY_NEW_VER="$NEW_VER"

# ---- 1. Build ------------------------------------------------------------
export IDF_PATH="${IDF_PATH:-$IDF_PATH_DEFAULT}"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null 2>&1
echo "[1/5] build (idf: $IDF_PATH)"
idf.py build >/dev/null
[ -f "$BIN" ] || { echo "ABORT: $BIN not produced"; exit 1; }
SIZE=$(wc -c < "$BIN" | tr -d ' ')
SHA=$(shasum -a 256 "$BIN" | awk '{print $1}')
echo "      ${SIZE} bytes  sha256=${SHA}"
DEPLOY_SIZE="$SIZE"; DEPLOY_SHA="$SHA"

# ---- 2. Publish atomically (unique tmp name, then mv) -------------------
# mv on the same filesystem is atomic, so the board can never fetch a
# half-written .bin if an OTA fires mid-copy. The temp name is UNIQUE per run
# ($$ = PID + epoch) so two concurrent deploys can't clobber each other's
# in-flight upload (a fixed echo.bin.tmp raced — Sol #7a).
TMP="echo.bin.tmp.$$.$(date +%s)"
echo "[2/5] publish -> ${FW_USER}@${FW_HOST}:${FW_DIR}/echo.bin (via ${TMP})"
scp -o StrictHostKeyChecking=no "$BIN" "${FW_USER}@${FW_HOST}:${FW_DIR}/${TMP}"
ssh -o StrictHostKeyChecking=no "${FW_USER}@${FW_HOST}" \
    "mv -f ${FW_DIR}/${TMP} ${FW_DIR}/echo.bin"

# ---- 3. Verify the served copy matches the build byte-for-byte -----------
# IMPORTANT: verify the EXACT url the board will download (FW_URL, derived from
# secrets' OTA_URL_PREFIX), NOT a separately-built FW_HOST/FW_PORT url. If those
# diverge (env override of FW_HOST/FW_PORT vs the secrets prefix) we could verify
# one endpoint while the board pulls another (Sol #7a). Strip https for the
# fetch only if the prefix is https (curl -k would be needed for the self-signed
# cert); the LAN default is http.
echo "[3/5] verify served copy @ ${FW_URL}"
GOT=$(curl -sk --max-time 15 "${FW_URL}" | shasum -a 256 | awk '{print $1}')
if [ "$GOT" != "$SHA" ]; then
    echo "ABORT: served sha ($GOT) != built sha ($SHA) at ${FW_URL} -- publish failed"
    echo "       (the board downloads THIS url; verify + download are the same endpoint)"
    DEPLOY_RESULT="fail_publish_mismatch"
    exit 1
fi
echo "      served copy OK"

# ---- 4. Trigger the pull on the board ------------------------------------
echo "[4/5] trigger OTA on ${BOARD_IP} (url=${FW_URL})"
RESP=""
for a in 1 2 3; do
    RESP=$(curl -s --max-time 15 "http://${BOARD_IP}/ota?key=${OTA_KEY}&url=${FW_URL}" || true)
    echo "      attempt ${a}: ${RESP}"
    echo "$RESP" | grep -q "OTA queued" && break
    # "task start failed" is a transient heap condition — retry.
    sleep 4
done
if ! echo "$RESP" | grep -q "OTA queued"; then echo "ABORT: board did not queue the OTA"; DEPLOY_RESULT="fail_trigger"; exit 1; fi

# ---- 5. Confirm from /info: version must flip AND stay (no rollback) -----
# Correct success predicates (Sol review, 2026-10-10). The old logic had three
# false-success holes: a sticky SEEN_NEW flag accepted a later ROLLBACK to the
# same-looking version; --no-bump made old==new so the very first idle read
# "passed"; and there was no soak, so an immediate idle read passed before any
# rollback could occur. This version:
#   - requires that we actually OBSERVED the board go away/busy (a real reboot
#     or in-progress), so we never pass on the pre-OTA state;
#   - rechecks the version on EACH response (never remembers a past match);
#   - requires a SOAK: the new version must read back idle on N consecutive
#     polls, so a rollback during the probation window fails the deploy;
#   - with --no-bump (old==new) the version string cannot prove anything, so it
#     demands the soak AND warns that version alone is not proof.
echo "[5/5] confirm fw ${CUR_VER} -> ${NEW_VER} (reboot + ${SOAK_OK:=4}-poll soak, ~2 min max)"
if [ "$NEW_VER" = "$CUR_VER" ]; then
    echo "      NOTE: --no-bump — old and new share version \"${NEW_VER}\"; the version"
    echo "      string alone cannot distinguish success from a no-op/rollback. Relying on"
    echo "      the reboot-observed + soak checks only. Prefer a version bump for real proof."
fi

OBSERVED_CHANGE=0   # saw the board reboot or report in-progress (proof the OTA acted)
SOAK=0              # consecutive new+idle reads
PREV_UPTIME=""      # V09: previous uptime_s, to detect a reboot via an uptime RESET
# V09: capture the pre-trigger uptime so a reboot that completes entirely
# between two polls (fast boot) is still caught by the uptime going backwards.
BASE_INFO=$(curl -s --max-time 5 "http://${BOARD_IP}/info" 2>/dev/null || true)
PREV_UPTIME=$(echo "$BASE_INFO" | grep -o '"uptime_s":[0-9]*' | head -1 | cut -d: -f2 || true)
for i in $(seq 1 24); do
    sleep 5
    INFO=$(curl -s --max-time 5 "http://${BOARD_IP}/info" 2>/dev/null || true)
    if [ -z "$INFO" ]; then
        echo "      [$((i*5))s] (unreachable — rebooting?)"
        OBSERVED_CHANGE=1          # a drop-out is evidence the OTA restarted it
        SOAK=0                     # reset soak across a reboot
        PREV_UPTIME=""             # unknown after a blackout; next read reseeds
        continue
    fi
    echo "      [$((i*5))s] ${INFO}"
    # V09: uptime going BACKWARDS is definitive reboot evidence — catches a fast
    # reboot that happened entirely within one 5 s gap (no unreachable/busy poll
    # seen), and distinguishes a REAL OTA reboot from a mere network blip (which
    # never resets uptime). Essential for --no-bump where the version can't prove it.
    CUR_UPTIME=$(echo "$INFO" | grep -o '"uptime_s":[0-9]*' | head -1 | cut -d: -f2 || true)
    if [ -n "$CUR_UPTIME" ] && [ -n "$PREV_UPTIME" ] && [ "$CUR_UPTIME" -lt "$PREV_UPTIME" ]; then
        OBSERVED_CHANGE=1
    fi
    [ -n "$CUR_UPTIME" ] && PREV_UPTIME="$CUR_UPTIME"
    if echo "$INFO" | grep -q '"ota_in_progress":true'; then
        OBSERVED_CHANGE=1
        SOAK=0
        continue
    fi
    # idle response: check the version FRESH on THIS response (not a memory).
    # V09: also read img_state — a right-version image that is still PENDING or
    # has rolled back/aborted must NOT count toward stable success. Only an
    # img_state we can confirm as valid (or benign pending while probation runs)
    # advances the soak; invalid/aborted resets it. This mirrors the host-tested
    # ota_identity_accept policy (core/ota_identity.h) in shell form.
    IMG_STATE=$(echo "$INFO" | grep -o '"img_state":"[a-z_]*"' | head -1 | cut -d: -f2 | tr -d '"' || true)
    if echo "$INFO" | grep -q "\"fw\":\"${NEW_VER}\""; then
        case "$IMG_STATE" in
            invalid|aborted)
                SOAK=0 ;;                       # right version but rejected -> not success
            *)
                SOAK=$((SOAK + 1)) ;;           # valid / pending_verify / (field absent) -> probation
        esac
    else
        SOAK=0                     # not the new version (e.g. rolled back) -> reset
    fi
    # Success only when: we saw the OTA act (reboot/busy) AND the NEW version has
    # held idle for SOAK_OK consecutive polls (survived the rollback window).
    if [ "$OBSERVED_CHANGE" = "1" ] && [ "$SOAK" -ge "$SOAK_OK" ]; then
        echo ""
        echo ">>> SUCCESS: board is running fw ${NEW_VER}, stable for $((SOAK*5))s <<<"
        DEPLOY_RESULT="success"
        exit 0
    fi
done

echo ""
echo "WARNING: did not confirm a stable fw ${NEW_VER} within the window."
echo "  observed_reboot_or_busy=${OBSERVED_CHANGE}  final_soak=${SOAK}/${SOAK_OK}"
echo "  - If /info kept showing fw ${CUR_VER}: the new image ROLLED BACK (it wedged before"
echo "    app_main confirmed it — check WiFi/HTTP bring-up) or the download failed."
echo "  - If it never went busy/rebooted: the trigger didn't actually start a transfer."
echo "  Check the board's device-info page 'ota:' line or USB serial for the reason."
# Distinguish the two failure shapes for the ledger.
if [ "$OBSERVED_CHANGE" = "1" ]; then
    DEPLOY_RESULT="fail_verify_or_rollback"   # downloaded+rebooted but didn't stick
else
    DEPLOY_RESULT="fail_no_transfer"          # never went busy — trigger/download didn't start
fi
exit 1
