#!/bin/bash
# ME2 fixes on the Switch SDK (masseffect-nx + SHD patch), the same as the local patches of the PC SDK. Idempotent.
set -e
cd /root/nx/masseffect-nx/sdk
# DLC: xeXamContentCreate kept root_name as a string_view into a guest buffer that the deferred completion outlives
# ('Invalid UTF-8' FATAL when the game mounts its DLC on PC).
f=src/kernel/xam/xam_content.cpp
if grep -q 'root_name = root_name.value()' $f; then
  sed -i 's/root_name = root_name\.value()/root_name = std::string(root_name.value())/' $f
  echo "xam_content: root_name copied"
fi
grep -n 'root_name = ' $f | head -3
python3 /mnt/c/ME2_Recomp/tools/nx/sdk_local_fixes.py /root/nx/masseffect-nx/sdk
