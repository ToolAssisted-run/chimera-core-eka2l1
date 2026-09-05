#!/bin/sh
# Applies the numbered patches to the extern/eka2l1 submodule. Idempotent:
# a tree that already carries the changes is left alone; anything else is an
# error worth seeing. The driver lives in waterbox/ and is built OUTSIDE the
# eka2l1 tree, so nothing is copied in.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
eka="$root/extern/eka2l1"

for p in "$root"/patches/*.patch; do
	if git -C "$eka" apply --check "$p" 2>/dev/null; then
		git -C "$eka" apply "$p"
		echo "applied: $(basename "$p")"
	elif git -C "$eka" apply --reverse --check "$p" 2>/dev/null; then
		echo "already applied: $(basename "$p")"
	else
		echo "NEITHER applies nor reverses: $(basename "$p")" >&2
		exit 1
	fi
done
