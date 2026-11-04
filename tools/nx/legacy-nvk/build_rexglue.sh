#!/bin/bash
# Host (Linux x86-64) build of the code generator from the patched masseffect-nx SDK.
# masseffect-nx only built it on macOS arm64; on x86-64 the SDK's SSSE3/AVX intrinsics need -march (host tool only).
set -e
cd /root/nx/masseffect-nx
# clang 18 + libstdc++ 13 hide std::expected (__cpp_concepts too old): clang 19.
command -v clang++-19 >/dev/null || DEBIAN_FRONTEND=noninteractive apt-get install -y clang-19 lld-19 >/dev/null
export CC=clang-19 CXX=clang++-19
if ! grep -q 'clang++-19' sdk/out/build/host/CMakeCache.txt 2>/dev/null; then rm -rf sdk/out/build/host; fi
if ! grep -q 'x86-64-v3' sdk/out/build/host/CMakeCache.txt 2>/dev/null; then rm -rf sdk/out/build/host; fi
export CFLAGS="-march=x86-64-v3" CXXFLAGS="-march=x86-64-v3"
JOBS=${JOBS:-10} tools/build_host.sh
ls -la sdk/out/host/rexglue
