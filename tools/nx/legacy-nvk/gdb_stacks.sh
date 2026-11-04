#!/bin/bash
# Attach to Eden's GDB stub, let the NRO run for $1 s (default 30), interrupt, dump modules and all thread stacks.
T=${1:-30}
GDB=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gdb
cat > /tmp/stacks.gdb <<'G'
set pagination off
set confirm off
target remote 127.0.0.1:6543
continue
monitor get info
monitor get mappings
info threads
thread apply all bt 25
G
timeout --foreground -s INT "$T" $GDB -q -batch -x /tmp/stacks.gdb /root/nx/me2nx/build/switch/me2 2>&1 | tee /root/gdb_stacks.log | tail -150
