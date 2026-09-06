#!/bin/sh
# Builds the OpenGL this core carries: Mesa, OSMesa front end, softpipe driver,
# for the miniBox guest toolchain.
#
# A game that draws through the Symbian window server has no pixels until
# something composes them, and composing needs an OpenGL. There is no GPU in a
# sandbox and no host driver to borrow one from, so the core brings its own.
# softpipe is plain C: no JIT, no dispatch on host CPU features, so it draws the
# same picture on every machine - which is the only kind of renderer a core that
# replays movies can use.
#
# Everything is pinned: the tarball by its SHA256, the five patches by being in
# this repository. The build lands in build/mesa and is what build-core.sh links
# against; a core built without it still runs every game that paints the panel
# itself, and draws nothing for the ones that do not.
#
# Usage: ./setup-mesa.sh [-m <miniBox dir>] [-j N]
#   MESA_TARBALL=<path>  use a tarball already on this machine
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/tools/chimera-common-minibox}"
jobs="$(nproc)"
while getopts "m:j:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		j) jobs="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
mb="$(cd "$mb" && pwd)"
sr="$mb/build/meson-cpp/guest-sysroot"

[ -f "$sr/lib/musl-gcc.specs" ] || {
	echo "miniBox C++ guest toolchain missing at $sr." >&2
	exit 1
}

version="24.0.9"
sha256="51aa686ca4060e38711a9e8f60c8f1efaa516baf411946ed7f2c265cd582ca4c"
url="https://archive.mesa3d.org/mesa-$version.tar.xz"

out="$root/build/mesa"
cache="${CHIMERA_DEPS_DIR:-$root/build/deps}"
tarball="${MESA_TARBALL:-$cache/mesa-$version.tar.xz}"

if [ -d "$out/build-guest" ] && [ -n "$(find "$out/build-guest" -name '*.a' -print -quit 2>/dev/null)" ]; then
	echo "mesa: already built in $out"
	exit 0
fi

mkdir -p "$cache"

if [ ! -f "$tarball" ]; then
	echo "mesa: fetching $url"
	curl -fL --retry 3 -o "$tarball.part" "$url"
	mv "$tarball.part" "$tarball"
fi

echo "$sha256  $tarball" | sha256sum -c - || {
	echo "mesa: the tarball is not the one this core is pinned to" >&2
	exit 1
}

rm -rf "$out"
mkdir -p "$out"
tar -xf "$tarball" -C "$out" --strip-components=1

# The five patches, all guarded by CHIMERA_GUEST so a normal Mesa build is
# untouched: no thread pointer (a sandbox has none, and a TLS write with none
# lands wherever %fs happens to point), one CPU, and no hand-written x86-64
# dispatch stubs - those read the dispatch table out of %fs too.
apply() { # <patch> <file>
	patch -p0 -d "$out" -i "$root/patches/mesa/$1" "$2" >/dev/null
}

apply meson.build.patch meson.build
apply src-util-u_thread.h.patch src/util/u_thread.h
apply src-util-u_qsort.cpp.patch src/util/u_qsort.cpp
apply src-util-u_call_once.c.patch src/util/u_call_once.c
apply src-util-u_cpu_detect.c.patch src/util/u_cpu_detect.c

# The guest toolchain, as meson wants to hear it.
cross="$out/guest-cross.ini"
gccver="$(gcc -dumpfullversion)"
cat > "$cross" <<EOF
[binaries]
c = 'gcc'
cpp = 'g++'
ar = 'ar'
strip = 'strip'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-specs', '$sr/lib/musl-gcc.specs', '-fvisibility=hidden', '-mcmodel=large', '-mstack-protector-guard=global', '-fno-stack-protector', '-fno-pic', '-fno-pie', '-fcf-protection=none', '-DCHIMERA_GUEST', '-I$mb/extern/emulibc', '-I$mb/source/guest/include']
cpp_args = ['-specs', '$sr/lib/musl-gcc.specs', '-fvisibility=hidden', '-mcmodel=large', '-mstack-protector-guard=global', '-fno-stack-protector', '-fno-pic', '-fno-pie', '-fcf-protection=none', '-fexceptions', '-DCHIMERA_GUEST', '-I$mb/extern/emulibc', '-I$mb/source/guest/include', '-I$sr/include/c++/$gccver', '-I$sr/include/c++/$gccver/x86_64-linux-musl']
c_link_args = ['-specs', '$sr/lib/musl-gcc.specs']
cpp_link_args = ['-specs', '$sr/lib/musl-gcc.specs']
EOF

( cd "$out" && meson setup build-guest \
	--cross-file "$cross" \
	-Dllvm=disabled -Dgallium-drivers=swrast -Dvulkan-drivers= \
	-Dosmesa=true -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dplatforms= -Dshared-glapi=disabled -Dzstd=disabled \
	-Dshader-cache=disabled -Ddefault_library=static >/dev/null )

# The final libOSMesa.so link fails by design - a guest is -fno-pic and cannot
# make a shared object - and everything this core links is built by then. So
# the failure is expected, and only a missing archive is a real one.
( cd "$out" && ninja -C build-guest -j "$jobs" >/dev/null 2>&1 || true )

for lib in libglapi_static.a libmesa_util.a; do
	find "$out/build-guest" -name "$lib" | grep -q . || {
		echo "mesa: $lib was not built - the build really did fail" >&2
		exit 1
	}
done

find "$out/build-guest" -name 'target.c.o' -path '*osmesa*' | grep -q . || {
	echo "mesa: the osmesa target object is missing" >&2
	exit 1
}

echo "mesa: built in $out"
