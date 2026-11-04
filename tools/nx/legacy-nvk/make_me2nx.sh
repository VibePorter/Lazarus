#!/bin/bash
# Creates /root/nx/me2nx: the Mass Effect 2 Switch app, derived from silent-hill-downpour-switch/shd.
# Stage 2 goal: SDK Xenos GPU emulation on the Switch (no game-specific D3D hooks yet).
# Re-runnable: only copies what is missing, never overwrites edited files.
set -euo pipefail
NX=${NX:-/root/nx}
SHD=/mnt/c/ME2_Recomp/silent-hill-downpour-switch/shd
PC=/mnt/c/ME2_Recomp/me2recomp
APP=$NX/me2nx
mkdir -p "$APP/src" "$APP/tools" "$APP/cmake"

cp -rn "$SHD/src/native" "$SHD/src/native_p2" "$APP/src/"
# No shd_splash.cpp: the SDK splash (nvInitialize/nvExit before NVK) hangs Eden in tmemWaitForPermission.
for f in guest_pacing_stats.h me_packaged.cpp me_packaged.h p2_compat.cpp proto_pgo.cpp proto_speed.cpp \
         shd_render_scale.cpp; do
  cp -n "$SHD/src/$f" "$APP/src/$f"
done
cp -n "$SHD/cmake/switch-devkitA64.cmake" "$APP/cmake/"
cp -n "$SHD/switch_release_perf.cpp" "$SHD/icon.jpg" "$APP/"
cp -rn "$SHD/shaders" "$APP/"
cp -n "$SHD/tools/patch_generated.py" "$SHD/tools/prepare_no_volatile.py" "$APP/tools/"

# Game: the PC project's merged D1+D2 tree (hardlinks); codegen reads only default.xex.
ln -sfn "$PC/assets" "$NX/me2-game"
# Function overrides found on PC (same SDK v0.10 codegen format). Strip the UTF-8 BOM.
[ -f "$APP/overrides.toml" ] || sed '1s/^\xEF\xBB\xBF//' "$PC/me2_config.toml" > "$APP/overrides.toml"

# shd -> me2 in the generated-file names the copied tools expect.
sed -i 's/shd_pch\.h/me2_pch.h/g; s/shd_recomp\./me2_recomp./g; s/prototype_nx/me2nx/g' "$APP/tools/prepare_no_volatile.py"
sed -i 's|generated/default/shd_pch.h|generated/default/me2_pch.h|' "$APP/src/proto_speed.cpp"
# The ME2-specific files are kept on Windows (tools/nx/app) and always win.
cp -r /mnt/c/ME2_Recomp/tools/nx/app/. "$APP/"
cp /mnt/c/ME2_Recomp/tools/nx/prepare_proto_codegen.py "$APP/tools/"
chmod +x "$APP/build.sh"
echo "me2nx created at $APP"
