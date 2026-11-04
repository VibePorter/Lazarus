# ME2 Direct3D map for Renut (mgr-nx-deko renderer)

ME2 XDK 2.0.8955 (xex import libraries). The device layout is Banjo-Kazooie N&B's (XDK 7645), the layout Renut was
written for (d3d.h lists N&B's values in brackets), with differences in the bound-object area. MGR (21173) and SA2
(21076) are shifted against it. Every entry below is read from ME2's own code
(`tools/deko/scripts/d3d_find.py --dump <address>`).

## Device

| Field | ME2 | N&B | MGR/SA2 | Evidence |
|---|---|---|---|---|
| Allocation size | 24064 (0x5E00) | 0x5E00 | 0x6080 | Direct3D_CreateDevice `li r5,24064` |
| Creation thread id | 10888 | 10888 | 11016 | CreateDevice `stw r3,10888(r31)` |
| Is title process | 10892 | 10892 | 11020 | CreateDevice |
| Fence completion pointer | 10896 | 10896 | 11024 | BlockOnFence, IsFencePending |
| Fence tag | 10908 | 10908 | 11036 | BlockOnFence, IsFencePending |
| Flags (no-hardware 0x80) | 10940 | 10940 | 11068 | KickOff `lbz r11,10940` + `rlwinm. 0,0,24` |
| Flags2 (0x20 out of memory, 0x02 immediate fences) | 10941 | 10941 | 11069 | NewSegment, KickOff, CBlocker_Check |
| In KickOff | 13232 | 13232 | 13600 | KickOff, BlockOnFence |
| Command buffer being recorded | 13516 | 13516 | 13920 | NewSegment, KickOffSegment |
| Side buffer (4800 bytes) | 16728 | 16728 | 17136 | CreateDevice, ring reset |
| Present fence tag | 14940 | 14940 | 15344 | NewSegment |
| Behavior flags | 23980 | 23980 | 24392 | NewSegment |
| Index buffer | 12444 | 12436 | 12812 | DrawIndexedVertices |
| Stream strides (u8[16], /4) | 12536 | 12528 | 12904 | BeginVertices `stb r10,12536` |
| Pixel shader | 12700 | 12692 | 13068 | SetPixelShader |
| Vertex shader | 12704 | 12696 | 13072 | SetVertexShader, draws |
| Pending-flip count | 11012 | | | (earlier note: VBlank wait) |
| VdGlobalDevice import slot | 0x82000800 (0x82000804 if process type 2) | 0x820007FC | 0x820005F8 | IsFencePending |

### Translating an N&B offset (Renut's bracketed values, the hex offsets in device.cpp / d3d_native.h) to ME2

| N&B offset | ME2 | Checked with |
|---|---|---|
| below 0x2B00 (public device) and 0x2A88-0x2ABF | same | fence fields, flags, TilePredication 10932, RunPredication 10936 |
| 0x2E2C (vertex declaration) | +4 (11824) | InitializeApiState `stw r29,11824` = dev+12200 |
| 0x2E30 qword (XenonGpu_Initialize 0x00000000FFFFFFFF) | +8 (11832) | ring code `ld r10,11832` vs -1 >> 32 |
| 0x2FA0 - 0x3390 (default declaration, bound objects, shaders, predication, tiling, scissor) | +8 | InitializeApiState: 12200/12204/12256/12264/12440..; BeginTiling: TileCount 12756, rects 12760, origins 13000, extent 13188; ClearF RT 12448 / DS 12464; scissor 12684; tiling surfaces 12736; Predication 12716, RunPredicationState 12720, PredicationSet 12724 |
| 0x3390 and up (BeginTiling's clear color 13200, Z 13216, stencil 13220; InKickOff 13232, command buffer 13516, present interval 13596, auto surfaces 14824/28/32, side buffer 16728, behavior flags 23980) | same | BeginTiling, KickOff, NewSegment, InitializePresentationParameters, InitializeApiState |

The 8 bytes come back between N&B 0x3388 and 0x3390: BeginTiling stores the tiling extent at 13188/13192 (N&B
0x337C+8) and its flags at 13196 (N&B 0x3384+8), then the clear color at 13200 (N&B 0x3390, no shift). N&B's
0x3388-0x338F has no ME2 counterpart (me2-nx-deko/tools/me2_layout_port.py reports any use).

Public part (< 0x2B00): as Renut's (`m_Constants` at 0x480: the draws flush VS constants from +1920 = 0x780 and PS
constants from +6016 = 0x1780).

## Functions

| Renut hook (MGR address) | ME2 | Notes |
|---|---|---|
| Direct3D_CreateDevice (826B2418) | 82A7FAD8 | |
| KickOff (826B5688) | 82A90A68 | returns m_pRing |
| KickOffSegment (826B5348) | 82A90810 | |
| NewSegment (826B45A8) | 82A90020 | (dev, dwords) |
| ring reset to the side buffer | 82A8FB20 | called by NewSegment when out of memory |
| BlockOnFence (826B42A8) | 82A90B68 | (dev, fence, type, no_kick) |
| CBlocker_Check (826B1C88) | 82A7F888 | |
| IsFencePending (826B57C8) | 82A90F98 | (fence) |
| BeginVertices (826BC468) | 82A8E498 | (dev, prim, count, stride) |
| BeginIndexedVertices | 82A8E978 | MGR has none |
| DrawVertices (826BC928) | 82A8EE90 | (dev, prim, start, count) |
| DrawIndexedVertices (826BCD40) | 82A8F280 | (dev, prim, base, start, count) |
| XGRegisterPixelShader (826AC3D8) | 82A82E70 | |
| XGRegisterVertexShader (826ACD00) | 82A83158 or 82A83250 | `li r5,872` |
| SetPixelShader | 82A82F98 | |
| SetVertexShader | 82A83340 | |
| ring submit (INDIRECT_BUFFER into the primary ring) | 82A90998 | |
| ring kick (CP_RB_WPTR) | 82A90380 | |
| state publisher of the draws | 82A972C8 | |

| D3DDevice_Swap (826B3510) | 82A950F8 | stores swap count 16560, swap fence 10928 (N&B) |
| SynchronizeToPresentationInterval (826B3050) | 82A94BC0 | (dev, immediate); sets 10941 |= 0x10; inserts SwapCallback 82A94A10 |
| BeginRingBig (826B58C0) | 82A8F8A8 | (dev, dwords) |
| RingBufferDeviceAllocate (826B4078) | 82A8FC50 | (dev, flags, *size, alignment) |
| AllocateInlineData (826B44C0) | 82A8FE80 | (dev, dwords, alignment) |
| SetRingBufferParameters (826B5A08) | 82A91028 | called by the hardware init 82A988E0 |
| InitializePresentationParameters (826C0C48) | 82A98488 | auto depth/front/back 14824/14828/14832 |
| InitializeApiState (826C0278) | 82A981A8 | N&B's native copy in device.cpp, field for field (+8 rule) |
| D3D__InitializeHardwareDevice | 82A988E0 | KeGetCurrentProcessType, SetRingBufferParameters, KickOff |
| ClearF (826BC260) | 82A929F0 | core 82A92350 |
| Resolve (826BB0D0) | 82A930B8 | the whole resolve (RB_COPY_CONTROL 0x2318); f1 = ClearZ |
| BeginTiling | 82A94258 | (dev, flags, count, rects, color, Z f1, stencil) |
| ConfigureForOnePassZPass | 82A93FC8 | called by BeginTiling |
| EndTiling (?) | 82A9BD30 or 82A945B0 | 9BD30 calls Resolve, ClearF's targets, BeginRingBig; 945B0 inserts a callback |
| CreateVertexDeclaration (826AC938) | 82A83510 | |
| D3DResource_Destroy (826AAF80) | 82A7E660 | |
| resource lock | 82A7E880 | |
| WindowScissor (826B5FC8) | 82A8B168 | (dev, x1, y1, x2, y2) |
| CBlocker / InsertCallback core: ring callback | 82A90C60 | (dev, ring, type, callback, context); no public InsertCallback in ME2 |
| asynchronous swap thread | 82A9A628 | waits, then SynchronizeToPresentationInterval + Swap with dev+14844: ME2 swaps on a D3D thread, MGR does not |
| Present-like (image + Sync + Swap) | 82A8F6D0 | movies? |

To do: XenonGpu_Initialize, EndTiling for sure, BeginCommandBuffer/EndCommandBuffer/RunCommandBuffer (does ME2 record
any?), the swap progress the engine polls (MGR's EngineSwapProgress), D3DDevice_Release, InsertFence/SetFence.
