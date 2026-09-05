#!/bin/sh
# Guest (waterbox) build: the same CMake and the same options as the native
# reference, under the miniBox musl toolchain, with no host devices and no
# logging. Artifacts land in build/guest.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
cmake -G Ninja -B "$root/build/guest" -DCMAKE_TOOLCHAIN_FILE="$here/guest-toolchain.cmake" \
	-DEKA2L1_HOST_DEVICES=OFF -DEKA2L1_BUILD_TESTS=OFF $EKA2L1_OPTS "$here"
ninja -C "$root/build/guest" epoc epocservs drivers epocdispatch epocpkg "$@"
