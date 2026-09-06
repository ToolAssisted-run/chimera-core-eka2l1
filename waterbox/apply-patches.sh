#!/bin/sh
# Applies the numbered patches to the extern/eka2l1 submodule.
#
# The series is applied to a PRISTINE tree, all of it or none: patches that
# touch the same neighbourhood cannot be recognised one by one afterwards
# (git apply --reverse --check on one of them fails once another has moved the
# lines around it), and a half-applied tree is not a state worth modelling. So
# a clean tree gets the series, and a dirty one is taken to have it already.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
eka="$root/extern/eka2l1"

# The submodule has to BE one. A fresh clone made without --recursive leaves
# extern/eka2l1 an empty directory, and every git command run in it answers for
# the repository above it instead - so the series would be applied to this
# repository's own working tree, quietly, and nothing would say so.
if [ "$(git -C "$eka" rev-parse --show-toplevel 2>/dev/null)" != "$eka" ]; then
	echo "extern/eka2l1 is not checked out; run:" >&2
	echo "  git submodule update --init --recursive" >&2
	exit 1
fi

# Untracked files count too: a patch that adds a file cannot be applied twice,
# and git diff does not see one.
if [ -n "$(git -C "$eka" status --porcelain)" ]; then
	echo "patches: already applied ($(ls "$root"/patches/*.patch | wc -l) in the series)"
	exit 0
fi

for p in "$root"/patches/*.patch; do
	git -C "$eka" apply "$p" || {
		echo "FAILED to apply $(basename "$p") - the tree is now half patched;" >&2
		echo "run: git -C extern/eka2l1 reset --hard && git -C extern/eka2l1 clean -fdq," >&2
		echo "then try again" >&2
		exit 1
	}
	echo "applied: $(basename "$p")"
done
