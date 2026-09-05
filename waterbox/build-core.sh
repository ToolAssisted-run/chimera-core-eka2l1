#!/bin/sh
# Links core.wbx - EKA2L1 as a Chimera waterbox core - from the guest CMake
# archives and the adapter objects, and builds run-wbx, the host driver the
# equivalence gate uses.
#
# Prereq: ./build-guest.sh, and a miniBox checkout built WITH the C++ guest
# toolchain:
#   meson setup <miniBox>/build/meson-cpp -Dguest_cpp=true
#   ninja -C <miniBox>/build/meson-cpp
#
# Usage: ./build-core.sh [-m <miniBox dir>] [-o <output dir>] [-j N]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/tools/chimera-common-minibox}"
out="$here/bin"
jobs="$(nproc)"
while getopts "m:o:j:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		o) out="$OPTARG" ;;
		j) jobs="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
mb="$(cd "$mb" && pwd)"
mbuild="$mb/build/meson-cpp"
sr="$mbuild/guest-sysroot"

[ -f "$sr/lib/libstdc++.a" ] || {
	echo "miniBox C++ guest toolchain missing at $sr." >&2
	exit 1
}
[ -d "$root/build/guest" ] || {
	echo "build/guest missing - run build-guest.sh first." >&2
	exit 1
}

make -f "$here/guest.mk" -C "$here" -j"$jobs" MB="$mb"

mkdir -p "$out"

# One group: the archives cross-reference each other freely.
libs="$(find "$root/build/guest" -name '*.a' | sort | tr '\n' ' ')"

g++ -specs "$sr/lib/musl-gcc.specs" -mcmodel=large -fno-pic -fno-pie \
	-static -no-pie -Wl,--eh-frame-hdr,-O2,--no-relax,-z,stack-size=8388608 -T "$mb/source/guest/linkscript.T" \
	-Wl,-u,pthread_once -Wl,-u,pthread_cond_wait -Wl,-u,pthread_cond_broadcast -Wl,-u,pthread_key_create \
	-Wl,-u,pthread_mutexattr_init -Wl,-u,pthread_mutexattr_settype -Wl,-u,pthread_mutexattr_destroy \
	-o "$out/core.wbx" \
	"$here"/obj-guest/wbx-entry.o "$here"/obj-guest/machine.o "$here"/obj-guest/vclock.o \
	"$here"/obj-guest/host-ui.o "$here"/obj-guest/guest-syscalls.o \
	"$mbuild/source/guest/cxxglue.c.o" "$mbuild/source/guest/emulibc.c.o" \
	-Wl,--start-group $libs -Wl,--end-group \
	-L"$sr/lib" -lstdc++ -lgcc -lgcc_eh -lc
sh "$mb/source/guest/check-wbx.sh" "$out/core.wbx"
echo "built $out/core.wbx"

# The host driver for the gate.
mbhost="${MINIBOX_HOST_DIR:-$mb/build/meson-linux/source/host}"
[ -f "$mbhost/libminiboxhost.so" ] || mbhost="$mbuild/source/host"
gcc -O2 -Wall -I"$mb/source/host" -o "$out/run-wbx" "$here/run-wbx.c" \
	"$mbhost/libminiboxhost.so" -Wl,-rpath,"$mbhost"
echo "built $out/run-wbx"
