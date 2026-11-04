# 🧭 How Lazarus works

```text
 default.xex + .xexp ──▶ ReXGlue code generator ──▶ app/generated (C++ of every guest function)
                                                          │
           app/me2_src (game hooks, natives) ─────────────┤
           app/src/renut_engine (renderer) ───────────────┤──▶ devkitA64 + LTO + PGO ──▶ me2_opt.nro
           sdk/ (Xbox 360 kernel, XAM, audio, input, FS) ─┘
                                                                 at run time
 game shaders ──▶ XenosRecomp ──▶ SPIR-V ──▶ GLSL / DKSH ──────▶ deko3d (Switch GPU)
```

## 1. The CPU side: static recompilation

The game is a PowerPC (Xenon) program. The [ReXGlue](https://github.com/rexglue/rexglue-sdk) code generator turns
every function of `default.xex` into a C++ function `sub_<address>(ctx, base)`: guest registers in `ctx`, guest memory
at `base` (big-endian, byte-swapped on access). `app/sa2_manifest.toml` and `app/overrides.toml` tell it where the
functions are where its own analysis is unsure; `app/perf_overrides.toml` holds the optimised flavour's options, and
`tools/post_codegen.py` rewrites the result further (registers as locals, direct calls, AArch64-friendly vector code).

Each generated function is a **weak** symbol. Defining `REX_HOOK_RAW(sub_X)` anywhere replaces it at link time and
`__imp__sub_X` still calls the original: that is how every hook and native in the port works.

> The "sa2" in many names (`sa2_recomp.*.cpp`, the CMake target) is inherited from the sa2-nx port this tree
> descends from; the project was never renamed internally to keep the scripts working.

## 2. The runtime: `sdk/`

ReXGlue's runtime with Xenia's Xbox 360 kernel emulation (`xboxkrnl`, `xam`), a virtual file system (game folder,
DLC packages, overlays, `UPDATE:`), XMA audio decoding (FFmpeg), input, threads, and the **Horizon layer** for the
Switch (guest memory views, exceptions, cores and priorities, libnx input, audio output). Changes against upstream are
in `patches/rexglue-sdk-v0.10.0.patch`.

## 3. The GPU side: Renut on deko3d

The Xbox 360 GPU is never emulated. **Renut** (`app/src/renut_engine`) hooks about 35 functions of the game's own
XDK Direct3D (create device, draws, shader binds, resolves, swap, fences), keeps the D3D state from the device the
game fills in, and replays each frame through [plume](https://github.com/renderbag/plume) on **deko3d**:

- `gpu/d3d/`: the hooks and the device layout (`me2_hook_map.h` checks each hooked function's instructions at start);
- `gpu/render/`: draws, surfaces, textures (untiling, formats, gamma), resolves, the EDRAM aliases UE3 relies on;
- `gpu/backend/`: frame recording on a worker thread, uploads, presentation, the final FXAA / FSR pass;
- `banjo_renut_deko3d.cpp`: shader packages, DKSH variants (compiled with uam when a specialisation is new).

Shaders are converted ahead of time (Xenos microcode → HLSL → SPIR-V by XenosRecomp, → GLSL by SPIRV-Cross, → DKSH by
uam) because ~38,700 of them are far too many to compile on the console.

## 4. Game-specific work: `app/me2_src`

| File | |
| :--- | :--- |
| `me2_main.cpp`, `me2_hooks.cpp` | start-up, configuration, shared hooks |
| `me2_occlusion.cpp`, `me2_prepass.cpp` | Umbra occlusion queries answered (latent results, reprojection, guards) |
| `me2_cull_distance.cpp`, `me2_light_test.cpp`, `me2_anim_lod.cpp`, `me2_crowd_cpu.cpp` | size culling, light tests, animation / crowd cost |
| `hot/` | **natives**: hand-written C++ for hot guest functions, each run side by side with the original for its first calls and compared (`me2_hot_guard_calls`) |
| `me2_material_*.cpp`, `me2_physx.cpp`, `me2_ini_override.cpp` | material and physics costs, the game's ini settings |
| `me2_button_icons.cpp`, `me2_menu.cpp`, `me2_gyro_menu.cpp`, `me2_subtitles.cpp` | Switch glyphs, menu changes, the new option rows (Scaleform / AVM1 patches) |
| `me2_me1_import.cpp` | the ME1 character import ([me1-import.md](me1-import.md)) |
| `me2_switch_save.cpp`, `me2_disc.cpp`, `me2_startup_movies.cpp`, `me2_cerberus.cpp` | user saves, single-disc play, start-up order, offline Cerberus |
| `me2_frame_diag.cpp`, `me2_sound_diag.cpp`, `me2_write_census.cpp` | diagnostics (off by default) |

## 5. Folders

| Folder | |
| :--- | :--- |
| `app/` | the application: CMake, game hooks, renderer, codegen configuration |
| `sdk/` | the ReXGlue SDK fork (BSD-3-Clause) |
| `shaders/` | XenosRecomp fork and the shader library tools of the native (reference) renderer |
| `tools/` | codegen, post-codegen passes, shader conversion, analysis; `nx/` = Switch build, `win/` = Windows, `tu2/` = address port, `deko/` = shader collection and D3D analysis |
| `config/profiles/` | the tuned `me2.toml` files |
| `forwarder/` | the NSP forwarder's packer |
| `patches/` | every third-party change as a diff against its upstream |
| `docs/` | these guides |
