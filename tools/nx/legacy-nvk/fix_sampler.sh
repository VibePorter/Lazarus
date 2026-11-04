#!/bin/bash
# The SHD SDK patch adds guest_stack_sampler.* without the sdk/ prefix: move them where sdk/ expects them.
set -e
cd /root/nx/masseffect-nx
for f in include/rex/system/guest_stack_sampler.h src/system/guest_stack_sampler.cpp src/system/guest_stack_sampler.h; do
  [ -f "$f" ] && { mkdir -p "sdk/$(dirname "$f")"; mv "$f" "sdk/$f"; echo "moved $f"; }
done
ls sdk/src/system/guest_stack_sampler.* sdk/include/rex/system/guest_stack_sampler.h
