#!/bin/bash
# Lazarus: builds the Switch NRO (deko3d renderer) in WSL / Linux.
#
#   bash tools/nx/build_switch.sh [sync|tools|shaders|configure|build|all]...
#
# The repository is copied to a Linux folder first (BUILD_ROOT, default ~/lazarus-build): building through /mnt/c is
# many times slower. Steps:
#   sync       copy the repository (with app/generated from tools/codegen.ps1 and sdk/thirdparty from
#              tools/fetch_thirdparty.py + tools/fetch_app_thirdparty.py) to BUILD_ROOT
#   tools      the host shader tools: SPIRV-Cross and uam (the deko3d shader compiler)
#   shaders    the game's shaders for the Switch, made from YOUR game (docs/building.md, "Shaders"):
#                SHADER_CACHE_CPP  the renderer's shader table written by the PC shader conversion (shader_cache.cpp)
#                GLSL_DIR          every game shader as GLSL (<hash>.glsl)
#                DKSH_DIR          (optional) precompiled default variants (<hash>.dksh): no compile on first use
#   configure  CMake for the Switch (devkitA64)
#   build      the NRO, then tools/deploy_switch.py copies it to OUT_SD (default <repo>/out/sd/switch/me2-deko)
#   all        everything above
#
# Variables: DEV=1 (no LTO, minutes instead of ~12 min), PGO=generate|use (+ PGO_DIR), JOBS (default 6: more can run a
# 16 GB machine out of memory), LTO_JOBS (default 8).
set -euo pipefail
SRC=$(cd "$(dirname "$0")/../.." && pwd)
DST=${BUILD_ROOT:-$HOME/lazarus-build}
OUT_SD=${OUT_SD:-$SRC/out/sd/switch/me2-deko}
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
JOBS=${JOBS:-6}
LTO_JOBS=${LTO_JOBS:-8}
if [ "${DEV:-0}" = 1 ]; then BUILD=out/nx-dev; LTO=OFF; else BUILD=out/nx; LTO=ON; fi
# PGO=generate: an instrumented NRO (out/nx-pgogen, no LTO) that writes pgo/*.stream next to itself; merge them with
# tools/nx/pgo_merge.sh, then build with PGO=use. A profile only fits the code it was recorded with.
PGO=${PGO:-}
PGO_DIR=${PGO_DIR:-$HOME/lazarus-pgo/profile}
if [ "$PGO" = generate ]; then BUILD=out/nx-pgogen; LTO=OFF; fi

step_sync() {
  mkdir -p "$DST"
  [ -d "$SRC/app/generated/default" ] || { echo "app/generated missing: run tools/codegen.ps1 -Flavor optimised first"; exit 1; }
  [ -d "$SRC/sdk/thirdparty/fmt" ] || { echo "sdk/thirdparty incomplete: run tools/fetch_thirdparty.py first"; exit 1; }
  # By content (-rlp --checksum, not -a): a file edited during a build gets the copy's time and is rebuilt.
  rsync -rlp --checksum --delete --exclude '/out/' --exclude '/assets/' --exclude '/.git/' \
        --exclude '/app/out/renut_switch_release/' "$SRC/" "$DST/"
  # Files from Windows can lose their exec bit on the way: the tools the build runs.
  find "$DST/app/thirdparty/reblue-XenosRecomp/thirdparty/dxc-bin" -name 'dxc-linux' -exec chmod +x {} + 2>/dev/null || true
  find "$DST/sdk" "$DST/tools" -name '*.sh' -exec chmod +x {} +
  # FFmpeg's config.h for Horizon (upstream's says "no config" there).
  cp -p "$SRC/tools/nx/ffmpeg_config.h" "$DST/sdk/thirdparty/FFmpeg/config.h"
  grep -q "^ZSTD = 'zstd'" "$DST/tools/deko3d_shader_package.py" ||
    sed -i "s|^ZSTD = .*|ZSTD = 'zstd'|" "$DST/tools/deko3d_shader_package.py"
}

step_tools() {
  mkdir -p "$DST/out/deko3d_tools" && cd "$DST/out/deko3d_tools"
  if [ ! -d SPIRV-Cross-vulkan-sdk-1.4.304.0 ]; then
    git clone -q --depth 1 --branch vulkan-sdk-1.4.304.0 https://github.com/KhronosGroup/SPIRV-Cross.git \
      SPIRV-Cross-vulkan-sdk-1.4.304.0
  fi
  cmake -S SPIRV-Cross-vulkan-sdk-1.4.304.0 -B spirv-cross-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF > tools.log
  cmake --build spirv-cross-build --target spirv-cross -j "$JOBS" >> tools.log
  cmake -S "$DST/app/thirdparty/uam" -B uam-build -G Ninja -DCMAKE_BUILD_TYPE=Release >> tools.log
  cmake --build uam-build --target uam_tool -j "$JOBS" >> tools.log
  ls -la spirv-cross-build/spirv-cross uam-build/uam
}

step_shaders() {
  cd "$DST"
  : "${SHADER_CACHE_CPP:?set SHADER_CACHE_CPP to the PC conversion's shader_cache.cpp (docs/building.md)}"
  : "${GLSL_DIR:?set GLSL_DIR to the folder of the game shaders' GLSL (docs/building.md)}"
  # The renderer's shader table for the Switch from the PC one; the release layout keeps the table without SPIR-V and
  # puts every shader's GLSL in deko3d_glsl.bin beside the NRO.
  mkdir -p out/pcsc/renut_gpu out/glsl
  cp "$SHADER_CACHE_CPP" out/pcsc/renut_gpu/shader_cache.cpp
  python3 tools/renut_switch_shader_cache.py --pc out/pcsc
  rsync -a --delete "$GLSL_DIR/" out/glsl/
  # 540p (render-scale code 4 = 3/4 on deko3d, render_scale.h): the 8 shaders that read the code must say 0.75
  # (tools/me2_scale34_patch.py). A conversion without that patch is refused.
  if grep -l "== 4u) ? 3\.0 :" out/glsl/*.glsl 2>/dev/null | head -1 | grep -q .; then
    echo "build_switch: GLSL with the scale code 4 = 3.0: run tools/me2_scale34_patch.py on the conversion"; exit 1
  fi
  python3 tools/renut_release_shaders.py --glsl out/glsl
  # Precompiled default variants (optional): deko3d_shaders.bin beside the NRO, and its record index.
  DKSH_DIR=${DKSH_DIR:-} python3 - <<'EOF'
import os, struct, sys
from pathlib import Path
sys.path.insert(0, 'tools')
import deko3d_variant_cache
dksh_dir = os.environ.get('DKSH_DIR')
warm = {}
if dksh_dir:
    for glsl in sorted(Path('out/glsl').glob('*.glsl')):
        dksh = Path(dksh_dir) / (glsl.stem + '.dksh')
        if dksh.is_file():
            warm[deko3d_variant_cache.fnv1a64(glsl.read_bytes())] = dksh.read_bytes()
out = bytearray(b'BDKC' + struct.pack('<I', 1) + deko3d_variant_cache.TAG)
for key, d in sorted(warm.items()):
    out += struct.pack('<QI', key, len(d)) + d
target = Path('app/out/renut_switch_release/sd/deko3d_shaders.bin')
target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(out)
print(f'deko3d_shaders.bin: {len(warm)} default variants, {len(out) >> 20} MB')
EOF
  python3 tools/deko3d_cache_index.py app/out/renut_switch_release/sd/deko3d_shaders.bin
  ls -la app/out/renut_switch_release/sd
}

step_configure() {
  cd "$DST"
  cmake -S app -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DMGR_SWITCH_LTO=$LTO \
    -DCMAKE_TOOLCHAIN_FILE="$DST/tools/switch/cmake/switch-devkitA64.cmake" -DDEVKITPRO="$DEVKITPRO" \
    -DREXGLUE_SWITCH_NO_NVK=ON \
    -DMGR_RENUT_RENDERER=ON -DMGR_NATIVE_RENDERER=OFF -DMGR_RENUT_RELEASE_SHADERS=ON \
    -DREXGLUE_RECOMP_DEBUG_INFO=none -DREXGLUE_BUILD_HOST_TOOLS=OFF \
    -DPYTHON_EXECUTABLE=/usr/bin/python3 -DPython3_EXECUTABLE=/usr/bin/python3 \
    -DBANJO_DEKO3D_SPIRV_CROSS="$DST/out/deko3d_tools/spirv-cross-build/spirv-cross" \
    -DBANJO_DEKO3D_UAM="$DST/out/deko3d_tools/uam-build/uam" -DME2_PGO=$PGO -DME2_PGO_DIR="$PGO_DIR" \
    > out/nx-configure.log 2>&1 || { tail -40 out/nx-configure.log; exit 1; }
  tail -3 out/nx-configure.log
}

step_build() {
  cd "$DST"
  [ -f $BUILD/CMakeCache.txt ] || step_configure
  if [ "$LTO" = ON ] && ! grep -q "^MGR_SWITCH_LTO_JOBS:STRING=$LTO_JOBS\$" $BUILD/CMakeCache.txt 2>/dev/null; then
    cmake $BUILD -DMGR_SWITCH_LTO_JOBS=$LTO_JOBS > /dev/null
  fi
  # The PGO mode is kept in the build tree: set it again when it changed.
  if ! grep -q "^ME2_PGO:STRING=$PGO\$" $BUILD/CMakeCache.txt || ! grep -q "^ME2_PGO_DIR:PATH=$PGO_DIR\$" $BUILD/CMakeCache.txt; then
    cmake $BUILD -DME2_PGO=$PGO -DME2_PGO_DIR="$PGO_DIR" > /dev/null
  fi
  local t0=$(date +%s)
  cmake --build $BUILD --target sa2 -j "$JOBS" > $BUILD-build.log 2>&1 ||
    { grep -n -m 20 -i "error" $BUILD-build.log; tail -20 $BUILD-build.log; exit 1; }
  echo "build took $(( $(date +%s) - t0 )) s (JOBS=$JOBS, LTO_JOBS=$LTO_JOBS)"
  # The NRO goes to OUT_SD as me2_opt.nro; its unstripped ELF is kept in <repo>/out/deployments/<sha256> for crash
  # stacks (docs/debugging.md). The shader files of step 'shaders' are copied beside it.
  python3 "$SRC/tools/deploy_switch.py" --nro "$DST/$BUILD/sa2.nro" --elf "$DST/$BUILD/sa2" --sd-dir "$OUT_SD"
  for f in deko3d_glsl.bin deko3d_shaders.bin deko3d_shaders.bin.idx; do
    [ -f "app/out/renut_switch_release/sd/$f" ] && cp -p "app/out/renut_switch_release/sd/$f" "$OUT_SD/" || true
  done
}

steps=("$@"); [ ${#steps[@]} -eq 0 ] && steps=(all)
for s in "${steps[@]}"; do
  if [ "$s" = all ]; then step_sync; step_tools; step_shaders; step_configure; step_build
  else "step_$s"; fi
done
