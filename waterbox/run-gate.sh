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

# ---- the processor this core runs on is right ------------------------------
# Upstream's own differential harness: 200,000 single instructions against a
# golden ALU model, and thousands of whole programs against dynarmic as an
# independent oracle. It lives in its own build tree because building it changes
# the cpu target (a test-only VFP selector), and the machine this core ships
# must not be built that way.
dt="$root/build/difftest/eka2l1/src/emu/cpu/dyncom_difftest"

if [ ! -x "$dt" ]; then
	echo "SKIP cpu: dyncom_difftest not built (./waterbox/build-difftest.sh)"
else
	dt_out="$(timeout 900 "$dt" 2>&1 | tail -2)"

	if echo "$dt_out" | grep -q "^dyncom_difftest: PASS"; then
		echo "PASS cpu: $(echo "$dt_out" | grep coverage: | sed 's/^ *//')"
		pass=$((pass + 1))
	else
		echo "FAIL cpu"; echo "$dt_out"; fail=$((fail + 1))
	fi
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
	box="$(timeout 600 "$rw" "$core" --frames 60 --timer-us 1000 2>&1 | grep -E "$digest")"

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

# ---- the picture's colours -----------------------------------------------
# chimera#130: every N-Gage game came out with its reds and blues exchanged,
# because the 12-bit display mode was read as 0x0RGB when the blue is in the
# high nibble. Nothing in this gate could have caught it: every digest agreed
# with itself, and a screenshot only says what it says to somebody looking.
#
# So the order is asserted on values whose colours are not a matter of
# opinion - pure red, green and blue each live in one nibble - which needs no
# machine, no content and no picture, and fails the moment somebody reorders
# them. Watched red against the old order: it named 0x000F and 0x0F00.
if out="$("$rn" --colour-check 2>&1)"; then
	echo "PASS colour (12-bit pixels unpack to the colours they name)"
	pass=$((pass + 1))
else
	echo "FAIL colour: $out"; fail=$((fail + 1))
fi

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
		# on demand and read back. What it draws must be a picture, and the
		# same picture every time.
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
		elif echo "$one_gpu" | grep -q "^screen: .* lit 0$"; then
			echo "FAIL gpu (the composed screen is empty)"; echo "$one_gpu"; fail=$((fail + 1))
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

			# ---- the machine survives being saved and reloaded ----------
			# Saved and reloaded before EVERY frame. Anything of the machine
			# that lived outside the sandbox's memory, or any pointer the core
			# kept across a load, would show up as a run that no longer matches
			# an ordinary one.
			srec="$(timeout 1800 "$rw" "$core" --frames 60 --rom "$rom" --run 0x101f4cd2 --rerecord 2>&1 | grep -E "$digest2" | sort)"

			if [ -z "$srec" ]; then
				echo "FAIL state (the rerecording run produced nothing)"; fail=$((fail + 1))
			elif [ "$boxp" != "$srec" ]; then
				echo "FAIL state (a save and reload before every frame changed the run)"
				echo "$boxp" > "$work/boxp.txt"
				echo "$srec" > "$work/srec.txt"
				diff "$work/boxp.txt" "$work/srec.txt" | head -20; fail=$((fail + 1))
			else
				echo "PASS state: 60 saves and reloads, and the machine is the same one"
				pass=$((pass + 1))
			fi

			# ---- a package installs into the machine --------------------
			# Upstream's own test package, which is in this repository and
			# needs no ROM of anybody's. It installs into drive C, which is
			# the machine's own memory - so what lands there is machine state
			# and travels in its savestates, and both flavors must write
			# exactly the same amount of it.
			sis="$root/extern/eka2l1/src/intests/sis/intests.sis"
			digest3='^(install|apps|drive entries|drive written):'

			rm -rf "$work/install"
			mkdir -p "$work/install"

			nati="$(timeout 900 "$rn" --data "$work/install" --rom-only "$rom" --frames 60 --install "$sis" 2>&1 | grep -E "$digest3" | sort)"
			boxi="$(timeout 900 "$rw" "$core" --frames 60 --rom "$rom" --game "$sis" 2>&1 | grep -E "$digest3" | sort)"

			if ! echo "$boxi" | grep -q "^install: 0"; then
				echo "FAIL install (the sandbox refused the package)"; echo "$boxi"; fail=$((fail + 1))
			elif [ "$nati" != "$boxi" ]; then
				echo "FAIL install (native vs sandbox)"
				echo "$nati" > "$work/nati.txt"
				echo "$boxi" > "$work/boxi.txt"
				diff "$work/nati.txt" "$work/boxi.txt" | head -20; fail=$((fail + 1))
			else
				echo "PASS install: native == sandbox ($(echo "$boxi" | grep 'drive' | tr '\n' ' '))"
				pass=$((pass + 1))
			fi

			# ---- a game that draws through the window server ------------
			# Not every game paints the panel itself. One that draws through
			# the window server needs the machine to COMPOSE, which needs an
			# OpenGL - and a sandbox has no GPU to borrow one from, so the core
			# carries Mesa's softpipe. This leg is the proof that it works:
			# the same game, drawn inside the box, with nothing outside it.
			wserv="$root/tests/roms-local/King Of Fighters.rar"

			if [ ! -f "$wserv" ]; then
				echo "SKIP compositor: no window-server game in tests/roms-local"
			else
				digest9='^(launched|instructions|screen):'

				rm -rf "$work/wserv"
				mkdir -p "$work/wserv"

				natw="$(timeout 1800 "$rn" --data "$work/wserv" --rom-only "$rom" --gpu --frames 3000 --card "$wserv" 2>&1 | grep -E "$digest9" | sort)"
				boxw="$(timeout 1800 "$rw" "$core" --frames 3000 --rom "$rom" --game "$wserv" 2>&1 | grep -E "$digest9" | sort)"

				if [ -z "$boxw" ]; then
					echo "FAIL compositor (the sandbox produced nothing)"; fail=$((fail + 1))
				elif echo "$boxw" | grep -q "^screen: .* lit 0$"; then
					echo "FAIL compositor (the sandbox composed nothing)"; echo "$boxw"; fail=$((fail + 1))
				elif [ "$natw" != "$boxw" ]; then
					echo "FAIL compositor (native vs sandbox)"
					echo "$natw" > "$work/natw.txt"
					echo "$boxw" > "$work/boxw.txt"
					diff "$work/natw.txt" "$work/boxw.txt" | head -20; fail=$((fail + 1))
				else
					echo "PASS compositor ($(basename "$wserv")): composed inside the box, and it matches the host's own OpenGL ($(echo "$boxw" | grep '^screen:'))"
					pass=$((pass + 1))
				fi
			fi

			# ---- a card image the machine unpacks for itself ------------
			# A .blz is a container this core cannot read and neither can the
			# machine: what reads one is a Symbian application, so the core
			# installs it, puts the .blz where it looks, runs it and works its
			# menu - all before the first frame anybody asked for. Both flavors
			# must do that identically, and end with the game running.
			# The gate names the games it was written against, and falls back to
			# whatever is there. A folder of games is a collection, not a
			# suite: one that crashes is a finding, not a broken gate.
			blz="$root/tests/roms-local/MotoGP.blz"
			[ -f "$blz" ] || blz="$(ls "$root"/tests/roms-local/*.blz 2>/dev/null | head -1)"
			blzapp="$root/tests/roms-local/BLZinstapp.sis"

			if [ -z "$blz" ] || [ ! -f "$blzapp" ]; then
				echo "SKIP blz: no .blz and BLZinstapp.sis in tests/roms-local (both are the user's to supply)"
			else
				digest8='^(launched|instructions|screen):'

				rm -rf "$work/blz"
				mkdir -p "$work/blz"

				natz="$(timeout 1800 "$rn" --data "$work/blz" --rom-only "$rom" --frames 3000 --blz "$blz" --blz-installer "$blzapp" 2>/dev/null | grep -E "$digest8" | sort)"
				boxz="$(timeout 1800 "$rw" "$core" --frames 3000 --rom "$rom" --game "$blz" --blz-installer "$blzapp" 2>&1 | grep -E "$digest8" | sort)"

				if ! echo "$boxz" | grep -q "^launched: "; then
					echo "FAIL blz (the sandbox unpacked nothing)"; echo "$boxz"; fail=$((fail + 1))
				elif echo "$boxz" | grep -q "^screen: .* lit 0$"; then
					echo "FAIL blz (the game drew nothing)"; echo "$boxz"; fail=$((fail + 1))
				elif [ "$natz" != "$boxz" ]; then
					echo "FAIL blz (native vs sandbox)"
					echo "$natz" > "$work/natz.txt"
					echo "$boxz" > "$work/boxz.txt"
					diff "$work/natz.txt" "$work/boxz.txt" | head -20; fail=$((fail + 1))
				else
					echo "PASS blz ($(basename "$blz")): the machine unpacked it and ran it, native == sandbox ($(echo "$boxz" | tr '\n' ' '))"
					pass=$((pass + 1))
				fi
			fi

			# ---- a game card, and the game running ----------------------
			# The user's own, never committed. The card is copied out of its
			# archive straight onto the machine's drive E, the application
			# list finds it, and it runs - the same instructions on both
			# sides, which is the whole claim this core makes.
			card="$root/tests/roms-local/Red Faction.rar"
			[ -f "$card" ] || card="$(ls "$root"/tests/roms-local/*.rar "$root"/tests/roms-local/*.zip 2>/dev/null | head -1)"

			if [ -z "$card" ]; then
				echo "SKIP card: no game in tests/roms-local (the game is the user's to supply)"
			else
				# The picture too. The game draws straight into the panel, which
				# is the machine's own memory, so the sandbox has a picture with
				# no OpenGL anywhere and it must be the reference's pixel for
				# pixel.
				digest4='^(apps|launched|instructions|drive entries|drive written|screen|bus body):'

				rm -rf "$work/card"
				mkdir -p "$work/card"

				natc="$(timeout 1800 "$rn" --data "$work/card" --rom-only "$rom" --frames 1500 --card "$card" 2>&1 | grep -E "$digest4" | sort)"
				boxc="$(timeout 1800 "$rw" "$core" --frames 1500 --rom "$rom" --game "$card" 2>&1 | grep -E "$digest4" | sort)"

				if ! echo "$boxc" | grep -q "^launched: "; then
					echo "FAIL card (the game did not start in the sandbox)"; echo "$boxc"; fail=$((fail + 1))
				elif echo "$boxc" | grep -q "^screen: .* lit 0$"; then
					echo "FAIL card (the game drew nothing)"; echo "$boxc"; fail=$((fail + 1))
				elif [ "$natc" != "$boxc" ]; then
					echo "FAIL card (native vs sandbox)"
					echo "$natc" > "$work/natc.txt"
					echo "$boxc" > "$work/boxc.txt"
					diff "$work/natc.txt" "$work/boxc.txt" | head -20; fail=$((fail + 1))
				else
					echo "PASS card ($(basename "$card")): native == sandbox, the game started itself ($(echo "$boxc" | grep -E '^(launched|instructions|screen):' | tr '\n' ' '))"
					pass=$((pass + 1))
				fi

				# ---- the machine's memory can be watched ----------------
				# A chimera bus: an address space rather than a block, because
				# Symbian memory IS an address space - every chunk has its own
				# mapping, and a game's chunks do not exist until it has run, so
				# there is no pointer and size to hand over once. The body of
				# the game's heap must hold something, and must hold the same
				# thing in both flavors.
				#
				# The whole space is NOT compared: the emulator keeps a little
				# of its own bookkeeping inside guest chunks - host pointers,
				# eight bytes wide - and those differ between a native process
				# and a sandbox by construction. A hundred and thirty-four bytes
				# of sixty-seven million, all of them at a chunk's base.
				digest7='^bus body:'

				natb="$(echo "$natc" | grep -E "$digest7")"
				boxb="$(echo "$boxc" | grep -E "$digest7")"

				if [ -z "$boxb" ]; then
					echo "FAIL bus (the sandbox read no memory)"; fail=$((fail + 1))
				elif echo "$boxb" | grep -q "nonzero 0$"; then
					echo "FAIL bus (the game's heap reads as nothing)"; echo "$boxb"; fail=$((fail + 1))
				elif [ "$natb" != "$boxb" ]; then
					echo "FAIL bus (native vs sandbox)"
					echo "  native:  $natb"
					echo "  sandbox: $boxb"; fail=$((fail + 1))
				else
					echo "PASS bus: the game's memory reads the same in both ($boxb)"
					pass=$((pass + 1))
				fi

				# ---- the keypad reaches the game ------------------------
				# A key held near the end of the run, and the screen read
				# straight afterwards: the game must answer it, and both
				# flavors must answer it the same way. Down on the menu moves
				# the selection, which is the shortest thing to see.
				digest6='^(instructions|screen):'

				rm -rf "$work/key"
				mkdir -p "$work/key"

				natk="$(timeout 1800 "$rn" --data "$work/key" --rom-only "$rom" --frames 3200 --card "$card" --press 1 2>&1 | grep -E "$digest6" | sort)"
				boxk="$(timeout 1800 "$rw" "$core" --frames 3200 --rom "$rom" --game "$card" --press 1 2>&1 | grep -E "$digest6" | sort)"
				natq="$(timeout 1800 "$rn" --data "$work/key" --rom-only "$rom" --frames 3200 --card "$card" 2>&1 | grep -E '^screen:')"

				if [ -z "$boxk" ]; then
					echo "FAIL keypad (the sandbox produced nothing)"; fail=$((fail + 1))
				elif [ "$natk" = "$natq" ] || ! echo "$natk" | grep -q "^screen:"; then
					echo "FAIL keypad (the key changed nothing)"; echo "$natk"; fail=$((fail + 1))
				elif [ "$natk" != "$boxk" ]; then
					echo "FAIL keypad (native vs sandbox)"
					echo "$natk" > "$work/natk.txt"
					echo "$boxk" > "$work/boxk.txt"
					diff "$work/natk.txt" "$work/boxk.txt" | head -20; fail=$((fail + 1))
				else
					echo "PASS keypad: the game answered the key, native == sandbox ($(echo "$boxk" | grep '^screen:'))"
					pass=$((pass + 1))
				fi

				# ---- a state outlives the process that wrote it ---------
				# What a movie asks of a core: one process saves, another one
				# loads and carries on, and the machine is the same machine.
				# A core holding anything the host gave it - a graphics
				# context above all - fails here and nowhere else.
				st="$work/card.state"
				digest5='^(instructions|screen):'

				half="$(timeout 1800 "$rw" "$core" --frames 600 --rom "$rom" --game "$card" --state-out "$st" 2>&1 | grep -E '^state out:')"
				rest="$(timeout 1800 "$rw" "$core" --frames 900 --rom "$rom" --game "$card" --state-in "$st" 2>&1 | grep -E "$digest5" | sort)"
				whole="$(echo "$boxc" | grep -E "$digest5" | sort)"

				if [ -z "$half" ]; then
					echo "FAIL session (nothing was written)"; fail=$((fail + 1))
				elif [ "$rest" != "$whole" ]; then
					echo "FAIL session (a state loaded in another process runs differently)"
					echo "$whole" > "$work/whole.txt"
					echo "$rest" > "$work/rest.txt"
					diff "$work/whole.txt" "$work/rest.txt" | head -20; fail=$((fail + 1))
				else
					echo "PASS session: 600 frames saved, another process ran the other 900 ($half)"
					pass=$((pass + 1))
				fi
			fi
		fi
	fi
fi

echo "totals: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
