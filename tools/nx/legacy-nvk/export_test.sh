#!/bin/bash
# Copies the NRO and config into C:\ME2_Recomp\nx_test\switch\me2-nx (Eden's sdmc/switch points there), with the
# diagnostics turned on; nx_out stays the clean copy for the console.
set -e
OUT=/mnt/c/ME2_Recomp/nx_test/switch/me2-nx
cp /root/nx/me2nx/build/switch/me2.nro "$OUT/"
{ cat /root/nx/me2nx/me2.toml; printf '\n# test overrides (export_test.sh)\nme2_diag_views = true\nme2_eden_fence_poll = true\n'; } > "$OUT/me2.toml"
cp /root/nx/me2nx/build/switch/me2 /mnt/c/ME2_Recomp/nx_test/me2.elf
ls -la "$OUT/me2.nro"
