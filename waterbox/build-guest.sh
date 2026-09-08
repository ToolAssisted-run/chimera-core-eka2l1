#!/bin/sh
# Guest (waterbox) build: the same CMake and the same options as the native
# reference, under the miniBox musl toolchain, with no host devices and no
# logging. Artifacts land in build/guest.
#
# Usage: ./build-guest.sh [-m <miniBox dir>] [extra ninja targets]
#
# The miniBox is named the same way setup-mesa.sh and build-core.sh name it, and
# for the same reason: only a developer's own tree lives at $HOME/chimera. CI
# checks Chimera out beside this repository, and a build that could not be told
# so silently used a path that does not exist - which the toolchain file only
# notices when gcc says it cannot read the specs.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/chimera-common-minibox}"
while getopts "m:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
mb="$(cd "$mb" && pwd)"
MINIBOX_SYSROOT="${MINIBOX_SYSROOT:-$mb/build/meson-cpp/guest-sysroot}"
export MINIBOX_SYSROOT
[ -f "$MINIBOX_SYSROOT/lib/musl-gcc.specs" ] || {
	echo "miniBox C++ guest toolchain missing at $MINIBOX_SYSROOT." >&2
	echo "pass -m <miniBox dir>, or build it:" >&2
	echo "  meson setup <miniBox>/build/meson-cpp <miniBox> -Dguest_cpp=true" >&2
	echo "  meson compile -C <miniBox>/build/meson-cpp" >&2
	exit 1
}
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
# Release, not RelWithDebInfo: a core.wbx carries its debug info into every
# package and every savestate's ELF hash, and three hundred megabytes of it is
# not worth the backtrace nobody can take inside a sandbox anyway.
# FFmpeg is what gives games their sound, and it is built separately because
# the submodule's prebuilts are glibc and this guest is musl. Missing, the core
# still runs every game and none of them makes a sound, so say so and carry on
# rather than refusing to build.
ff=""
if [ -f "$root/build/ffmpeg-guest/stage/lib/libavcodec.a" ]; then
	ff="$EKA2L1_GUEST_FFMPEG_OPTS"
else
	echo "no guest FFmpeg at build/ffmpeg-guest: building a silent core." >&2
	echo "run waterbox/setup-ffmpeg.sh for sound." >&2
fi
cmake -G Ninja -B "$root/build/guest" -DCMAKE_TOOLCHAIN_FILE="$here/guest-toolchain.cmake" \
	-DEKA2L1_HOST_DEVICES=OFF -DEKA2L1_BUILD_TESTS=OFF $EKA2L1_OPTS $ff \
	-DCMAKE_BUILD_TYPE=Release "$here"
ninja -C "$root/build/guest" epoc epocservs drivers epocdispatch epocpkg "$@"
