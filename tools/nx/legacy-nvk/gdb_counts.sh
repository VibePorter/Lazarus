#!/bin/bash
# Counts how often key functions run (breakpoints with a huge ignore count), interrupting after $1 s.
T=${1:-40}
GDB=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gdb
cat > /tmp/counts.gdb <<'G'
set pagination off
set confirm off
target remote 127.0.0.1:6543
break rex::runtime::MMIOHandler::CheckStore
break rex::graphics::GraphicsSystem::WriteRegisterThunk
break rex::graphics::CommandProcessor::UpdateWritePointer
break sub_82A90380
ignore 1 100000000
ignore 2 100000000
ignore 3 100000000
ignore 4 100000000
continue
info breakpoints
G
timeout --foreground -s INT "$T" $GDB -q -batch -x /tmp/counts.gdb /root/nx/me2nx/build/switch/me2 2>&1 | grep -v "Internal error" | tee /root/gdb_counts.log | tail -24
