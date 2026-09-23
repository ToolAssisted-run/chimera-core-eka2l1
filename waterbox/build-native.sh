#!/bin/sh
# Native reference build: this directory's CMake at the top, upstream's
# underneath, the canonical option set. Artifacts land in build/native.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
# the same FFmpeg the guest has (setup-ffmpeg.sh -n): with none, the machine
# cannot open a sound stream and is not the machine the sandbox runs
sh "$here/setup-ffmpeg.sh" -n
cmake -G Ninja -B "$root/build/native" $EKA2L1_OPTS -DEKA2L1_BUILD_FFMPEG=ON \
	-DEKA2L1_FFMPEG_GUEST_ROOT="$root/build/ffmpeg-native/stage" "$here"
ninja -C "$root/build/native" run-native install-device ekatests "$@"
