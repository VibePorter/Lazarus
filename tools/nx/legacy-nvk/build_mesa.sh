#!/bin/bash
# NVK (Mesa) for the Switch, native in WSL Ubuntu-24.04, no Docker. The same steps as mesa-switch's build-switch.sh
# (Docker.rust image: devkitA64 + LLVM 15 + SPIRV-Tools 1.3.290 + Rust nightly), with Ubuntu's LLVM 18 for the host
# tools, -Doptimization=2 (masseffect-nx measured +5 % draws over upstream's 1) and the NVK build id of the SHD
# script (tools/proot/mesa_p2_build.sh). Result: $NVK_SDK/lib/libvulkan.a (NVK + NAK + its Rust runtime).
#   bash build_mesa.sh             full build (native tools, configure, ninja, merge)
#   bash build_mesa.sh --incremental
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
NX=${NX:-/root/nx}
P=${MESA_SRC:-$NX/mesa-switch}
NVK_SDK=${NVK_SDK:-$NX/nvk-sdk}
OPT=${MESA_OPT:-2}
JOBS=${JOBS:-12}
export DEVKITPRO=/opt/devkitpro
PL=$DEVKITPRO/portlibs/switch
. "$HOME/.cargo/env"
INCREMENTAL=0
[ "${1:-}" = "--incremental" ] && INCREMENTAL=1

if [ "$INCREMENTAL" = 0 ]; then
  echo "== host packages"
  apt-get install -y llvm-18-dev libclang-18-dev libclang-cpp18-dev libllvmspirvlib-18-dev libclc-18-dev \
    clang-18 python3-setuptools python3-wheel pipx >/dev/null
  # Mesa 26 wants a newer meson than Ubuntu's 1.3.
  python3 -m venv /root/nx/pyenv
  /root/nx/pyenv/bin/pip install -q meson mako pyyaml packaging ply
  # SPIRV-Tools >= 2024.1 (Ubuntu has 2023.x): the version Docker.rust pins.
  if ! pkg-config --atleast-version=2024.1 SPIRV-Tools 2>/dev/null; then
    echo "== SPIRV-Tools vulkan-sdk-1.3.290.0"
    rm -rf /tmp/SPIRV-Tools
    git clone -q --depth 1 --branch vulkan-sdk-1.3.290.0 https://github.com/KhronosGroup/SPIRV-Tools.git /tmp/SPIRV-Tools
    git clone -q --depth 1 --branch vulkan-sdk-1.3.290.0 https://github.com/KhronosGroup/SPIRV-Headers.git \
      /tmp/SPIRV-Tools/external/spirv-headers
    cmake -S /tmp/SPIRV-Tools -B /tmp/SPIRV-Tools/build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DCMAKE_BUILD_TYPE=Release -DSPIRV_SKIP_TESTS=ON >/dev/null
    ninja -C /tmp/SPIRV-Tools/build install >/dev/null
    rm -rf /tmp/SPIRV-Tools
  fi
  command -v cbindgen >/dev/null || cargo install cbindgen --locked
fi
export PATH="/root/nx/pyenv/bin:$PATH"

cd "$P"
echo "== wrappers, headers, POSIX stub archives"
mkdir -p /usr/local/libexec
cp bindgen-switch-wrapper.sh /usr/local/libexec/bindgen
cp rustc-switch-wrapper.sh /usr/local/libexec/rustc
cp bindgen-atomic-shim.h /usr/local/libexec/bindgen-atomic-shim.h
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc
cp src/gallium/winsys/nouveau/drm/nouveau.h src/nouveau/headers/nv_device_info.h "$PL/include/"
if [ ! -f "$PL/lib/libdl.a" ]; then
  echo "void __mesa_switch_stub_lib(void) {}" > /tmp/stub_posix.c
  $DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc -c /tmp/stub_posix.c -o /tmp/stub_posix.o
  for L in dl rt util; do $DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar rcs "$PL/lib/lib$L.a" /tmp/stub_posix.o; done
fi
# The wrappers call the real tools through these (see the wrapper scripts).
# Rust target: upstream builds in an aarch64 container on Apple silicon, where the host target is already AArch64;
# on an x86-64 host NAK came out as x86-64 objects ("skipping incompatible libvulkan.a" at the NRO link).
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
rustup target add "$MESA_SWITCH_RUST_TARGET" >/dev/null
export MESA_SWITCH_RUSTC=$HOME/.cargo/bin/rustc
export MESA_SWITCH_BINDGEN=$HOME/.cargo/bin/bindgen
NATIVE_PATH="/usr/local/libexec:$P/builddir-native/src/compiler/clc:$P/builddir-native/src/compiler/spirv"

if [ "$INCREMENTAL" = 0 ]; then
  echo "== native host tools (mesa_clc, vtn_bindgen2)"
  # LLVM 18: Ubuntu only ships LLVMSPIRVLib for 18, and clang-19 (installed for the host rexglue) would win otherwise.
  printf "[binaries]\nllvm-config = '/usr/bin/llvm-config-18'\n" > /root/nx/mesa-native-llvm18.ini
  meson setup builddir-native --wipe --native-file /root/nx/mesa-native-llvm18.ini -Dvulkan-drivers= -Dgallium-drivers= -Dshader-cache=true -Dplatforms= \
    -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dtools=[] -Dllvm=enabled \
    -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true
  ninja -C builddir-native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2

  echo "== configure the cross build (-Doptimization=$OPT)"
  NVK_BUILD_ID="$(find src/nouveau/compiler src/nouveau/vulkan src/nouveau/nil src/compiler/nir src/compiler/spirv -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.rs' -o -name '*.py' \) -print0 | LC_ALL=C sort -z | xargs -0 cat | sha1sum | cut -c1-40)"
  echo "NVK build id: $NVK_BUILD_ID"
  # Build-machine Rust (proc macros such as paste, built as host .so) must be the real rustc: the wrapper in PATH
  # forces the AArch64 target (SHD's mesa_p2_build.sh does the same with mesa-native.ini).
  printf "[binaries]\nrust = '%s'\n" "$HOME/.cargo/bin/rustc" > /root/nx/mesa-native-rust.ini
  PATH="$NATIVE_PATH:$PATH" meson setup builddir-switch --wipe --cross-file switch_cross_file.txt \
    --native-file /root/nx/mesa-native-rust.ini --buildtype=release \
    -Doptimization=$OPT -Db_lto=false -Db_ndebug=true -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau \
    -Dshader-cache=true -Dgallium-rusticl=false -Dplatforms=switch -Dglx=disabled -Degl=disabled -Dopengl=false \
    -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dshared-glapi=disabled -Dshared-llvm=disabled \
    -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false -Dnvk-build-id="$NVK_BUILD_ID"
fi

echo "== ninja (Switch archives)"
PATH="$NATIVE_PATH:$PATH" ninja -j"$JOBS" -C builddir-switch src/nouveau/vulkan/libnvk.a src/nouveau/vulkan/libvulkan.a \
  src/vulkan/util/libvulkan_util.a src/compiler/nir/libnir.a src/compiler/libcompiler.a src/compiler/spirv/libvtn.a \
  src/util/libxmlconfig.a src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a src/nouveau/nil/libnil.a \
  src/nouveau/nil/liblibnil_format_table.a src/nouveau/mme/libnouveau_mme.a src/nouveau/winsys/libnouveau_ws.a \
  src/nouveau/headers/libnvidia_headers_c.a src/compiler/rust/libcompiler_c_helpers.a src/util/libmesa_util.a \
  src/util/libmesa_util_simd.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a

echo "== merge libnak_rs.a into libvulkan.a"
mkdir -p "$NVK_SDK/lib"
rm -f "$NVK_SDK/lib/libvulkan.a"
cd builddir-switch
printf "CREATE $NVK_SDK/lib/libvulkan.a\nADDLIB src/nouveau/vulkan/libvulkan.a\nADDLIB src/nouveau/compiler/libnak_rs.a\nSAVE\nEND\n" \
  | $DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar -M
ls -la "$NVK_SDK/lib/libvulkan.a"
echo "mesa-switch $(git -C "$P" rev-parse --short HEAD) + SHD patches/mesa-switch.patch, -Doptimization=$OPT, native WSL build" \
  > "$NVK_SDK/SOURCE.txt"
echo "== mesa done"
