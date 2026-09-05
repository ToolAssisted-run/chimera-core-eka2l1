#!/bin/sh
# Upstream's own CPU differential harness, in its own build directory.
#
# It is separate on purpose. Building it defines EKA2L1_DYNCOM_DIFFTEST on the
# cpu target PUBLICLY - it switches in a test-only VFP selector so one process
# can compare host-fast against reference softfloat - and the machine this core
# ships must not be built with that. So the harness gets its own tree, and the
# gate runs the binary rather than linking it.
#
# What it proves: the interpreter this core runs on is right. 200,000 single
# instructions against a golden ALU model, and thousands of whole programs
# against dynarmic as an independent oracle.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
cmake -G Ninja -B "$root/build/difftest" $EKA2L1_OPTS -DCMAKE_BUILD_TYPE=Release \
	-DEKA2L1_BUILD_TESTS=OFF -DEKA2L1_BUILD_DYNCOM_DIFFTEST=ON "$here"
ninja -C "$root/build/difftest" dyncom_difftest
