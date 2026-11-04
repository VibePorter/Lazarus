#!/bin/bash
# Lazarus: one-time setup of the Switch build environment on Ubuntu 24.04 (WSL or native), run as root:
#   sudo bash tools/nx/setup_wsl.sh
# Installs the host packages, then checks devkitPro (devkitA64 + libnx + switch portlibs) in /opt/devkitpro.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

echo "== host packages"
apt-get update
apt-get install -y build-essential clang lld cmake ninja-build python3 python3-pip git curl wget rsync zstd \
  pkg-config file unzip

echo "== devkitPro"
if [ ! -x /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc ]; then
  cat >&2 <<'EOF'
/opt/devkitpro is missing. Install devkitA64 with the switch-dev group (https://devkitpro.org/wiki/Getting_Started):
    dkp-pacman -S switch-dev switch-portlibs
If the devkitPro package server is not reachable from your network, take /opt/devkitpro from the official Docker
image instead (on a machine with Docker):
    docker create --name dka64tmp devkitpro/devkita64
    docker export dka64tmp -o dka64.tar && docker rm dka64tmp
    sudo tar -C / -xf dka64.tar opt/devkitpro
EOF
  exit 1
fi
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc --version | head -1
echo "== ready: bash tools/nx/build_switch.sh (see docs/building.md)"
