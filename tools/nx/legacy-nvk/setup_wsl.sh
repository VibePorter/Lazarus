#!/bin/bash
# One-time setup of the Switch build environment in WSL Ubuntu-24.04 (run as root, no Docker):
#   wsl -d Ubuntu-24.04 -u root -- bash /mnt/c/ME2_Recomp/tools/nx/setup_wsl.sh
# Sources live on ext4 under $NX (default /root/nx); the Windows tree is only read for patches.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
NX=${NX:-/root/nx}
SHD=/mnt/c/ME2_Recomp/silent-hill-downpour-switch
# The Windows clone has CRLF patches (autocrlf): use LF copies.
mkdir -p "$NX/patches"
for f in "$SHD"/patches/*.patch; do sed 's/\r$//' "$f" > "$NX/patches/$(basename "$f")"; done
PATCHES=$NX/patches
# SDK base of the SHD port: its README says a85b0b4 (not public); patches/rexglue-sdk-masseffect-nx.patch applies
# cleanly to 96b6e09 (2026-10-07) and to no newer public commit.
ME_NX_SDK_REV=96b6e09
ME_NX_SHADER_REV=650e14e   # XenosRecomp base of the SHD shader pipeline
MESA_REV=d4a00ea0ab3f59afb967cc5d779e4263d237bd77

echo "== apt"
apt-get update
apt-get install -y build-essential clang lld cmake ninja-build python3 python3-pip python3-venv git curl wget \
  pkg-config meson glslang-tools spirv-tools libxxhash-dev libfmt-dev xxhash bison flex python3-mako \
  python3-yaml python3-packaging python3-ply libclang-dev llvm-dev libelf-dev zlib1g-dev libzstd-dev \
  libexpat1-dev unzip file

echo "== devkitPro"
# apt.devkitpro.org answers 403 from here, so /opt/devkitpro comes from the devkitpro/devkita64 Docker image
# (devkitA64, libnx and the switch portlibs), exported once on Windows:
#   docker create --name dka64tmp devkitpro/devkita64; docker export dka64tmp -o dka64.tar; docker rm dka64tmp
#   wsl -u root -- tar -C / -xf dka64.tar opt/devkitpro
[ -x /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc ] || { echo "error: /opt/devkitpro missing (see above)" >&2; exit 1; }

echo "== rust (for NAK, the NVK shader compiler)"
if [ ! -x "$HOME/.cargo/bin/rustc" ]; then
  curl -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain nightly --profile minimal
fi
. "$HOME/.cargo/env"
rustup component add rust-src
command -v bindgen >/dev/null || cargo install bindgen-cli --locked

mkdir -p "$NX"
cd "$NX"
echo "== masseffect-nx (SDK at $ME_NX_SDK_REV)"
[ -d masseffect-nx/.git ] || git clone https://github.com/nebadasSwifty/masseffect-nx.git masseffect-nx
cd masseffect-nx
if git apply --reverse --check "$PATCHES/rexglue-sdk-masseffect-nx.patch" 2>/dev/null; then
  echo "SDK patch already applied"
else
  git checkout -q "$ME_NX_SDK_REV"
  git apply "$PATCHES/rexglue-sdk-masseffect-nx.patch"
fi
bash /mnt/c/ME2_Recomp/tools/nx/fix_sampler.sh 2>/dev/null || true
python3 tools/fetch_thirdparty.py
cd "$NX"
[ -e masseffect-nx-shaders/.git ] || git -C masseffect-nx worktree add "$NX/masseffect-nx-shaders" "$ME_NX_SHADER_REV"
(cd masseffect-nx-shaders && { git apply --reverse --check "$PATCHES/masseffect-nx-xenosrecomp.patch" 2>/dev/null || \
  git apply "$PATCHES/masseffect-nx-xenosrecomp.patch"; })

echo "== mesa-switch at $MESA_REV"
[ -d mesa-switch/.git ] || git clone --no-checkout https://github.com/danfromtico/mesa-switch.git mesa-switch
cd mesa-switch
if git apply --reverse --check "$PATCHES/mesa-switch.patch" 2>/dev/null; then
  echo "mesa patch already applied"
else
  git checkout -q "$MESA_REV"
  git apply "$PATCHES/mesa-switch.patch"
fi
echo "== setup done: $NX"
