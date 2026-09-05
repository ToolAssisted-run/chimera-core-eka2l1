#!/bin/sh
# The gate. At M0 it can only ask the two questions that need no device dump:
# does upstream's own test suite pass in the headless configuration this port
# builds, and does the emulator, built and torn down by an embedder, say exactly
# the same thing twice?
#
# Usage: ./run-gate.sh
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
tests="$root/build/native/eka2l1/src/tests/ekatests"
rn="$root/build/native/run-native"

[ -x "$tests" ] || { echo "ekatests not built (./waterbox/build-native.sh)"; exit 1; }
[ -x "$rn" ] || { echo "run-native not built (./waterbox/build-native.sh)"; exit 1; }

work="$here/tests/work"
rm -rf "$work"
mkdir -p "$work/storage"

pass=0
fail=0

# ---- the upstream suite ----------------------------------------------------
# 194 cases over the kernel, the MMU, the VFS, the loader, the services and the
# drivers. It needs no device dump, and it is the reason a headless build can be
# trusted before there is anything to boot.
out="$(cd "$(dirname "$tests")" && timeout 900 ./ekatests 2>&1 | tail -3)"
if echo "$out" | grep -q "All tests passed"; then
	echo "PASS upstream: $(echo "$out" | grep 'All tests passed')"
	pass=$((pass + 1))
else
	echo "FAIL upstream"; echo "$out"; fail=$((fail + 1))
fi

# ---- the reference harness -------------------------------------------------
# Twice over the same empty storage, byte for byte. Nothing it prints may come
# from the host, so a difference here is a determinism bug and nothing else.
a="$(timeout 120 "$rn" --data "$work/storage" 2>&1)"
b="$(timeout 120 "$rn" --data "$work/storage" 2>&1)"
if [ -z "$a" ]; then
	echo "FAIL reference (no output)"; fail=$((fail + 1))
elif [ "$a" != "$b" ]; then
	echo "FAIL reference (two runs disagree)"
	echo "--- first"; echo "$a"; echo "--- second"; echo "$b"; fail=$((fail + 1))
else
	echo "PASS reference: $(echo "$a" | tr '\n' ' ')"
	pass=$((pass + 1))
fi

echo "totals: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
