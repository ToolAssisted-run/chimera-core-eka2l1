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

# ---- the same machine, inside the sandbox ----------------------------------
# core.wbx runs the identical workload through the miniBox host. Every number
# comes from the machine, so native and sandbox must agree exactly; a
# difference here is the sandbox changing the emulator, which is the one thing
# a waterboxed core may never do.
core="$here/bin/core.wbx"
rw="$here/bin/run-wbx"
digest='^(frames|virtual us|instructions|timer fired|timer last|timer lateness):'

if [ ! -x "$rw" ] || [ ! -f "$core" ]; then
	echo "SKIP sandbox: core.wbx not built (./waterbox/build-guest.sh && ./waterbox/build-core.sh)"
else
	nat="$(run | grep -E "$digest")"
	box="$(timeout 600 "$rw" "$core" --frames 60 2>&1 | grep -E "$digest")"

	if [ -z "$box" ]; then
		echo "FAIL sandbox (no output)"; fail=$((fail + 1))
	elif [ "$nat" != "$box" ]; then
		echo "FAIL sandbox (native vs sandbox)"
		echo "$nat" > "$work/native.txt"
		echo "$box" > "$work/sandbox.txt"
		diff "$work/native.txt" "$work/sandbox.txt" | head -20; fail=$((fail + 1))
	else
		echo "PASS sandbox: native == sandbox ($(echo "$box" | grep 'timer fired'))"
		pass=$((pass + 1))
	fi
fi

# ---- a real device, and real Symbian code ----------------------------------
# Only when the user's own ROM is here. It is never committed, and the gate
# says so rather than failing when it is absent.
rom="$root/tests/roms-local/SYM.ROM"
installer="$root/build/native/install-device"

if [ ! -f "$rom" ] || [ ! -x "$installer" ]; then
	echo "SKIP device: no tests/roms-local/SYM.ROM (the ROM is the user's to supply)"
else
	mkdir -p "$work/device"
	install_out="$(timeout 600 "$installer" --data "$work/device" --rom "$rom" 2>&1)"

	if ! echo "$install_out" | grep -q "^install: ok"; then
		echo "FAIL device (install)"; echo "$install_out"; fail=$((fail + 1))
	else
		echo "PASS device: $(echo "$install_out" | grep '^device 0:')"
		pass=$((pass + 1))

		# The machine's own menu, launched through its registration, for a
		# second of emulated time. What matters is that the ARM interpreter
		# executes the same instructions every time, and the same number of
		# them whatever the host is doing.
		boot() {
			rm -rf "$work/boot"
			cp -r "$work/device" "$work/boot"
			timeout 900 "$rn" --data "$work/boot" --frames 60 --run 0x101f4cd2 "$@" 2>&1 				| grep -E '^(run|virtual us|instructions|loops):'
		}

		one="$(boot)"
		two="$(boot)"
		three="$(boot --sleep-ms 3)"

		if ! echo "$one" | grep -q "started"; then
			echo "FAIL boot (the application did not start)"; echo "$one"; fail=$((fail + 1))
		elif [ "$one" != "$two" ] || [ "$one" != "$three" ]; then
			echo "FAIL boot (runs disagree)"
			echo "--- first"; echo "$one"
			echo "--- second"; echo "$two"
			echo "--- stalled"; echo "$three"; fail=$((fail + 1))
		else
			echo "PASS boot: $(echo "$one" | grep -E '^(instructions|loops):' | tr '\n' ' ')"
			pass=$((pass + 1))
		fi

		# Everything below runs against the ROM and nothing else, the way a
		# core will: an empty storage root, and one file.
		rm -rf "$work/romonly"
		mkdir -p "$work/romonly"

		# ---- the machine on a real OpenGL context ---------------------------
		# The emulator's renderer runs on a context the harness made and never
		# on one of its own, its command lists run on the stepping thread
		# rather than on a thread of the driver's, and the screen is composed
		# on demand and read back. What it draws is not yet a picture - see
		# docs/PLAN.md - but the size, the digest and the path are the
		# machine's, and they must be the same every time.
		gpu() {
			rm -rf "$work/gpu"
			cp -r "$work/romonly" "$work/gpu"
			timeout 900 "$rn" --data "$work/gpu" --rom-only "$rom" --gpu --frames 120 \
				--run 0x10005902 2>/dev/null | grep -E '^(gpu|screen|instructions):'
		}

		one_gpu="$(gpu)"
		two_gpu="$(gpu)"

		if ! echo "$one_gpu" | grep -q "^gpu: on"; then
			echo "SKIP gpu: no OpenGL context here"
		elif ! echo "$one_gpu" | grep -q "^screen: "; then
			echo "FAIL gpu (the machine composed no screen)"; echo "$one_gpu"; fail=$((fail + 1))
		elif [ "$one_gpu" != "$two_gpu" ]; then
			echo "FAIL gpu (two runs disagree)"
			echo "--- first"; echo "$one_gpu"; echo "--- second"; echo "$two_gpu"; fail=$((fail + 1))
		else
			echo "PASS gpu: $(echo "$one_gpu" | grep '^screen:')"
			pass=$((pass + 1))
		fi

		# ---- the device inside the sandbox ---------------------------------
		# One file crosses into the box: the ROM. It says which device it is,
		# it carries drive Z, and the writable drives are the machine's own
		# memory. Both flavors then run the same application, and what the
		# processor did must be identical.
		if [ ! -x "$rw" ] || [ ! -f "$core" ]; then
			echo "SKIP in-box: core.wbx not built"
		else
			digest2='^(apps|run|virtual us|instructions):'
			# Sorted: the two report the same facts, not necessarily in the
			# same order - one learns the application count while booting, the
			# other when asked.
			natp="$(timeout 900 "$rn" --data "$work/romonly" --rom-only "$rom" --frames 60 --run 0x101f4cd2 2>&1 | grep -E "$digest2" | sort)"
			boxp="$(timeout 900 "$rw" "$core" --frames 60 --rom "$rom" --run 0x101f4cd2 2>&1 | grep -E "$digest2" | sort)"

			if [ -z "$boxp" ]; then
				echo "FAIL in-box (the sandbox produced nothing)"; fail=$((fail + 1))
			elif [ "$natp" != "$boxp" ]; then
				echo "FAIL in-box (native vs sandbox with a device)"
				echo "$natp" > "$work/natp.txt"
				echo "$boxp" > "$work/boxp.txt"
				diff "$work/natp.txt" "$work/boxp.txt" | head -20; fail=$((fail + 1))
			else
				echo "PASS in-box: native == sandbox with the device ($(echo "$boxp" | tr '\n' ' '))"
				pass=$((pass + 1))
			fi
		fi
	fi
fi

echo "totals: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
