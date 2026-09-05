#!/bin/sh
# The gate. Until there is a device dump to boot, it asks the questions that
# need none: does upstream's own suite pass in the headless configuration this
# port builds, does the machine answer the same way twice, and is its clock
# really its own?
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

# The workload: a kernel timer every millisecond over a second of emulated
# time. It is the only thing an empty machine can be asked to do, and it runs
# through the nanokernel timer, which is what this port's whole notion of time
# is built on.
run() {
	timeout 300 "$rn" --data "$work/storage" --frames 60 --timer-us 1000 "$@" 2>&1
}

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

# ---- the machine says the same thing twice ---------------------------------
a="$(run)"
b="$(run)"
if [ -z "$a" ]; then
	echo "FAIL reference (no output)"; fail=$((fail + 1))
elif [ "$a" != "$b" ]; then
	echo "FAIL reference (two runs disagree)"
	echo "--- first"; echo "$a"; echo "--- second"; echo "$b"; fail=$((fail + 1))
else
	echo "PASS reference: $(echo "$a" | grep -E '^(virtual us|timer fired|timer lateness):' | tr '\n' ' ')"
	pass=$((pass + 1))
fi

# ---- the clock is the machine's own ----------------------------------------
# The same run with the host stalled five milliseconds every frame. A machine
# reading the wall would see a third of a second go by that this one must not.
stalled="$(run --sleep-ms 5)"
if [ "$a" != "$stalled" ]; then
	echo "FAIL clock (a host stall changed the machine)"
	echo "$a" > "$work/quiet.txt"
	echo "$stalled" > "$work/stalled.txt"
	diff "$work/quiet.txt" "$work/stalled.txt" | head -20; fail=$((fail + 1))
else
	echo "PASS clock: 300 ms of host stalls, and the machine did not notice"
	pass=$((pass + 1))
fi

# ---- and the check has teeth -----------------------------------------------
# The control: the same machine left on the host clock, where the stall MUST
# show. If these two ever agree, the leg above is proving nothing.
hosta="$(run --host-clock)"
hostb="$(run --host-clock --sleep-ms 5)"
if [ "$hosta" = "$hostb" ]; then
	echo "FAIL control (the host clock did not notice a 300 ms stall either)"; fail=$((fail + 1))
else
	echo "PASS control: on the host clock the same stall shows ($(echo "$hosta" | grep -c .) lines, $(echo "$hostb" | grep 'timer fired'))"
	pass=$((pass + 1))
fi

# ---- the machine runs on one thread -----------------------------------------
# EKA2L1 starts a timer thread by default and fires the kernel's timers off the
# host clock from it. Driven, it starts none, and this is what says so.
threads="$(echo "$a" | grep '^host threads:' | awk '{print $3}')"
if [ "$threads" = "1" ]; then
	echo "PASS threads: the machine ran on one"
	pass=$((pass + 1))
else
	echo "FAIL threads: $threads (the timer thread is back)"; fail=$((fail + 1))
fi

echo "totals: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
