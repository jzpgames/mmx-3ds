#!/usr/bin/env bash
# Packs build-game/mmx-3ds.elf as .3dsx and .cia. Needs devkitPro's
# 3dsxtool plus bannertool and makerom on PATH.
set -euo pipefail
cd "$(dirname "$0")"
out=dist; mkdir -p "$out"
bannertool makebanner -i cia/banner.png -a cia/banner.wav -o "$out/banner.bnr"
bannertool makesmdh -s "Mega Man X 3DS" -l "v1.0.1 - 3D estereoscopico y MSU-1" \
  -p "jzpgames" -i cia/icon.png -o "$out/mmx-3ds.smdh"
3dsxtool build-game/mmx-3ds.elf "$out/mmx-3ds.3dsx" --smdh="$out/mmx-3ds.smdh"
makerom -f cia -elf build-game/mmx-3ds.elf -rsf cia/mmx3ds.rsf -icon "$out/mmx-3ds.smdh" \
  -banner "$out/banner.bnr" -exefslogo -target t -o "$out/mmx-3ds.cia"
echo "Listo: $out/mmx-3ds.3dsx, $out/mmx-3ds.cia"
