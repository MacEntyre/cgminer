#!/usr/bin/env bash
# Content / intent policies that shellcheck cannot express.
#
# Drop checks into tools/policy-lint.d/*.sh (numbered for order, e.g.
# 10-hw-smoke-test-defaults.sh). Each policy:
#   - is run from the repo root
#   - prints "OK   <name>: ..." or "FAIL <name>: ..." lines
#   - exits 0 on success, non-zero on failure
#
# The runner runs every policy (does not stop on first failure) and
# exits 1 if any failed or if the policy directory is empty/missing.
#
# Usage (from anywhere):
#   tools/policy-lint.sh
#   bash tools/policy-lint.d/10-hw-smoke-test-defaults.sh   # single policy

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 1
cd "$ROOT" || exit 1

policy_dir="$ROOT/tools/policy-lint.d"
fail=0
ran=0

if [ ! -d "$policy_dir" ]; then
	echo "FAIL policy-lint: missing $policy_dir" >&2
	exit 1
fi

shopt -s nullglob
for policy in "$policy_dir"/*.sh; do
	ran=$((ran + 1))
	if ! bash "$policy"; then
		fail=1
	fi
done

if [ "$ran" -eq 0 ]; then
	echo "FAIL policy-lint: no policies in $policy_dir" >&2
	exit 1
fi

if [ "$fail" -ne 0 ]; then
	echo "policy-lint: FAILED ($ran policies)"
	exit 1
fi
echo "policy-lint: PASSED ($ran policies)"
