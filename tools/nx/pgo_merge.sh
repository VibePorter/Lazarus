#!/bin/bash
# Lazarus PGO, step 2 of 3 (docs/pgo.md).
#   1. PGO=generate bash tools/nx/build_switch.sh build   -> an instrumented NRO; play it (console or Ryujinx): it
#      writes pgo/sa2_pgo_<N>.stream next to the NRO, one file per session.
#   2. bash tools/nx/pgo_merge.sh <folder with the .stream files>
#      adds every session into the .gcda files of PROFILE (default ~/lazarus-pgo/profile) and fills in their summary
#      (tools/pgo_summary.py; the stream API writes it as zero). An existing profile is moved aside first, because
#      merge-stream ADDS to the files it finds.
#   3. PGO=use bash tools/nx/build_switch.sh build
set -euo pipefail
STREAMS=${1:?usage: pgo_merge.sh <folder with sa2_pgo_*.stream>}
SRC=$(cd "$(dirname "$0")/../.." && pwd)
PROFILE=${PROFILE:-$HOME/lazarus-pgo/profile}
TOOL=${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-gcov-tool
shopt -s nullglob
files=("$STREAMS"/sa2_pgo_*.stream)
[ ${#files[@]} -gt 0 ] || { echo "no sa2_pgo_*.stream in $STREAMS"; exit 1; }
if [ -d "$PROFILE" ] && ls "$PROFILE"/*.gcda >/dev/null 2>&1; then
  mv "$PROFILE" "$PROFILE.$(date +%m%d_%H%M)"
  echo "previous profile moved aside"
fi
mkdir -p "$PROFILE"
for f in "${files[@]}"; do
  echo "merging $(basename "$f") ($(( $(stat -c %s "$f") >> 20 )) MB)"
  "$TOOL" merge-stream "$f"
done
count=$(ls "$PROFILE"/*.gcda 2>/dev/null | wc -l)
[ "$count" -gt 0 ] || { echo "no .gcda in $PROFILE: the streams name another folder (the PGO_DIR of the instrumented build)"; exit 1; }
python3 "$SRC/tools/pgo_summary.py" "$PROFILE" ${#files[@]}
echo "${#files[@]} session(s) merged into $count file(s) in $PROFILE"
