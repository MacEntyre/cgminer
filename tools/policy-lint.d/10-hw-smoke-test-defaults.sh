#!/usr/bin/env bash
# Pin real-hardware smoke-test pool defaults in hw-smoke-test.sh.
#
# That script talks to a live Braiins pool on a plugged-in miner; a
# placeholder worker (e.g. test.worker) never auths and never produces
# shares. Defaults must stay aligned with cgminer.sh's test-rig worker.
# See the header comments in apibridge/tools/hw-smoke-test.sh.
set -u

fail=0
name=hw-smoke-test-defaults
file=apibridge/tools/hw-smoke-test.sh

if [ ! -f "$file" ]; then
	echo "FAIL $name: missing $file"
	exit 1
fi

require_line() {
	local desc=$1 pattern=$2
	if grep -qF -- "$pattern" "$file"; then
		echo "OK   $name: $desc"
	else
		echo "FAIL $name: $desc"
		echo "     expected literal: $pattern"
		fail=1
	fi
}

# Patterns are fixed source literals (must not expand under shellcheck).
# shellcheck disable=SC2016
require_line "default POOL is Braiins stratum" \
	'POOL="${2:-stratum+tcp://stratum.braiins.com:3333}"'
# shellcheck disable=SC2016
require_line "default USER is real test-rig worker" \
	'USER="${3:-MacEntyre.APIBridgeTest}"'
# shellcheck disable=SC2016
require_line "default PASS is x" \
	'PASS="${4:-x}"'

# Guard against reintroducing a known-bad placeholder on the USER default
# even if someone tweaks the require_line pattern above.
if grep -nE 'USER="\$\{3:-[^"]*\}"' "$file" | grep -vF 'MacEntyre.APIBridgeTest'; then
	echo "FAIL $name: USER default is not MacEntyre.APIBridgeTest"
	grep -nE 'USER="\$\{3:-[^"]*\}"' "$file" || true
	fail=1
fi

exit "$fail"
