#!/bin/bash
# Copies the NRO and its config to C:\ME2_Recomp\nx_out\switch\me2-nx (the SD card layout, without game files).
set -e
OUT=/mnt/c/ME2_Recomp/nx_out/switch/me2-nx
mkdir -p "$OUT"
cp /root/nx/me2nx/build/switch/me2.nro "$OUT/"
cp /root/nx/me2nx/me2.toml "$OUT/"
cp /root/nx/me2nx/build/switch/me2 /mnt/c/ME2_Recomp/nx_out/me2.elf   # unstripped, for addr2line on crash logs
ls -la "$OUT" /mnt/c/ME2_Recomp/nx_out/me2.elf
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm -n /root/nx/me2nx/build/switch/me2 | grep -m3 ' [Tt] '
