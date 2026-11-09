#!/usr/bin/env bash
# Applies the 3DS runtime changes to the snesrecomp submodule (pinned at
# e9f5f65f). Run once after cloning with --recurse-submodules.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
patch="$here/patches/snesrecomp-3ds.patch"
if git -C "$root/snesrecomp" apply --reverse --check "$patch" 2>/dev/null; then
  echo "snesrecomp: 3DS patch already applied"
else
  git -C "$root/snesrecomp" apply "$patch"
  echo "snesrecomp: 3DS patch applied"
fi
