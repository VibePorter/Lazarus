# 🗄️ The first attempt: NVK (kept for reference)

Before deko3d, the port followed the route of the other Xbox 360 recompilation ports on the Switch: the SDK's Xenos
**GPU emulation** (or a native D3D-level renderer) on **Vulkan**, through Mesa's **NVK** driver built for Horizon.
The scripts are kept unchanged in [`tools/nx/legacy-nvk`](../tools/nx/legacy-nvk) as a record. They point at the
original machine's folders and are **not** part of the build.

## What it was

- Base: the [Silent Hill: Downpour Switch](https://github.com/fungalspores/silent-hill-downpour-switch) port's app
  layer and
  [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx)'s SDK (patched ReXGlue at `96b6e09`),
  [mesa-switch](https://github.com/danfromtico/mesa-switch) for NVK (Rust NAK compiler).
- `setup_wsl.sh` (WSL toolchain, masseffect-nx + patches, mesa-switch), `build_mesa.sh` (Mesa for Horizon),
  `build_rexglue.sh` / `make_me2nx.sh` / `build_nro.sh` (the app), `sdk_local_fixes.*` (fixes from the PC build),
  `eden_*.ps1` / `gdb_*.sh` / `ryu_run.ps1` (emulator runs and GDB stacks), `D3D_ME2.md` (the first Direct3D map).

## How far it got

On 24 October it ran on a real Switch: the main menu for 7 minutes stable with the SDK's Xenos GPU emulation (all DLC
mounted), then in game at **13-15 fps** with a native renderer borrowed from Silent Hill: Downpour's port.

## Why it was left

- **CPU cost:** drawing through Vulkan cost ~40 ms of CPU a frame in the renderer alone, on a game that is CPU-bound
  already.
- **Eden:** the first NRO hung right after the intro movie: the render thread waited for a flip that never came
  (the GPU ring's write pointer did not reach the command processor), and GDB breakpoints in host code hung the
  emulator, so it could not be traced further.
- **Ryujinx:** NVK failed to bind its virtual address range (`ErrorOutOfDeviceMemory`), so there was no emulator to
  test in.

The sa2-nx / mgr-nx-deko line had shown a D3D-level renderer on **deko3d** with offline shaders (no Vulkan driver in
the NRO, no GPU emulation). On 2026-10-25 the port switched to it; the first deko3d build showed the title screen in
Ryujinx the same evening and ran on a real Switch that night.
