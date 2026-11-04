#!/bin/bash
# Syncs the ME2-specific files from Windows and builds the NRO in /root/nx/me2nx.
set -e
cp -r /mnt/c/ME2_Recomp/tools/nx/app/. /root/nx/me2nx/
cp /mnt/c/ME2_Recomp/tools/nx/prepare_proto_codegen.py /root/nx/me2nx/tools/
# ME2's Direct3D device pointer / offsets and library name in the copied Mass Effect NX renderer.
python3 /mnt/c/ME2_Recomp/tools/nx/me2_native_patch.py /root/nx/me2nx/src
# Shaders the patch changed: rebuild their SPIR-V headers (the comments in each .glsl give the command).
( cd /root/nx/me2nx/src/native_p2/shaders &&
  glslangValidator -S comp -V --target-env vulkan1.1 --vn kDepthResolveCs -o depth_resolve_cs.h.new depth_resolve.cs.glsl >/dev/null &&
  { cmp -s depth_resolve_cs.h.new depth_resolve_cs.h && rm depth_resolve_cs.h.new || mv depth_resolve_cs.h.new depth_resolve_cs.h; } )
# The fork's FFmpeg/config.h is missing from the public masseffect-nx (see ffmpeg_config.h).
cmp -s /mnt/c/ME2_Recomp/tools/nx/ffmpeg_config.h /root/nx/masseffect-nx/sdk/thirdparty/FFmpeg/config.h || cp /mnt/c/ME2_Recomp/tools/nx/ffmpeg_config.h /root/nx/masseffect-nx/sdk/thirdparty/FFmpeg/config.h
cd /root/nx/me2nx
# Build number + time, logged at startup (me2_cvars.cpp Me2LogBuild), so a console log names its build.
n=$(( $(cat build_number 2>/dev/null || echo 0) + 1 ))
echo $n > build_number
printf '#pragma once\n#define ME2_BUILD_NUMBER %d\n#define ME2_BUILD_TIME "%s"\n' "$n" "$(date '+%Y-%m-%d %H:%M')" \
  > src/me2_build_info.h
echo "== build #$n"
JOBS=${JOBS:-12} bash build.sh
