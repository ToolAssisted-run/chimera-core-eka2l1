#!/bin/sh
# Native reference build: this directory's CMake at the top, upstream's
# underneath, the canonical option set. Artifacts land in build/native.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
cmake -G Ninja -B "$root/build/native" $EKA2L1_OPTS "$here"
ninja -C "$root/build/native" run-native ekatests "$@"
