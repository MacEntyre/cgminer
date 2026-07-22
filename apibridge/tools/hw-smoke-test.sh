#!/usr/bin/env bash
# Manual smoke test for apibridge against a real, physically-attached
# GekkoScience miner. Not part of CI - GitHub Actions runners have no USB
# access to the hardware. Run this by hand from the repo root whenever a
# device is plugged in and you want to sanity-check apibridge end to end
# (not just that the binary starts, which is all the CI smoke test can do).
#
# Requires: cgminer and apibridged already built with
#   ./configure --enable-gekko --enable-apibridge
#
# Usage:
#   apibridge/tools/hw-smoke-test.sh <gekko-detect-flag> [pool] [user] [pass]
#
# Example (Compac A1):
#   apibridge/tools/hw-smoke-test.sh --gekko-compaca1-detect \
#       stratum+tcp://stratum.braiins.com:3333 MyWorker.test x
#
# See CLAUDE.md's "Device Identity -> ASIC Mapping" table for the detect
# flag matching your hardware.

set -u

DETECT_FLAG="${1:?usage: $0 <gekko-detect-flag> [pool] [user] [pass]}"
POOL="${2:-stratum+tcp://stratum.braiins.com:3333}"
USER="${3:-test.worker}"
PASS="${4:-x}"

cd "$(dirname "$0")/../.." || exit 1

if [ ! -x ./cgminer ] || [ ! -x ./apibridged ]; then
	echo "cgminer/apibridged not found - build with --enable-gekko --enable-apibridge first" >&2
	exit 1
fi

LOG="$(mktemp -t apibridge-hwtest-XXXXXX.log)"
TOKEN_FILE="./apibridge.token"
rm -f "$TOKEN_FILE"

fail=0
note() { printf '\n== %s ==\n' "$1"; }
check() {
	if [ "$2" = "$3" ]; then
		echo "OK   $1 ($2)"
	else
		echo "FAIL $1 (got $2, want $3)"
		fail=1
	fi
}

note "starting cgminer against $POOL as $USER, detect flag $DETECT_FLAG"
./cgminer -T "$DETECT_FLAG" --api-listen --api-bridge \
	-o "$POOL" -u "$USER" -p "$PASS" --verbose >"$LOG" 2>&1 &
CGMINER_PID=$!

note "waiting for device detection and apibridge token"
for _ in $(seq 1 20); do
	[ -f "$TOKEN_FILE" ] && break
	sleep 1
done
if [ ! -f "$TOKEN_FILE" ]; then
	echo "FAIL apibridge token never appeared - check $LOG" >&2
	kill -TERM "$CGMINER_PID" 2>/dev/null
	exit 1
fi
TOKEN=$(cat "$TOKEN_FILE")

note "device detection log lines"
grep -E "found 0 chip|GSA 0 -|GSF |GSK |NewPac|R606|Compac|Terminus|2Pac" "$LOG" | grep -iv "reset succeess" | tail -5

note "REST endpoints"
code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:4029/api/v1/health)
check "GET /health (no auth)" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/summary)
check "GET /summary" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/devs)
check "GET /devs" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/pools)
check "GET /pools" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/stats)
check "GET /stats" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:4029/api/v1/summary)
check "GET /summary without token is rejected" "$code" "401"

note "devs payload (spot-check real device fields)"
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/devs

note "stats payload (spot-check Fan/FanCeiling on GSA1/GSA2 devices)"
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/stats \
	| python3 -c 'import json,sys; d=json.load(sys.stdin); print([e for e in d.get("stats",{}).get("STATS",[]) if str(e.get("ID","")).startswith("GSA")])'

note "WebSocket /stream (3 live frames)"
python3 "$(dirname "$0")/ws_probe.py" 127.0.0.1 4029 "$TOKEN" 3 | grep -c '"type":"stats"' \
	| { read -r n; check "stream frames received" "$n" "3"; }

note "clean shutdown (must not require SIGKILL within cgminer's 2s grace period)"
kill -TERM "$CGMINER_PID"
sleep 3
if grep -q "did not exit cleanly, sending SIGKILL" "$LOG"; then
	echo "FAIL apibridged needed SIGKILL - see statscache.c poll thread shutdown handling"
	fail=1
else
	echo "OK   apibridged exited within grace period"
fi

rm -f "$TOKEN_FILE"
note "full log: $LOG"

if [ "$fail" -ne 0 ]; then
	printf '\nSMOKE TEST: FAILED\n'
	exit 1
fi
printf '\nSMOKE TEST: PASSED\n'
