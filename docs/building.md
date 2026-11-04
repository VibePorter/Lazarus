# 🛠️ Building Lazarus

From your Xbox 360 discs to `me2_opt.nro` and its shader files. The PC side (code generator, shader conversion) runs on
**Windows**; the Switch NRO is built in **WSL** (Ubuntu 24.04) or on Linux with devkitA64.

> [!IMPORTANT]
> Everything made in these steps from the game (`assets/`, `app/generated/`, the shader folders under `out/`, the
> NRO) stays on your machine. `.gitignore` keeps it out of git: never force-add it.

> [!NOTE]
> This is the maintainer's working flow, written down from the scripts that produced the tested builds. The chain has
> not yet been re-run end to end from a fresh clone; if a step fails for you, please open an issue with its log.

## 0. What you need

| | |
| :--- | :--- |
| Windows 10/11 | **Visual Studio 2022 Build Tools** (MSVC libraries + Windows SDK), **LLVM/clang 20**, **CMake** 3.28+, **Ninja**, **Git**, **Python 3.10+** (with Pillow for the icon tools) |
| WSL | **Ubuntu 24.04** with devkitPro (devkitA64, libnx, switch portlibs): `sudo bash tools/nx/setup_wsl.sh` checks and explains |
| Disk | ~60 GB (game 9.4 GB, generated C++ ~300 MB, shader work folders ~2 GB, build trees ~20 GB) |
| RAM | 16 GB: keep `JOBS` at 6 or less for the Switch build, the LTO link alone takes ~8 GB |
| Your game | both discs of *Mass Effect 2* (Xbox 360) and its **Title Update 2** package; DLC packages if you have them |

`tools\win\env.ps1` loads the Windows toolchain into a PowerShell session. It looks in the default install folders;
set `SA2_LLVM`, `SA2_VSDIR`, `SA2_CMAKE_DIR` or `SA2_NINJA_DIR` if yours differ.

## 1. Source and third-party code

```sh
git clone https://github.com/VibePorter/Lazarus.git
cd Lazarus
python tools/fetch_thirdparty.py        # sdk/thirdparty: ReXGlue SDK v0.10.0 and its submodules (~400 MB)
python tools/fetch_app_thirdparty.py    # app/thirdparty: plume's and XenosRecomp's submodules at pinned commits
```

The repository keeps only the third-party files this port changed; both scripts add everything else and never
overwrite a tracked file.

## 2. The game files

```sh
python tools/extract_iso.py "<disc 1>.iso" -o out/disc1
python tools/extract_iso.py "<disc 2>.iso" -o out/disc2
python tools/merge_discs.py assets/game_root out/disc1 out/disc2
python tools/stfs_extract.py "<Title Update 2 package>" out/tu2
```

Then copy `out/tu2/.../disc001/default.xexp` next to `assets/game_root/default.xex`, and the update's
`BioGame_Update_<LANG>.tlk` files into `assets/update/` (the PC test scripts mount that folder as `UPDATE:`).

**Russian release only?** It works the same way: extract it (Games on Demand packages with `tools/god_extract.py
<header> extract-all <folder>`, see [install.md](install.md#russian)), merge both discs into `assets/game_root` and add
the Russian TU2 files. The Russian program is byte for byte the English one once patched, so the generated code and the
NRO are the same.

## 3. The code generator and the game's C++

```powershell
. tools\win\env.ps1
cd sdk
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config Release --target rexglue
cd ..
powershell -NoProfile -ExecutionPolicy Bypass -File tools\codegen.ps1 -Flavor optimised
```

`codegen.ps1` translates `assets\game_root\default.xex` (the SDK applies the sibling `default.xexp` first) into
`app\generated\`, then runs the post-codegen passes of `tools\post_codegen.py` (registers as locals, direct calls,
AArch64 rewrites, ...). It also writes `out\default_image.bin`, the patched image, which the analysis tools read. The
log is `out\codegen.log`.

## 4. Shaders

The renderer draws with the game's own shaders, converted ahead of time. Four stages:

```text
your game ──collect──▶ ~38,700 containers ──XenosRecomp──▶ SPIR-V + shader_cache.cpp ──SPIRV-Cross/uam──▶ GLSL + DKSH
```

**4a. Collect the containers** (WSL or Git Bash):

```sh
bash tools/deko/collect_shaders.sh assets/game_root out/shader_containers [<DLC folder> ...]
```

**4b. Build the tools** (Windows): SPIRV-Cross, XenosRecomp (Renut flavour) and uam.

```powershell
git clone --depth 1 --branch vulkan-sdk-1.4.304.0 https://github.com/KhronosGroup/SPIRV-Cross.git `
    out\deko3d_tools\SPIRV-Cross-vulkan-sdk-1.4.304.0
Set-Content out\deko3d_tools\spirv-cross-revision.txt 'vulkan-sdk-1.4.304.0'
powershell -File tools\win\build_deko_shader_tools.ps1
powershell -File tools\win\build_uam.ps1
```

**4c. Translate** (Windows, ~10 min): every container to SPIR-V and the renderer's shader table.

```powershell
. tools\win\env.ps1
$env:BANJO_DEKO3D_SPIRV_DIR = "$PWD\out\shaders_xenos\spirv"
out\deko3d_tools\xenos-build\XenosRecomp\XenosRecomp.exe out\shader_containers `
    out\shaders_xenos\shader_cache.cpp app\thirdparty\reblue-XenosRecomp\XenosRecomp\shader_common.h out\shaders_xenos\hlsl
```

XenosRecomp follows the Xbox 360's multiply rule (0 × ∞ = 0) in the particle vertex shaders that need it
(`ME2_DX9_MULTIPLY=vs|all|0` widens or disables it): without it some particles vanish on the Switch.

**4d. GLSL and DKSH** (Windows):

```powershell
python tools\deko3d_shaders.py --spirv out\shaders_xenos\spirv --out out\me2-deko-shaders\conversion --jobs 8
Select-String -List -SimpleMatch '>> 26u) & 7u' out\me2-deko-shaders\conversion\glsl\*.glsl |
    ForEach-Object Path | Set-Content out\scale34_list.txt
python tools\me2_scale34_patch.py out\scale34_list.txt
```

The last two lines patch the 8 shaders that read the render-scale code, so that 540p works on deko3d
(`render_scale.h`); the Switch build refuses a conversion without it.

**4e. The shaders the game compiles while it runs.** A few dozen shaders (86 in the maintainer's game) are compiled
from HLSL by the game at run time and are in no file. Any build of the port writes each shader missing from its table
to `shader_capture/` next to the program (`renut_shader_capture_dir`). Play the PC build (or the Switch build) through
the prologue, a hub and a fight, copy the `.bin` files of `shader_capture/` into `out/shader_containers`, and run 4c
and 4d again. Until then, the effects that use them can be missing.

## 5. The Switch NRO (WSL)

```sh
sudo bash tools/nx/setup_wsl.sh            # once
export SHADER_CACHE_CPP=/mnt/c/<path>/Lazarus/out/shaders_xenos/shader_cache.cpp
export GLSL_DIR=/mnt/c/<path>/Lazarus/out/me2-deko-shaders/conversion/glsl
export DKSH_DIR=/mnt/c/<path>/Lazarus/out/me2-deko-shaders/conversion/dksh
bash tools/nx/build_switch.sh              # sync, tools, shaders, configure, build
```

The result is in `out/sd/switch/me2-deko/`: `me2_opt.nro`, `deko3d_glsl.bin`, `deko3d_shaders.bin` and its `.idx`.
The unstripped ELF of every build is kept in `out/deployments/<sha256 of the NRO>/me2_opt.elf` for crash stacks.

Useful variations:

| | |
| :--- | :--- |
| `DEV=1 bash tools/nx/build_switch.sh build` | no LTO: a one-file change links in minutes instead of ~12 |
| `bash tools/nx/build_switch.sh sync build` | after a source change (the shaders step is only needed once) |
| `PGO=generate` / `PGO=use` | profile-guided builds, see [pgo.md](pgo.md) |
| `JOBS=4 LTO_JOBS=6` | less memory |

## 6. The PC build (optional)

The same app also builds for Windows (Vulkan through plume), handy for debugging with RenderDoc:

```powershell
. tools\win\env.ps1
cd app; cmake --preset win-pc; cd ..               # configures out\pc
powershell -File tools\win\run.ps1 out\pc_build.log "cmake --build out\pc --target sa2"
powershell -File tools\win\smoke_me2_pc.ps1 -Height 720     # starts it with test settings, reaches the menu
```

## When the game changes

A different `default.xex` / `default.xexp` means different addresses: the hooks are checked at start-up against
instruction "witnesses" (`app/src/renut_engine/gpu/d3d/me2_hook_map.h`, written by `tools/me2_renut_hooks.py`) and
the log reports any mismatch. Lazarus targets **Title Update 2** only; [title-update-2.md](title-update-2.md) explains how the base-game
addresses were moved.
