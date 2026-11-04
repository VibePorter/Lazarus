#!/bin/bash
# Lazarus: collects every Xenos shader container of YOUR game into one folder, the input of XenosRecomp
# (docs/building.md, "Shaders"). Linux / WSL / Git Bash with a C++20 compiler.
#
#   bash tools/deko/collect_shaders.sh <game folder> <out folder> [<DLC folder> ...]
#
#   1. ue3_shader_scan: decompresses every package (*.xxx, LZO chunks) and writes each shader container it finds as
#      {vs,ps}_<fnv64>.bin (material and global shaders cooked into the packages; ~38,500 for the game + DLC).
#   2. extract_unaligned_containers.py: the global shader files keep their containers at any byte offset
#      (BioGame/CookedXenon/GlobalShaders-Xbox360.bin, media/effects/SimpleShaders.fxobj).
# A few dozen shaders are compiled by the game at run time (from HLSL) and are in no file; the renderer logs any shader
# it does not find in its table.
set -euo pipefail
GAME=${1:?usage: collect_shaders.sh <game folder> <out folder> [<DLC folder> ...]}
OUT=${2:?usage: collect_shaders.sh <game folder> <out folder> [<DLC folder> ...]}
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
CXX=${CXX:-clang++}
BIN=${SCAN_BIN:-$HERE/../../out/tools/ue3_shader_scan}
mkdir -p "$(dirname "$BIN")" "$OUT"
"$CXX" -O2 -std=c++20 -o "$BIN" "$HERE/ue3_shader_scan.cpp"
for root in "$GAME" "$@"; do
  find "$root" -iname '*.xxx' -print0 | xargs -0 -n 200 "$BIN" "$OUT"
done
python3 "$HERE/extract_unaligned_containers.py" "$OUT" \
  "$GAME/BioGame/CookedXenon/GlobalShaders-Xbox360.bin" "$GAME/media/effects/SimpleShaders.fxobj"
echo "containers: $(ls "$OUT" | wc -l) ($(ls "$OUT" | grep -c '^vs_') vertex shaders)"
