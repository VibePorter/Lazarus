# Mass Effect 2 — XDK Direct3D map (base XEX 454108CE, no TU)

Found in the dumped guest image (`me2recomp/logs/image.bin`) with `me2recomp/tools/find_d3d.py`,
`me2recomp/tools/find_pm4.py` and `me2recomp/tools/disasm.py`, compared with Silent Hill: Downpour
(`silent-hill-downpour-switch/docs/D3D_SHD.md`) and Mass Effect 1 NX (`masseffect-nx/docs/native-renderer.md`).
Same XDK layout as both; the device offsets of the shader slots are shifted by +288 against them.

Direct3D lives at `0x82A7E000-0x82AA3000`.

## Device

| What | Value | Downpour |
|---|---|---|
| Global device pointer | `0x833C072C` (`lis r,0x833C; lwz r3,0x72C(r)`, 166 loads in 126 functions) | `0x8378F690` |
| Bound pixel shader | `dev+12700` (0x319C) | `dev+12412` |
| Bound vertex shader | `dev+12704` (0x31A0) | `dev+12416` |
| Command buffer write pointer / limit | `dev+48` / `dev+56` | `dev+48` |
| Dirty flags | `dev+16` (64-bit, `oris`/`std` in the setters) | `dev+16` |
| Runtime device (Eden, PC) | `0x4004C400` (also the `VdSetGraphicsInterruptCallback` context) | |

## Shader objects (same layout as Downpour and Mass Effect 1)

| | Vertex | Pixel |
|---|---|---|
| Type word (word 0, low byte) | 6 | 7 |
| Microcode pointer | `+32` | `+24` |
| Container copy (`0x102A11xx` header) | `+872` (0x368) | `+40` (0x28) |
| Shader header | container + `[container+24]` (the publisher reads `[VS+0x380] + VS + 0x368`) | `[PS+0x40] + PS + 0x28` |

## Functions

| Address | What |
|---|---|
| `sub_82A82F98` | SetPixelShader (device r3, object r4): `stw r29, 12700(r30)` |
| `sub_82A83340` | SetVertexShader (device r3, object r4): `stw r29, 12704(r30)` |
| `sub_827D5CA0` | UE3 RHI bound shader state: stores the VS into `dev+12704` itself, calls SetPixelShader |
| `sub_82A8E498` | Draw (294 insns, 12 call sites) |
| `sub_82A8E978` | DrawIndexed (326 insns) |
| `sub_82A8EE90` | Draw (252 insns) |
| `sub_82A8F280` | Draw (276 insns) |
| `sub_82A972C8` | state publisher called by all four draws: IM_LOAD of the bound VS/PS, constants (`sub_82A97048`) |
| `sub_82A90380` | ring kick: writes INDIRECT_BUFFER (`0xC0013F00`) and `CP_RB_WPTR` |
| `sub_82A90F18` | waits for the pending-flip count `dev+11012` |
| `sub_82A90E80` | VBlank interrupt callback; flip handler `sub_82A948F8` |
| `sub_82A8D590`, `sub_82A8D610` | sampler state setters (fetch-constant table at `dev+12552`) — not shaders |
| `sub_82A8C960`..`sub_82A8CB28` | sampler setters at `dev+12036..12048` — not shaders |

UE3 RHI callers of the draws: `sub_827CD0D8`, `sub_827CD3E0`, `sub_827CD4E8`, `sub_827CD680`, `sub_827CD030`,
`sub_827CE3B8`, `sub_827EA770`, `sub_827EC9A0`, `sub_827ED088`.

## Shader library input

UE3 packages use standard chunk headers (tag `0x9E2A83C1`, block size `0x20000`; Mass Effect 1 repeats the tag)
and LZO1X like Mass Effect 1. `tools/nx/shaders/ue3_shader_scan.cpp` (patched for that header) over all packages
of the game and the 23 DLC: 0 bad blocks, **38,542 distinct containers (750 vertex)** in
`/root/nx/shaders/containers` (WSL).
