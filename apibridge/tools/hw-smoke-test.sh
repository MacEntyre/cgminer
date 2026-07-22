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
# Set HW_SMOKE_TEST_CONTROL=1 to also exercise the Phase 2 control endpoint
# end to end against the real device (starts cgminer with
# --api-bridge-control --api-allow W:127.0.0.1, then relays a "zeromaxt"
# ascset - chosen because it's a no-op on a healthy device, unlike freq/corev,
# so this doesn't perturb whatever the device is actually doing). Off by
# default since it changes cgminer's ACL and talks to the write endpoint;
# left as an opt-in env var rather than a positional arg so the common case
# (just check the REST/WebSocket surface) stays a one-line invocation.
#
# Also relays a real "setfan" (50%) and confirms /api/v1/stats' Fan RPM
# actually changes through apibridge itself (not just raw cgminer RPC) -
# unlike zeromaxt this briefly changes fan speed, so it always restores
# setfan to 100 (the driver's own boot default) afterwards. Only applies to
# GSA1/GSA2 with V2/V3 telemetry - on any other device/telemetry version
# the driver rejects it (HTTP 422), which this script treats as SKIP, not
# FAIL, since it's plugged-in-hardware dependent rather than a bug.
#
# See CLAUDE.md's "Device Identity -> ASIC Mapping" table for the detect
# flag matching your hardware.

set -u

DETECT_FLAG="${1:?usage: $0 <gekko-detect-flag> [pool] [user] [pass]}"
POOL="${2:-stratum+tcp://stratum.braiins.com:3333}"
USER="${3:-test.worker}"
PASS="${4:-x}"
TEST_CONTROL="${HW_SMOKE_TEST_CONTROL:-0}"

cd "$(dirname "$0")/../.." || exit 1

if [ ! -x ./cgminer ] || [ ! -x ./apibridged ]; then
	echo "cgminer/apibridged not found - build with --enable-gekko --enable-apibridge first" >&2
	exit 1
fi

LOG="$(mktemp -t apibridge-hwtest-XXXXXX.log)"
TOKEN_FILE="./apibridge.token"
WRITE_TOKEN_FILE="./apibridge-write.token"
rm -f "$TOKEN_FILE" "$WRITE_TOKEN_FILE"

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

CGMINER_ARGS=(-T "$DETECT_FLAG" --api-listen --api-bridge -o "$POOL" -u "$USER" -p "$PASS" --verbose)
if [ "$TEST_CONTROL" = "1" ]; then
	CGMINER_ARGS+=(--api-bridge-control --api-allow "W:127.0.0.1")
fi

note "starting cgminer against $POOL as $USER, detect flag $DETECT_FLAG"
./cgminer "${CGMINER_ARGS[@]}" >"$LOG" 2>&1 &
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

if [ "$TEST_CONTROL" = "1" ]; then
	note "control endpoint (zeromaxt - non-destructive, see comment at top of this script)"
	for _ in $(seq 1 20); do
		[ -f "$WRITE_TOKEN_FILE" ] && break
		sleep 1
	done
	if [ ! -f "$WRITE_TOKEN_FILE" ]; then
		echo "FAIL apibridge write token never appeared - check $LOG" >&2
		fail=1
	else
		WRITE_TOKEN=$(cat "$WRITE_TOKEN_FILE")

		available=$(curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/health \
			| python3 -c 'import json,sys; print(json.load(sys.stdin).get("control_available"))')
		check "control_available on /health" "$available" "True"

		code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
			-H "Authorization: Bearer $WRITE_TOKEN" -H "Content-Type: application/json" \
			-d '{"asc_id":0,"option":"zeromaxt"}' http://127.0.0.1:4029/api/v1/control)
		check "POST /control zeromaxt with write token" "$code" "200"

		code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
			-H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
			-d '{"asc_id":0,"option":"zeromaxt"}' http://127.0.0.1:4029/api/v1/control)
		check "POST /control with read-only token is rejected" "$code" "401"

		note "setfan round-trip (verifies Fan RPM actually changes through apibridge)"
		# The GSA1/GSA2 driver sometimes logs a transient "found 0 chip(s)"
		# on first init and self-recovers a few seconds later (observed on
		# real A2 hardware) - the apibridge token appearing only means
		# apibridge itself is up, not that chip/telemetry init has finished,
		# so wait for stats' Chips>0 before judging setfan support, the same
		# way this script already waits for WRITE_TOKEN_FILE above.
		for _ in $(seq 1 15); do
			chips=$(curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/stats \
				| python3 -c 'import json,sys
d = json.load(sys.stdin)
devs = [e for e in d.get("stats", {}).get("STATS", []) if str(e.get("ID", "")).startswith("GSA")]
print(devs[0].get("Chips", 0) if devs else 0)' 2>/dev/null)
			[ "${chips:-0}" -gt 0 ] 2>/dev/null && break
			sleep 1
		done

		setfan_code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
			-H "Authorization: Bearer $WRITE_TOKEN" -H "Content-Type: application/json" \
			-d '{"asc_id":0,"option":"setfan","value":50}' http://127.0.0.1:4029/api/v1/control)
		if [ "$setfan_code" = "200" ]; then
			# the PWM command reaches the MCU on the driver's next telemetry
			# poll cycle (not instantly), and the physical fan then needs a
			# moment to actually spin up before the tach reading reflects
			# it - poll instead of a single fixed sleep, since a fan coming
			# from a cold/near-0 start can take a few seconds.
			fan_after=0
			for _ in $(seq 1 10); do
				fan_after=$(curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/stats \
					| python3 -c 'import json,sys
d = json.load(sys.stdin)
devs = [e for e in d.get("stats", {}).get("STATS", []) if str(e.get("ID", "")).startswith("GSA")]
print(devs[0].get("Fan", "") if devs else "")')
				python3 -c "import sys; sys.exit(0 if float('$fan_after' or 0) > 0 else 1)" 2>/dev/null && break
				sleep 1
			done
			if python3 -c "import sys; sys.exit(0 if float('$fan_after' or 0) > 0 else 1)" 2>/dev/null; then
				echo "OK   POST /control setfan changed Fan RPM (now ${fan_after}rpm)"
			else
				echo "FAIL POST /control setfan accepted (200) but Fan RPM did not change (got '$fan_after')"
				fail=1
			fi

			# restore the driver's own boot default rather than leaving the
			# fan at the 50% test value
			curl -s -o /dev/null -X POST \
				-H "Authorization: Bearer $WRITE_TOKEN" -H "Content-Type: application/json" \
				-d '{"asc_id":0,"option":"setfan","value":100}' http://127.0.0.1:4029/api/v1/control
		else
			echo "SKIP POST /control setfan (HTTP $setfan_code - device doesn't support setfan, e.g. not GSA1/2 V2/V3)"
		fi
	fi
fi

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

rm -f "$TOKEN_FILE" "$WRITE_TOKEN_FILE"
note "full log: $LOG"

if [ "$fail" -ne 0 ]; then
	printf '\nSMOKE TEST: FAILED\n'
	exit 1
fi
printf '\nSMOKE TEST: PASSED\n'
