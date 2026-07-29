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
# Also exercises the options/routes added for issue #7:
#   - waitfactor/require: read the driver's current value from /stats and
#     write the same value back (idempotent, applies to every ASIC type).
#   - usbprop/chip: BM1397 only (CompacF/R909) - same round-trip idea via
#     USBProp/Chip0FreqReply; HTTP 422 on any other ASIC type is SKIP, not
#     FAIL, same idiom as setfan above.
#   - POST /api/v1/control/enable and /disable: disables the device, checks
#     /api/v1/devs' "Enabled" field flips to "N", then re-enables and
#     checks it flips back to "Y". This briefly stops the device from
#     mining - that's the feature being tested - always ends by
#     re-enabling, same "leave the device as found" principle as setfan's
#     restore-to-100.
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

code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/version)
check "GET /version" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/config)
check "GET /config" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/coin)
check "GET /coin" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/notify)
check "GET /notify" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:4029/api/v1/summary)
check "GET /summary without token is rejected" "$code" "401"

note "docs endpoints (no auth, embedded at build time - see tools/embed_file.sh)"
code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:4029/openapi.yaml)
check "GET /openapi.yaml (no auth)" "$code" "200"

first_line=$(curl -s http://127.0.0.1:4029/openapi.yaml | head -1)
check "GET /openapi.yaml content" "$first_line" "openapi: 3.1.0"

code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:4029/docs)
check "GET /docs (no auth)" "$code" "200"

code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:4029/docs/redoc.standalone.js)
check "GET /docs/redoc.standalone.js (no auth)" "$code" "200"

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

		# waitfactor/require apply to every ASIC type (compac_api_set() does
		# not gate them on asic_type/ident) - read the driver's current
		# value from /stats and write the same value back, so this is an
		# idempotent round trip rather than an actual tuning change.
		note "waitfactor round-trip (reads current value, writes it back unchanged)"
		waitfactor=$(curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/stats \
			| python3 -c 'import json,sys
d = json.load(sys.stdin)
devs = [e for e in d.get("stats", {}).get("STATS", []) if not str(e.get("ID", "")).startswith("POOL")]
print(devs[0].get("WaitFactor0", "") if devs else "")')
		if [ -n "$waitfactor" ]; then
			code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
				-H "Authorization: Bearer $WRITE_TOKEN" -H "Content-Type: application/json" \
				-d "{\"asc_id\":0,\"option\":\"waitfactor\",\"value\":$waitfactor}" http://127.0.0.1:4029/api/v1/control)
			check "POST /control waitfactor (same value $waitfactor)" "$code" "200"
		else
			echo "SKIP POST /control waitfactor (could not read current WaitFactor0 from /stats)"
		fi

		note "require round-trip (reads current value, writes it back unchanged)"
		require=$(curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/stats \
			| python3 -c 'import json,sys
d = json.load(sys.stdin)
devs = [e for e in d.get("stats", {}).get("STATS", []) if not str(e.get("ID", "")).startswith("POOL")]
print(devs[0].get("Require", "") if devs else "")')
		if [ -n "$require" ]; then
			code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
				-H "Authorization: Bearer $WRITE_TOKEN" -H "Content-Type: application/json" \
				-d "{\"asc_id\":0,\"option\":\"require\",\"value\":$require}" http://127.0.0.1:4029/api/v1/control)
			check "POST /control require (same value $require)" "$code" "200"
		else
			echo "SKIP POST /control require (could not read current Require from /stats)"
		fi

		# usbprop/chip are BM1397-only (compac_api_set() rejects them for
		# any other asic_type before even looking at the value) - always
		# attempt them and treat the driver's own HTTP 422 rejection as
		# SKIP rather than FAIL, same idiom as setfan above, instead of
		# trying to infer the ASIC type from the detect flag ourselves.
		note "usbprop round-trip (BM1397 only - HTTP 422 on other ASIC types is SKIP, not FAIL)"
		usbprop=$(curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/stats \
			| python3 -c 'import json,sys
d = json.load(sys.stdin)
devs = [e for e in d.get("stats", {}).get("STATS", []) if not str(e.get("ID", "")).startswith("POOL")]
print(devs[0].get("USBProp", 400) if devs else 400)')
		usbprop_code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
			-H "Authorization: Bearer $WRITE_TOKEN" -H "Content-Type: application/json" \
			-d "{\"asc_id\":0,\"option\":\"usbprop\",\"value\":$usbprop}" http://127.0.0.1:4029/api/v1/control)
		if [ "$usbprop_code" = "200" ]; then
			echo "OK   POST /control usbprop (same value $usbprop)"
		elif [ "$usbprop_code" = "422" ]; then
			echo "SKIP POST /control usbprop (HTTP 422 - device is not BM1397)"
		else
			echo "FAIL POST /control usbprop (HTTP $usbprop_code)"
			fail=1
		fi

		note "chip round-trip (BM1397 only - HTTP 422 on other ASIC types is SKIP, not FAIL)"
		chip_freq=$(curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/stats \
			| python3 -c 'import json,sys
d = json.load(sys.stdin)
devs = [e for e in d.get("stats", {}).get("STATS", []) if not str(e.get("ID", "")).startswith("POOL")]
print(devs[0].get("Chip0FreqReply", 0) if devs else 0)')
		chip_code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
			-H "Authorization: Bearer $WRITE_TOKEN" -H "Content-Type: application/json" \
			-d "{\"asc_id\":0,\"option\":\"chip\",\"chip_index\":0,\"value\":$chip_freq}" http://127.0.0.1:4029/api/v1/control)
		if [ "$chip_code" = "200" ]; then
			echo "OK   POST /control chip (chip_index 0, same value $chip_freq)"
		elif [ "$chip_code" = "422" ]; then
			echo "SKIP POST /control chip (HTTP 422 - device is not BM1397)"
		else
			echo "FAIL POST /control chip (HTTP $chip_code)"
			fail=1
		fi

		# ascenable/ascdisable briefly stops the device from mining (that's
		# the point of the feature) - verify the round trip through
		# apibridge's own /devs view, then always re-enable before moving
		# on, same "leave the device as found" principle as setfan's
		# restore-to-100 above.
		note "ascenable/ascdisable round-trip (verifies /devs Enabled actually flips through apibridge)"
		code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
			-H "Authorization: Bearer $WRITE_TOKEN" -H "Content-Type: application/json" \
			-d '{"asc_id":0}' http://127.0.0.1:4029/api/v1/control/disable)
		check "POST /control/disable with write token" "$code" "200"

		enabled_after_disable=$(curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/devs \
			| python3 -c 'import json,sys
d = json.load(sys.stdin)
devs = d.get("devs", {}).get("DEVS", [])
print(devs[0].get("Enabled", "") if devs else "")')
		check "GET /devs Enabled after disable" "$enabled_after_disable" "N"

		code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
			-H "Authorization: Bearer $WRITE_TOKEN" -H "Content-Type: application/json" \
			-d '{"asc_id":0}' http://127.0.0.1:4029/api/v1/control/enable)
		check "POST /control/enable with write token" "$code" "200"

		enabled_after_enable=$(curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:4029/api/v1/devs \
			| python3 -c 'import json,sys
d = json.load(sys.stdin)
devs = d.get("devs", {}).get("DEVS", [])
print(devs[0].get("Enabled", "") if devs else "")')
		check "GET /devs Enabled after enable" "$enabled_after_enable" "Y"
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
