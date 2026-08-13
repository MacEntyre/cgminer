#!/usr/bin/env bash
# Pin pool/user/pass defaults in the local test-rig helper cgminer.sh.
#
# Same rationale as 10-hw-smoke-test-defaults.sh: this launches against a
# real Braiins worker on the Compac A1 rig. Keep it aligned with
# apibridge/tools/hw-smoke-test.sh so a one-line ./cgminer.sh start and a
# hardware smoke test auth the same way.
set -u

fail=0
name=cgminer-sh-defaults
file=cgminer.sh

if [ ! -f "$file" ]; then
	# cgminer.sh is gitignored (real credentials/local test-rig wrapper) and
	# only exists on developer machines, not in the CI checkout. Nothing to
	# police there - skip rather than fail.
	echo "OK   $name: skipped, $file not present (gitignored, local-only)"
	exit 0
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

require_line "pool is Braiins stratum" \
	'-o stratum+tcp://stratum.braiins.com:3333'
require_line "user is real test-rig worker" \
	'-u MacEntyre.APIBridgeTest'
require_line "pass is x" \
	'-p x'

# Guard against a different -u slipping in while the expected line is
# removed or commented out.
if ! grep -qE '^\s*-u MacEntyre\.APIBridgeTest\s*\\?\s*$' "$file"; then
	echo "FAIL $name: -u MacEntyre.APIBridgeTest line missing or altered"
	grep -n -- '-u ' "$file" || true
	fail=1
fi

exit "$fail"
