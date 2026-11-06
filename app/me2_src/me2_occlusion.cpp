// Mass Effect 2 - real occlusion queries for UE3's occlusion culling on Renut's renderer.
//
// WHAT THE ENGINE DOES (ME2, XDK 2.0.8955, from the disassembly, 2026-10-27)
//   IDirect3DQuery9::Issue = sub_82A90458(query, flags): flags bit 1 = D3DISSUE_BEGIN, bit 0 = D3DISSUE_END. Query:
//   +0 device, +4 type (9 = occlusion, 10 = screen extent, 8 = event), +24 fence. For an occlusion query the GPU writes
//   ZPASS sample counts per tile (+148 tiles, report blocks at +28...) that GetData = sub_82A906E0(query, pData, size,
//   flags) sums once the fence has passed. UE3's RHIGetOcclusionQueryResult (sub_827DDCA8(query, &pixels, bWait))
//   polls GetData with Sleep(0) up to 100000 times while it answers S_FALSE; every caller in the scene renderer passes
//   bWait = TRUE (sub_82695EE0, sub_82696410, sub_82696780, sub_826A7690; sub_827486B8 decides), then divides the count
//   by the samples a pixel.
//
// WHY
//   Renut's renderer drew the query boxes but never wrote the counts, and UE3 polled forever (3.6 fps), so GetData
//   answered every occlusion query "visible" (me2_hooks.cpp, 2026-10-25): occlusion culling was off and UE3 drew every
//   primitive in the frustum. The console's limit is the UE3 render thread, ~35 us a draw at 1020 MHz, so each culled
//   draw counts (2026-10-26 console profile: 1500-7000 draws a frame in the heavy scenes).
//
// HOW
//   Issue (game side) queues BEGIN / END to Renut's render thread in draw order. While a query is open each guest draw
//   the render thread records gets a host occlusion query of its own around it (plume beginOcclusionQuery /
//   endOcclusionQuery: Vulkan VK_QUERY_TYPE_OCCLUSION on the PC, deko3d SamplesPassed counter reports on the Switch),
//   taken from one ring of kPoolSize; the guest query is the sum of its draws. GetData answers from the host results
//   when the GPU has written them (waiting up to me2_occlusion_wait_us); otherwise, or for a query with no host draw,
//   "visible" as before, so the picture can only lose culling, never objects.
//
// me2_occlusion_queries (default on): the above. Off: every occlusion query "visible" (the 2026-10-25 behaviour).

#include <algorithm>
#include "me2_ab_cycle.h"  // me2-nx-deko (2026-11-04): the draw-count rotation
#include <cstdio>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <fmt/format.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/surfaces.h"  // ResolutionScale (me2_occlusion_scale 0)
#include "renut_engine/gpu/render/textures.h"
#include "me2_occlusion_bounds.h"

// me2_cull_distance.cpp (2026-11-02): command 50's size-based "skip_instance"; true = answered.
extern "C" bool me2_umbra_size_cull(PPCContext& ctx, uint8_t* base);
extern "C" void me2_lod_distance_scale_view(PPCContext& ctx, uint8_t* base);  // me2_cull_distance.cpp
extern "C" void me2_lod_distance_scale_umbra(PPCContext& ctx, uint8_t* base);   // me2_cull_distance.cpp
// me2_occlusion_raster.cpp (2026-11-05): the occlusion rasterizer (me2_occlusion_cpu_source 1).
extern "C" void Me2RasterFrame(const uint8_t* base, uint32_t scene, const float* vp, const float* cam, uint64_t frame,
                               uint32_t renderer);
extern "C" uint8_t Me2RasterTestBox(const float* mn, const float* mx, uint64_t frame);
extern "C" bool Me2RasterBoxPass(uint64_t frame, const uint8_t** boxes, const uint32_t** wraps, const uint8_t** codes,
                                 uint32_t* count, uint64_t* worker_ns);
extern "C" uint8_t Me2RasterTest(const uint8_t* base, uint32_t verts_va, uint32_t count, uint32_t idx_va,
                                 uint32_t ntris, uint64_t frame, uint8_t* alt);
extern "C" void Me2RasterReport();
extern "C" uint32_t Me2RasterTakeToken();
extern "C" void Me2RasterTraceFalse(uint32_t token, uint32_t count, uint64_t now);
#include "me2_occlusion_view.h"

#if defined(PLUME_DEKO3D)
namespace plume {
bool Deko3DOcclusionSnapshot(const RenderQueryPool*, uint32_t, uint64_t (&)[4]);
}
#endif

REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_wall_trace, false, "ME2",
                    "Temporary bounded Citadel wall query-chain diagnostic: at most 2000 lines / 900 frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(me2_occlusion_queries, true, "ME2",
                    "Real occlusion queries for UE3's occlusion culling (host queries around the query's draws); off = "
                    "every occlusion query answers visible")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_reader_raster, false, "ME2",
                    "A per-primitive occlusion read (sub_82696410) whose GPU result is not ready is answered from the "
                    "CPU depth buffer instead of waiting for the GPU")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_wait_us, 30000, "ME2",
                     "How long GetData waits for an occlusion query's GPU result before it answers visible (us)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-31 (console logs of build 10-30 00:07): in GPU-bound windows (GPU 32-42 ms a frame at 480p) the readers of
// age-1 queries waited up to 6 s in 10 s for the previous frame's GPU work, so the UE3 render thread and the GPU took
// turns instead of overlapping. A timed-out wait answers 'visible' (an extra draw, never a missing object). This A/B
// steps the wait through a list on a timer in ONE run: "30000,2000,0" with 60 s = each value 60 s in turn, logged at
// every step ("[occlusion] wait setting now ...") and in each 10 s report.
REXCVAR_DEFINE_STRING(me2_occlusion_wait_cycle, "", "ME2",
                      "Diagnostic A/B: comma-separated GetData wait values (us) used in turn, each for "
                      "me2_occlusion_wait_cycle_s seconds; empty = me2_occlusion_wait_us always")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_wait_cycle_s, 60, "ME2", "Seconds each value of me2_occlusion_wait_cycle is used")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_previous, false, "ME2",
                    "A query of the current frame (not on the GPU yet) is answered with the previous issue of the same "
                    "query when that one has its GPU result (one frame late) instead of 'visible'. OFF: on the PC "
                    "(2026-10-27) objects / textures disappeared at random with it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-06): console 18:43 (moving through BioD_N7Spdr2): UE3's render thread waited 100-1,100 ms every
// 10 s (8.9 s in a 9-minute session; up to ~4 ms a frame) in Answer for GPU results of last frame's queries that were
// not done yet, spinning (sched_yield, occlusionResults: ~2 % of all its samples). With this, such a read is answered
// 'visible' at once when the previous issue of the same query has its result and found the object visible: the
// answer the GPU will most likely give, and 'visible' can only cost a draw, never hide anything (unlike
// me2_occlusion_previous, which also reused hidden answers). A query whose previous answer was 'hidden' still waits.
REXCVAR_DEFINE_BOOL(me2_occlusion_nowait_visible, false, "ME2",
                    "A query read that would wait for its GPU result answers 'visible' at once when the previous issue "
                    "of the same query found the object visible (no stall; at most a draw more). Off: waits as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-06): the same for the shadow readers' hidden answers. sub_82696780 / sub_826A7690 read the per-object
// shadow occlusion queries (FSceneViewState's shadow query map, keyed by caster and light): a late result there whose
// previous issue found the shadow volume hidden answers hidden at once. Worst case a shadow that just came into view
// appears a frame later; objects are never affected (their readers are not these).
REXCVAR_DEFINE_BOOL(me2_occlusion_nowait_shadows, false, "ME2",
                    "A late shadow occlusion read (sub_82696780 / sub_826A7690) answers 'hidden' at once when the "
                    "previous issue found the shadow hidden (a shadow coming into view can show a frame late). Off: waits")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_async, true, "ME2",
                    "deko3d only: Umbra's non-blocking result reads (sub_827486B8 opcode 130) of a query not on the GPU "
                    "yet force the render thread's open command list to the GPU (Video::KickLocked(true)) and answer "
                    "'not ready' (S_FALSE) until its real count arrives (Umbra polls again in the frame), 'visible' "
                    "after me2_occlusion_umbra_wait_us. Without the forced submit (PC / Vulkan) Umbra polled 31 M "
                    "times in 10 s for a result that only comes after the Swap, so it is off there")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-27 (console stacks of 22:2x, Omega, umbra_async off): Umbra still issued ~118 queries a frame (16k / 10 s),
// each a BEGIN / test-geometry draw / END on the UE3 render thread (the bottleneck) plus a host query and a kick per
// read, while every read was answered 'visible' without waiting. With the async reads off these commands are skipped:
// opcode 128 (Issue BEGIN), 129 (Issue END, also counts the kick opcode 130 does) and 131 (the test geometry:
// BeginIndexedVertices sub_827DC2F0, two memcpys, the inline end). Opcode 130 still runs: GetData of a query that was
// never issued answers 'visible' (Answer: unknown), the same answer Umbra got before.
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_skip, true, "ME2",
                    "With me2_occlusion_umbra_async off (Umbra answered 'visible' anyway): skip Umbra's query issues "
                    "and test-geometry draws (sub_827486B8 opcodes 128, 129, 131)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-27 (latent Umbra answers): Umbra reads its queries in the frame it issues them, which the GPU cannot answer
// without a stall (forced submit + up to 5 ms of polling a query: the UE3 render thread asleep 25-45 % of its time).
// Here the reads (opcode 130) never wait: each issue is keyed by its node's test geometry (opcode 131 copies the
// node's vertices / indices from Umbra's buffers 0x8339AB68 / 0x8339A5C8; a hash of them), its GPU count is collected
// when it is ready (a later frame), and a read answers the newest count of the same key from the last
// me2_occlusion_umbra_latent_frames frames, else 'visible'. A node that comes into view behind an occluder that moved
// away is drawn 1-2 frames late. No forced submits (opcode 129's kick counter is cleared before 130 runs).
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_latent, false, "ME2",
                    "Umbra's occlusion reads answered from the previous frames' GPU counts of the same node (keyed by "
                    "its test geometry), never waited for; overrides me2_occlusion_umbra_async / _skip")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Ryujinx 23:27: the test geometry's hash repeats frame to frame for only ~2 % of the keys (view-relative vertices),
// so the node is identified by Umbra's command object instead: [self+4] +0 (and +4), read by Umbra getters
// sub_82DDE180 / sub_82DDE238 (dPVS Commander instance / viewer). The report measures all three keys.
// PC dump 23:45: the test geometry is a world-space box (8 vertices, 12 triangles) that changes a little every frame
// (Umbra's conservative expansion), the command words carry no node (+0 always 0, one command object for all). Key 3:
// the box's centre, quantized to 8 units (a symmetric expansion keeps it).
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_latent_key, 3, "ME2",
                     "me2_occlusion_umbra_latent: the node key: 0 = hash of the test geometry, 1 = Umbra command +0, "
                     "2 = command +0 and +4, 3 = centre of the test box (8 units)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_latent_dump, false, "ME2",
                    "Diagnostic: every 10 s log 30 of Umbra's test geometries (opcode 131): command and record words, "
                    "the first vertices")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// PC 2026-10-28 (prepass on): Umbra re-tests a node only every few frames, so its newest count is often 6-30 frames
// old; with 30 frames the culling was 75-85 % of the reads but objects flickered on fast camera turns, with 6 the flicker
// was gone and so was most of the culling. An 'occluded' count is therefore used only while the camera is still close
// to where it was at that issue (the view matrix UE3 hands Umbra, sub_8274AA38 view +64, kept per frame).
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_latent_frames, 30, "ME2",
                     "me2_occlusion_umbra_latent: how many frames old a node's GPU count may be to answer a read")
    .range(1, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Only negative answers need confirmation. Holding back positive answers kept
// newly visible objects hidden; accepting one stale zero made the Citadel wall
// blink. Legacy confirm=1 still uses the conservative minimum of two fresh
// zero-count frames; larger values can require more negative evidence.
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_latent_confirm, 2, "ME2",
                     "me2_occlusion_umbra_latent: fresh zero-count frames required to start hiding a node "
                     "(at least 2, including legacy value 1); any positive count restores visibility immediately")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-28 (me2_occlusion_umbra_defer): Umbra tests its boxes against the depth there is at the time, which only the
// game's depth prepass fills (without it 2-3 % occluded, with it 70-85 %; the prepass was ~40 % of all draws in the heavy
// Omega windows). The latent answers are read a frame or more later anyway, so the boxes need not be drawn when Umbra
// issues them: with this on, the render thread holds each Umbra query (BEGIN, its box draws, END) back and draws them
// just before the frame's resolve of the same depth target (after the base pass: the whole scene's depth, not only the
// prepass occluders), or drops them at the present when no such resolve came (Umbra then gets 'visible'). Meant with
// me2_guest_skip_prepass = true.
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_defer, false, "ME2",
                    "me2_occlusion_umbra_latent: Umbra's query boxes are drawn at the frame's resolve of their depth "
                    "target (after the base pass) instead of when Umbra issues them; lets the prepass stay off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_defer_resolve, 1, "ME2",
                     "me2_occlusion_umbra_defer: the boxes are drawn before the N-th resolve of their depth target in the "
                     "frame (UE3 may resolve the scene depth after the prepass and again after the base pass)")
    .range(1, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// PC trace 01:16 (prepass off, ~1375 draws a frame): the boxes' depth target (400D7A00) is resolved once, at draw ~106,
// before the base pass (empty depth: 5-10 % occluded); the base pass's depth is another surface object (400D7980,
// resolved at draw ~660), then 400D74C0 x4 near the end. With retarget the boxes wait for the first resolve of another
// depth target after their own one and are drawn into that one, without colour targets.
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_defer_retarget, true, "ME2",
                    "me2_occlusion_umbra_defer: draw the boxes into the next depth target resolved after their own "
                    "(the base pass's) instead of at their own target's resolve")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_defer_trace, false, "ME2",
                    "me2_occlusion_umbra_defer diagnostic: log every depth resolve of one frame every 300 (30 frames; address, whether "
                    "it is the boxes' target, draws since the present, boxes held back)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_umbra_latent_rot, 0.03, "ME2",
                      "me2_occlusion_umbra_latent: an 'occluded' count is used only if no element of the view "
                      "rotation changed by more than this since its issue (0.03 ~ 2 degrees)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-04, console: Kasumi's party 23 fps looking at a wall, 5 fps turning the stick).
REXCVAR_DEFINE_DOUBLE(me2_occlusion_cut_rot, 1.0, "ME2",
                      "Camera cut detection: a view rotation element changing by more than this in ONE frame is a cut "
                      "(every occlusion answer thrown away); 1.0 ~ 60 degrees a frame (was a fixed 0.2 ~ 12 degrees)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_latent_turn, true, "ME2",
                    "me2_occlusion_umbra_latent: an 'occluded' count stays valid while the camera only turned (moved "
                    "at most me2_occlusion_umbra_latent_move) if the node's box was wholly on screen at the count: "
                    "occlusion depends on where the camera is, not where it looks")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-04, PC at 15 fps, Kasumi's party: spinning the stick made ~100-170 node reads a frame "too old ->
// visible": what comes back into view was counted a whole turn ago, beyond me2_occlusion_umbra_latent_frames).
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_latent_turn_frames, 150, "ME2",
                     "me2_occlusion_umbra_latent_turn: an 'occluded' count older than latent_frames stays valid up to "
                     "this many frames when the camera has not moved, the box was wholly on screen at the count and "
                     "has been off screen since (the camera turned away and back); 0 = off. A box that stayed on screen "
                     "(a door opening in front of a still camera) keeps the latent_frames limit")
    .range(0, 250)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_umbra_latent_move, 64.0, "ME2",
                      "me2_occlusion_umbra_latent: ... and the camera moved less than this (world units) since then")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-28 (user, console): standing behind cover, a door's interface prompt stayed hidden, showed for about a second,
// hid again, round and round. Umbra re-tests a node it was told is occluded only now and then; when it did, the node's
// last count was older than latent_frames (10), so the read answered 'visible' (no fresh count yet) and the prompt was
// drawn until the next counts said occluded again. An 'occluded' count older than latent_frames now still answers while
// the camera has stayed put since it was taken (an occluder that moves away with the camera still, e.g. a door, is
// then seen up to this many frames late).
// 2026-10-28 19:4x (PC flip trace, user: still blinks after walking a little or turning the camera): almost every
// occluded -> visible change was "count too old" with the count exactly as old as Umbra's gap between reads of the node
// (10-110 frames: Umbra re-reads a node it was told is occluded that rarely), i.e. the count of the node's previous test,
// the newest there can be. The camera limits of 1 degree / 16 units broke at the smallest step; then the node was
// answered visible and Umbra did not read it again for 20-60 frames (the prompt shown ~1 s over 0-pixel counts).
// Defaults now 240 frames (the view history), ~9 degrees, 200 units. Cost: an object an older count calls occluded but
// that the camera has since brought into view shows up at Umbra's next read of it (its own retest gap) instead of at once.
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_latent_still_frames, 240, "ME2",
                     "me2_occlusion_umbra_latent: an 'occluded' count older than latent_frames still answers up to this "
                     "many frames while the camera stays within latent_still_rot / _move of where it was; "
                     "<= latent_frames = off")
    .range(0, 250)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_umbra_latent_still_rot, 0.15, "ME2",
                      "me2_occlusion_umbra_latent_still_frames: largest change of a view rotation element "
                      "(0.02 ~ 1 degree, 0.15 ~ 9 degrees)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_umbra_latent_still_move, 200.0, "ME2",
                      "me2_occlusion_umbra_latent_still_frames: largest camera movement (world units)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_flip_trace, false, "ME2",
                    "Diagnostic: log every change of an Umbra node's answer (occluded <-> visible) with its reason "
                    "([umbra-flip], at most 120 lines per 300 frames)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_umbra_latent_key_stats, false, "ME2",
                    "Diagnostic: compute all four node keys of every Umbra test and report how stable each kind is")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_wait_us, 5000, "ME2",
                    "How long Umbra's polls of one query issue get 'not ready' before it is answered 'visible' (us)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-28 (console stacks, Omega, prepass + latent): each Umbra test (~400 a frame) cost the UE3 render thread three
// render-queue items (BEGIN, test box, END) through the queue's lock shared with Renut's thread, two hash maps under a
// mutex and a map node malloc / free for the box. me2_occlusion_carry: BEGIN / END ride on the next item instead
// (render_thread::CarryBegin / CarryEnd; same order on the render thread).
REXCVAR_DEFINE_BOOL(me2_occlusion_carry, true, "ME2",
                    "Occlusion query BEGIN / END are carried by the next render-queue item instead of items of their own")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2_occlusion_umbra_retest_frames (needs me2_occlusion_umbra_latent): Umbra tests every node it reaches every frame,
// and the answer it reads is anyway an earlier frame's count. With N > 1 a node with a count is tested once every N
// frames (the frame by its key, so the tests spread evenly); in between its test box is not drawn and its query is not
// issued on the host (opcode 131 skipped), and the read answers the last count. Cost: a node that comes into view is
// drawn up to N - 1 frames later.
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_retest_frames, 1, "ME2",
                     "me2_occlusion_umbra_latent: a node with a count is tested once every this many frames "
                     "(1 = every frame)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-03, Kasumi party console stacks: ~880 Umbra tests a frame, 80 % of them the same nodes as the frame
// before and nearly all answered occluded; each test is a box draw through UE3's callback, Renut's capture and the
// latent bookkeeping, ~10 % of the UE3 render thread). me2_occlusion_umbra_retest_frames skips re-tests of VISIBLE nodes
// only (252 of ~145,000 tests there). This is the same for OCCLUDED nodes: one whose last test is less than N frames old,
// whose count is fresh and whose camera has not moved since (within ~3 degrees and 50 units) is not tested; its
// read answers that count (occluded). Cost: what a door or a turn uncovers shows up to N - 1 frames later (N = 2: one
// frame, 33 ms). 1 = off.
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_retest_occluded, 1, "ME2",
                     "me2_occlusion_umbra_latent: a node last counted OCCLUDED is tested once every this many frames "
                     "while the camera stays still (1 = every frame)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_retest_occluded_ab_s, 0, "ME2",
                     "A/B: me2_occlusion_umbra_retest_occluded on and off in turn every this many seconds; 0 = always "
                     "as set")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-31: the guest's sample count is the host's times guest pixels / host pixels. The fixed 4 was right for the 360p
// scene only (1280x720 / 640x360); at 480p it is 2.25, at 720p 1. UE3 scales a lens flare by its source's count, so a
// partly hidden flare came out too bright at 480p (me2_renut_lens_flares). 0 = from the scene's resolution scale.
// 2026-11-01 (user: in cutscenes, at a camera change, shadows come in late): UE3 draws an object or a projected shadow
// only if the occlusion query it issued for it in an earlier frame came back with pixels. After a camera cut those
// counts were measured from the previous camera; the 360 has the new view's counts a frame later, the port one or two
// frames later (results are read back with a delay), so shadows and objects hidden from the old camera pop in late.
// For this many frames after a cut (the view observer below: the camera turned, moved or changed its projection at
// once) the reads of UE3's own queries (reader tags 1-4, not Umbra's) that say "no pixels" or "not ready" answer
// visible. Costs a few extra draws in the frames right after a cut. 0 = off.
REXCVAR_DEFINE_INT32(me2_occlusion_cut_visible_frames, 3, "ME2",
                     "After a camera cut, this many frames of UE3's own occlusion reads answer visible (shadows and "
                     "objects hidden from the old camera); 0 = off")
    .range(0, 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_scale, 0, "ME2",
                     "Host samples are multiplied by this for the guest count (Renut renders below the guest's 720p); "
                     "0 = automatic: (1 / the scene's resolution scale) squared")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap
// All GetData waiting since the start, us (me2_frame_diag.cpp reads it per frame for its spike lines).
std::atomic<uint64_t> g_me2_occlusion_wait_total_us{0};
std::atomic<int32_t> g_me2_occlusion_wait_setting_us{-1};  // the GetData wait in use (me2_occlusion_wait_cycle)

namespace {
using namespace renut::gpu;

constexpr uint32_t kPoolSize = 1u << 16;  // host queries, one a draw inside a guest query; reused after a wrap
constexpr uint32_t kRecords = 1u << 14;   // guest query issues kept for GetData
constexpr uint32_t kVisible = 0x100000u;  // the count answered "visible"
// me2_occlusion_cut_visible_frames: the game frame of the last camera cut (0: none yet) and the reads turned visible.
std::atomic<uint64_t> g_cut_frame{0};
std::atomic<uint64_t> g_cut_overrides{0};
int32_t CutVisibleFrames() {
  static const int32_t n = REXCVAR_GET(me2_occlusion_cut_visible_frames);
  return n;
}
constexpr uint32_t kNotReady = 0xFFFFFFFFu;  // Answer: "not ready" for a caller that can take it (Umbra)
std::atomic<uint64_t> st_umbra_skipped{0};  // me2_occlusion_umbra_skip: commands 128 / 129 / 131 not run
std::atomic<uint64_t> st_umbra_not_ready{0}, st_umbra_kicks{0}, st_umbra_timeouts{0}, st_umbra_real[2];
std::atomic<uint64_t> g_umbra_kicked_seq{0};  // g_seq at the last forced submit

}  // namespace
void Me2UmbraLatentReport(int64_t seconds);  // below
namespace {
using namespace renut::gpu;
uint32_t LoadBE(const uint8_t* base, uint32_t va) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va)); }

// One issue (BEGIN..END) of a guest query. Written by the render thread, read by GetData.
struct Record {
  std::atomic<uint64_t> seq{0};
  std::atomic<uint32_t> first{0}, count{0};
  std::atomic<uint32_t> state{0};  // kOpen, kClosed, kBroken
  std::atomic<uint64_t> frame{0};  // g_sa2_game_frames at BEGIN
  std::atomic<int64_t> first_poll_us{0};  // Umbra's first "not ready" poll of this issue (steady clock, us; 0 = none)
};
constexpr uint32_t kOpen = 0, kClosed = 1, kBroken = 2;
Record g_records[kRecords];

std::atomic<int> g_enabled{-1};
std::atomic<uint64_t> g_seq{0};
std::atomic<plume::RenderQueryPool*> g_pool{nullptr};

// Game side: the latest issue of each guest query, and the newest issue made in an EARLIER guest frame than its latest
// one (2026-10-27): the answer for a read in the frame of the latest issue, see Answer (a query may be issued several
// times a frame).
// 2026-10-28: a table of slots by query address (open addressing, never removed: the game's query objects are pooled)
// read and written without a lock; the maps under g_map_mutex only when the table is full.
struct QuerySlot {
  std::atomic<uint32_t> query{0};
  std::atomic<uint64_t> latest{0}, previous{0};
};
constexpr uint32_t kQuerySlots = 1u << 14;
constexpr uint32_t kQueryProbes = 64;
QuerySlot g_query_slots[kQuerySlots];
std::mutex g_map_mutex;
std::unordered_map<uint32_t, uint64_t> g_latest;
std::unordered_map<uint32_t, uint64_t> g_previous;
std::atomic<bool> g_map_used{false};

QuerySlot* FindQuerySlot(uint32_t query, bool insert) {
  if (!query)
    return nullptr;
  const uint32_t h = ((query >> 2) * 2654435761u) >> (32 - 14);
  for (uint32_t i = 0; i < kQueryProbes; ++i) {
    QuerySlot& s = g_query_slots[(h + i) & (kQuerySlots - 1)];
    uint32_t q = s.query.load(std::memory_order_acquire);
    if (q == query)
      return &s;
    if (q == 0) {
      if (!insert)
        return nullptr;
      if (s.query.compare_exchange_strong(q, query, std::memory_order_acq_rel) || q == query)
        return &s;
    }
  }
  return nullptr;
}

// The latest and the previous-frame issue of 'query' (0 = none).
void QueryIssues(uint32_t query, uint64_t& latest, uint64_t& previous) {
  latest = previous = 0;
  if (const QuerySlot* s = FindQuerySlot(query, false)) {
    previous = s->previous.load(std::memory_order_relaxed);
    latest = s->latest.load(std::memory_order_acquire);
    return;
  }
  if (!g_map_used.load(std::memory_order_acquire))
    return;
  std::lock_guard<std::mutex> lock(g_map_mutex);
  auto it = g_latest.find(query);
  if (it != g_latest.end())
    latest = it->second;
  auto ip = g_previous.find(query);
  if (ip != g_previous.end())
    previous = ip->second;
}
uint64_t LatestIssue(uint32_t query) {
  uint64_t latest, previous;
  QueryIssues(query, latest, previous);
  return latest;
}

// Render thread only.
struct RenderSide {
  std::unique_ptr<plume::RenderQueryPool> pool;
  bool pool_failed = false;
  uint32_t next = 0;
  uint64_t open_seq = 0;  // 0 = none
  uint32_t open_first = 0, open_count = 0;
  bool open_broken = false;
} g_rs;

// ME2 diagnostic (2026-10-27): who reads the queries. The guest functions that call RHIGetOcclusionQueryResult
// (sub_827DDCA8) set a tag for their call; GetData counts its answers by tag and outcome.
constexpr int kTags = 6;  // 0 other, 1 sub_82695EE0, 2 sub_82696410, 3 sub_82696780, 4 sub_826A7690, 5 sub_827486B8
constexpr const char* kTagNames[kTags] = {"other", "82695EE0", "82696410", "82696780", "826A7690", "827486B8"};
thread_local int t_tag = 0;
thread_local int t_outcome = 0;
thread_local bool t_umbra_nowait = false;  // inside Umbra's opcode 130 with the record's wait byte 0
// me2_occlusion_reader_raster (2026-11-05): console stacks put 9.2 % of the UE3 render thread in sub_82696410 (UE3's
// per-primitive occlusion history read from Umbra's op 48: ~80-530 reads per 10 s, i.e. 1-2 ms each), nearly all of it
// waiting for the previous frame's GPU query results (polling occlusionResults + sleeping): the render thread and the
// GPU took turns. With this on, a read whose GPU result is not ready at the first poll is answered from this frame's CPU
// depth buffer instead (the primitive's box, from its compact scene entry r4: +16 origin, +28 box extent): occluded =
// 0 pixels, else visible. A ready GPU result is still used. "reader_raster" in me2_ab_cycle turns it off in its slots.
thread_local bool t_reader_box = false;
thread_local float t_reader_mn[3], t_reader_mx[3];
std::atomic<uint64_t> st_reader_raster_occ{0}, st_reader_raster_vis{0}, st_reader_ready_first{0};
std::atomic<uint64_t> st_tag[kTags][5];  // outcomes: unanswerable, not ready age 0, not ready age 1+, visible, occluded
// me2-nx-deko (2026-11-06): the waits for a late GPU result by reader (count, us), and the shadow readers' late reads
// answered from their previous hidden result (me2_occlusion_nowait_shadows).
std::atomic<uint64_t> st_tag_waits[kTags], st_tag_wait_us[kTags], st_nowait_shadow{0};
std::atomic<uint64_t> st_init_views{0}, st_frames_seen{0};

// Stats (10 s report).
std::atomic<uint64_t> st_begun{0}, st_draws{0}, st_real_visible{0}, st_real_hidden{0}, st_not_ready{0},
    st_no_draws{0}, st_unknown{0}, st_waited{0}, st_wait_us{0}, st_broken{0}, st_same_frame{0},
    st_prev_visible{0}, st_prev_hidden{0}, st_nowait_visible{0};
// Answers from the GPU / not ready, by the query's age in guest frames (0, 1, 2, 3+).
std::atomic<uint64_t> st_age_ready[4], st_age_late[4];

bool Enabled() {
  int e = g_enabled.load(std::memory_order_relaxed);
  if (e < 0) {
    e = REXCVAR_GET(me2_occlusion_queries) ? 1 : 0;
    g_enabled.store(e, std::memory_order_relaxed);
    REXLOG_INFO("[occlusion] build 20261030-querybias1: independent visibility-test depth bias; camera epochs");
  }
  return e != 0;
}

void CloseOpenLocked(bool broken) {
  if (!g_rs.open_seq)
    return;
  Record& r = g_records[g_rs.open_seq & (kRecords - 1)];
  if (r.seq.load(std::memory_order_relaxed) == g_rs.open_seq) {
    r.first.store(g_rs.open_first, std::memory_order_relaxed);
    r.count.store(g_rs.open_count, std::memory_order_relaxed);
    r.state.store(broken || g_rs.open_broken ? kBroken : kClosed, std::memory_order_release);
  }
  g_rs.open_seq = 0;
}

void BeginLocked(uint64_t seq) {
  CloseOpenLocked(true);  // a BEGIN without END before it: that one is not answered from the GPU
  if (!g_rs.pool && !g_rs.pool_failed) {
    plume::RenderDevice* device = State().device.get();
    g_rs.pool = device ? device->createOcclusionQueryPool(kPoolSize) : nullptr;
    g_rs.pool_failed = !g_rs.pool;
    g_pool.store(g_rs.pool.get(), std::memory_order_release);
    REXLOG_INFO("[occlusion] host occlusion query pool: {}", g_rs.pool ? "created" : "NOT available (all visible)");
  }
  g_rs.open_seq = seq;
  g_rs.open_first = g_rs.next;
  g_rs.open_count = 0;
  g_rs.open_broken = !g_rs.pool;
}

void EndLocked(uint64_t seq) {
  if (g_rs.open_seq == seq)
    CloseOpenLocked(false);
}

uint64_t g_frame_draws = 0;  // render thread: draws recorded since the last present (me2_occlusion_umbra_defer_trace)
uint32_t BeforeDraw(plume::RenderCommandList* cmd) {
  ++g_frame_draws;
  if (!g_rs.open_seq || !g_rs.pool || g_rs.open_broken)
    return UINT32_MAX;
  uint32_t index = g_rs.next;
  if (index >= kPoolSize) {
    index = 0;
    if (g_rs.open_count)  // the issue's queries must be contiguous
      g_rs.open_broken = true;
    else
      g_rs.open_first = 0;
    if (g_rs.open_broken)
      return UINT32_MAX;
  }
  g_rs.next = index + 1;
  g_rs.pool->resetOcclusion(index, 1);
  cmd->beginOcclusionQuery(g_rs.pool.get(), index);
  ++g_rs.open_count;
  st_draws.fetch_add(1, std::memory_order_relaxed);
  return index;
}

void AfterDraw(plume::RenderCommandList* cmd, uint32_t index) { cmd->endOcclusionQuery(g_rs.pool.get(), index); }

// me2_occlusion_wait_cycle: the GetData wait in use now. Only the readers call it (UE3's render thread); the step
// changes are logged with their time, so a 10 s window can be matched to its setting.
std::atomic<uint32_t> g_wait_steps{0};  // step changes since the last 10 s report
int32_t CurrentWaitUs() {
  static const int32_t fixed = REXCVAR_GET(me2_occlusion_wait_us);
  static const std::vector<int32_t> cycle = [] {
    std::vector<int32_t> v;
    const std::string list = REXCVAR_GET(me2_occlusion_wait_cycle);
    size_t i = 0;
    while (i < list.size()) {
      size_t j = list.find(',', i);
      if (j == std::string::npos) j = list.size();
      const std::string item = list.substr(i, j - i);
      if (!item.empty()) v.push_back(std::max(0, std::atoi(item.c_str())));
      i = j + 1;
    }
    return v;
  }();
  if (cycle.empty()) {
    g_me2_occlusion_wait_setting_us.store(fixed, std::memory_order_relaxed);
    return fixed;
  }
  static const int64_t period_s = std::max(5, REXCVAR_GET(me2_occlusion_wait_cycle_s));
  static const auto start = std::chrono::steady_clock::now();
  static std::atomic<size_t> current{SIZE_MAX};
  const int64_t s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
  const size_t step = size_t(s / period_s) % cycle.size();
  size_t was = current.load(std::memory_order_relaxed);
  if (step != was && current.compare_exchange_strong(was, step)) {
    g_wait_steps.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("[occlusion] wait setting now {} us (me2_occlusion_wait_cycle step {} of {}, {} s each)", cycle[step],
                step + 1, cycle.size(), period_s);
  }
  g_me2_occlusion_wait_setting_us.store(cycle[step], std::memory_order_relaxed);
  return cycle[step];
}

void NoteWait(uint64_t us) {
  if (t_tag >= 0 && t_tag < kTags) {
    st_tag_waits[t_tag].fetch_add(1, std::memory_order_relaxed);
    st_tag_wait_us[t_tag].fetch_add(us, std::memory_order_relaxed);
  }
  st_waited.fetch_add(1, std::memory_order_relaxed);
  st_wait_us.fetch_add(us, std::memory_order_relaxed);
  g_me2_occlusion_wait_total_us.fetch_add(us, std::memory_order_relaxed);
}

void Report() {
  static std::atomic<int64_t> last{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t prev = last.load(std::memory_order_relaxed);
  if (prev == 0) {
    last.store(now, std::memory_order_relaxed);
    return;
  }
  if (now - prev < 10 || !last.compare_exchange_strong(prev, now))
    return;
  const uint64_t waited = st_waited.exchange(0);
  const uint64_t wait_us = st_wait_us.exchange(0);
  uint64_t ready[4], late[4];
  for (int i = 0; i < 4; ++i)
    ready[i] = st_age_ready[i].exchange(0), late[i] = st_age_late[i].exchange(0);
  REXLOG_INFO("[occlusion] {} s: {} queries begun, {} host draws in them; answers: {} visible + {} occluded from the "
              "GPU, {} not ready ({} of them from the current frame, not waited for), {} without a host draw, {} "
              "broken, {} unknown (answered visible); current-frame reads answered from the previous issue: {} "
              "visible + {} occluded; Umbra: {} 'not ready' polls, {} forced submits, {} real answers ({} visible / {} occluded), {} timed out (visible), {} commands skipped (me2_occlusion_umbra_skip); waited {} times, {:.1f} ms in all; by age in frames 0/1/2/3+: "
              "ready {}/{}/{}/{}, not ready {}/{}/{}/{}",
              now - prev, st_begun.exchange(0), st_draws.exchange(0), st_real_visible.exchange(0),
              st_real_hidden.exchange(0), st_not_ready.exchange(0), st_same_frame.exchange(0), st_no_draws.exchange(0),
              st_broken.exchange(0), st_unknown.exchange(0), st_prev_visible.exchange(0), st_prev_hidden.exchange(0), st_umbra_not_ready.exchange(0), st_umbra_kicks.exchange(0),
              st_umbra_real[0].load() + st_umbra_real[1].load(), st_umbra_real[0].exchange(0), st_umbra_real[1].exchange(0),
              st_umbra_timeouts.exchange(0), st_umbra_skipped.exchange(0), waited, wait_us / 1000.0, ready[0], ready[1], ready[2],
              ready[3], late[0], late[1], late[2], late[3]);
  REXLOG_INFO("[occlusion] {} s: {} draws made as repeats of the previous draw (me2_renut_repeat_draws)", now - prev,
              renut::gpu::g_repeat_draws.exchange(0));
  REXLOG_INFO("[occlusion] {} s: {} late reads answered visible at once from the previous visible result "
              "(me2_occlusion_nowait_visible); {} late shadow reads answered hidden from the previous hidden result "
              "(me2_occlusion_nowait_shadows)", now - prev, st_nowait_visible.exchange(0), st_nowait_shadow.exchange(0));
  {
    const uint32_t steps = g_wait_steps.exchange(0, std::memory_order_relaxed);
    REXLOG_INFO("[occlusion] wait setting {} us at the end of this window{}", g_me2_occlusion_wait_setting_us.load(std::memory_order_relaxed),
                steps ? fmt::format(" (changed {} times in it: mixed window)", steps) : std::string());
  }
  std::string tags;
  for (int t = 0; t < kTags; ++t) {
    uint64_t o[5];
    uint64_t any = 0;
    for (int k = 0; k < 5; ++k)
      any += (o[k] = st_tag[t][k].exchange(0));
    const uint64_t tw = st_tag_waits[t].exchange(0), twus = st_tag_wait_us[t].exchange(0);
    if (any)
      tags += fmt::format(" {}: {} unanswerable / {} not ready (age 0) / {} not ready (age 1+) / {} visible / {} "
                          "occluded / waited {} times {:.1f} ms;", kTagNames[t], o[0], o[1], o[2], o[3], o[4], tw,
                          double(twus) / 1000.0);
  }
  const uint64_t frames = g_sa2_game_frames.load(std::memory_order_relaxed);
  const uint64_t prev_frames = st_frames_seen.exchange(frames);
  if (REXCVAR_GET(me2_occlusion_reader_raster))
    REXLOG_INFO("[occlusion] reader raster (sub_82696410 reads not ready at once): {} answered occluded, {} visible "
                "from the CPU depth buffer instead of waiting", st_reader_raster_occ.exchange(0),
                st_reader_raster_vis.exchange(0));
  REXLOG_INFO("[occlusion] readers:{} InitViews (sub_8269B220) {} calls in {} Swap counts", tags,
              st_init_views.exchange(0), frames - prev_frames);
  Me2UmbraLatentReport(now - prev);
}

// The GPU's count for an earlier issue (seq) of a guest query, when it is closed, has host draws and all of their
// results are written. Never waits.
// The issue is from an earlier frame (submitted at its Swap), so it is waited for up to wait_us like the other readers.
// A host sample count as the guest's (me2_occlusion_scale): never 0 for a non-zero count (visible stays visible).
uint32_t GuestCount(uint64_t host_samples) {
  if (!host_samples) return 0;
  static const int32_t fixed = REXCVAR_GET(me2_occlusion_scale);
  double k = double(fixed);
  if (fixed <= 0) {
    const double f = double(ResolutionScale().Factor());  // host size / guest size (2/3 at 480p)
    k = f > 0.0 ? 1.0 / (f * f) : 1.0;
  }
  const double guest = std::round(double(host_samples) * k);
  return uint32_t(std::clamp(guest, 1.0, double(0x7FFFFFFFu)));
}

bool PreviousCount(uint64_t seq, int32_t wait_us, uint32_t& count) {
  if (!seq)
    return false;
  Record& r = g_records[seq & (kRecords - 1)];
  const auto start = std::chrono::steady_clock::now();
  bool waited = false;
  for (;;) {
    if (r.seq.load(std::memory_order_acquire) != seq)
      return false;
    const uint32_t state = r.state.load(std::memory_order_acquire);
    if (state == kBroken)
      return false;
    if (state == kClosed) {
      plume::RenderQueryPool* pool = g_pool.load(std::memory_order_acquire);
      const uint32_t first = r.first.load(std::memory_order_relaxed), n_all = r.count.load(std::memory_order_relaxed);
      if (!pool || n_all == 0)
        return false;
      uint64_t samples[64];
      uint64_t sum = 0;
      bool ready = true;
      for (uint32_t done = 0; ready && done < n_all;) {
        const uint32_t n = std::min<uint32_t>(n_all - done, 64);
        ready = pool->occlusionResults(first + done, n, samples);
        for (uint32_t i = 0; ready && i < n; ++i)
          sum += samples[i];
        done += n;
      }
      if (ready) {
        if (r.seq.load(std::memory_order_acquire) != seq)  // reused while reading (a ring wrap)
          return false;
        if (waited)
          NoteWait(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start).count()));
        count = GuestCount(sum);
        return true;
      }
    }
    if (wait_us <= 0 || std::chrono::steady_clock::now() - start > std::chrono::microseconds(wait_us))
      return false;
    waited = true;
    std::this_thread::yield();
  }
}

// The guest count for the latest issue of 'query', or kVisible when it cannot be answered from the GPU.
uint32_t Answer(uint32_t query, bool can_wait_later = false) {
  uint64_t seq = 0, previous = 0;
  QueryIssues(query, seq, previous);
  Record& r = g_records[seq & (kRecords - 1)];
  if (!seq || r.seq.load(std::memory_order_acquire) != seq) {
    st_unknown.fetch_add(1, std::memory_order_relaxed);
    t_outcome = 0;
    return kVisible;
  }
  const int32_t wait_us = CurrentWaitUs();
  plume::RenderQueryPool* pool = g_pool.load(std::memory_order_acquire);
  // A query of the frame the game is building cannot have reached the GPU (the frame is submitted at its Swap): never
  // waited for.
  const uint64_t age = g_sa2_game_frames.load(std::memory_order_relaxed) - r.frame.load(std::memory_order_relaxed);
  const uint32_t age_slot = uint32_t(std::min<uint64_t>(age, 3));
  std::chrono::steady_clock::time_point start{};
  bool waiting = false;
  uint64_t samples[64];
  for (;;) {
    const uint32_t state = r.state.load(std::memory_order_acquire);
    if (state == kBroken) {
      st_broken.fetch_add(1, std::memory_order_relaxed);
      t_outcome = 0;
      return kVisible;
    }
    if (state == kClosed) {
      const uint32_t first = r.first.load(std::memory_order_relaxed), count = r.count.load(std::memory_order_relaxed);
      if (count == 0) {
        st_no_draws.fetch_add(1, std::memory_order_relaxed);
        t_outcome = 0;
        return kVisible;
      }
      if (!pool) {
        st_broken.fetch_add(1, std::memory_order_relaxed);
        t_outcome = 0;
        return kVisible;
      }
      uint64_t sum = 0;
      bool ready = true;
      for (uint32_t done = 0; ready && done < count;) {
        const uint32_t n = std::min<uint32_t>(count - done, 64);
        ready = pool->occlusionResults(first + done, n, samples);
        for (uint32_t i = 0; ready && i < n; ++i)
          sum += samples[i];
        done += n;
      }
      // The record may have been reused while reading (a ring wrap): then not trusted.
      if (ready && r.seq.load(std::memory_order_acquire) == seq) {
        st_age_ready[age_slot].fetch_add(1, std::memory_order_relaxed);
        if (can_wait_later)
          st_umbra_real[sum ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
        if (waiting)
          NoteWait(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start).count()));
        if (sum) {
          st_real_visible.fetch_add(1, std::memory_order_relaxed);
          t_outcome = 3;
          return GuestCount(sum);
        }
        st_real_hidden.fetch_add(1, std::memory_order_relaxed);
        t_outcome = 4;
        return 0;
      }
    }
    // A read in the frame the query was issued in (sub_827486B8, the game's per-object visibility test: the
    // interaction markers): its result cannot exist yet. The previous issue of the same query is one frame old and
    // usually on the GPU: its answer instead of "visible" (2026-10-27; before, every such object counted as visible,
    // e.g. the markers of doors behind walls).
    static const bool use_previous = REXCVAR_GET(me2_occlusion_previous);
    uint32_t prev_count = 0;
    if (age == 0 && use_previous && PreviousCount(previous, wait_us, prev_count)) {
      (prev_count ? st_prev_visible : st_prev_hidden).fetch_add(1, std::memory_order_relaxed);
      t_outcome = prev_count ? 3 : 4;
      return prev_count;
    }
    if (age == 0 && can_wait_later) {  // Umbra reads without waiting and handles "not ready" itself
      // Its own issue is not on the GPU yet: the render thread's open list goes to the GPU now (once per new batch
      // of issues: g_seq moves at every BEGIN), queued behind this issue's END and draws.
      const uint64_t now_seq = g_seq.load(std::memory_order_relaxed);
      uint64_t kicked = g_umbra_kicked_seq.load(std::memory_order_relaxed);
      if (kicked != now_seq && g_umbra_kicked_seq.compare_exchange_strong(kicked, now_seq)) {
        st_umbra_kicks.fetch_add(1, std::memory_order_relaxed);
        render_thread::Queue([] {
          FlushVertexFillsLocked();
          FlushTextureFillsLocked();
          Video::KickLocked(true);
        });
      }
      static const int64_t umbra_wait = REXCVAR_GET(me2_occlusion_umbra_wait_us);
      const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count();
      int64_t first = r.first_poll_us.load(std::memory_order_relaxed);
      if (first == 0) {
        r.first_poll_us.compare_exchange_strong(first, now_us);
        first = now_us;
      }
      if (now_us - first > umbra_wait || r.seq.load(std::memory_order_relaxed) != seq) {
        st_umbra_timeouts.fetch_add(1, std::memory_order_relaxed);
        t_outcome = 1;
        return kVisible;
      }
      st_umbra_not_ready.fetch_add(1, std::memory_order_relaxed);
      t_outcome = 1;
      return kNotReady;
    }
    if (t_reader_box && age >= 1 && !waiting) {
      static const bool reader_raster = REXCVAR_GET(me2_occlusion_reader_raster);
      if (reader_raster && !Me2AbOff(Me2Ab::kReaderRaster)) {
        const uint8_t code = Me2RasterTestBox(t_reader_mn, t_reader_mx, g_sa2_game_frames.load(std::memory_order_relaxed));
        if (code == 2) {
          st_reader_raster_occ.fetch_add(1, std::memory_order_relaxed);
          t_outcome = 4;
          return 0;
        }
        if (code != 0) {
          st_reader_raster_vis.fetch_add(1, std::memory_order_relaxed);
          t_outcome = 3;
          return kVisible;
        }
      }
    }
    // me2-nx-deko (2026-11-06, me2_occlusion_nowait_visible): before the first wait, the previous issue's visible result.
    if (age >= 1 && !waiting && wait_us > 0) {
      static const bool nowait = REXCVAR_GET(me2_occlusion_nowait_visible);
      uint32_t prev_count = 0;
      if ((nowait || Me2AbInRotation(Me2Ab::kOcclNoWait)) && !Me2AbOff(Me2Ab::kOcclNoWait) &&
          PreviousCount(previous, 0, prev_count) && prev_count) {
        st_nowait_visible.fetch_add(1, std::memory_order_relaxed);
        t_outcome = 3;
        return prev_count;
      }
      // me2-nx-deko (2026-11-06, me2_occlusion_nowait_shadows): the shadow readers (tags 3, 4), previous issue hidden.
      static const bool nowait_shadows = REXCVAR_GET(me2_occlusion_nowait_shadows);
      if ((t_tag == 3 || t_tag == 4) && (nowait_shadows || Me2AbInRotation(Me2Ab::kOcclNoWaitShadow)) &&
          !Me2AbOff(Me2Ab::kOcclNoWaitShadow) && PreviousCount(previous, 0, prev_count) && prev_count == 0) {
        st_nowait_shadow.fetch_add(1, std::memory_order_relaxed);
        t_outcome = 4;
        return 0;
      }
    }
    if (wait_us <= 0 || age == 0) {
      st_not_ready.fetch_add(1, std::memory_order_relaxed);
      st_age_late[age_slot].fetch_add(1, std::memory_order_relaxed);
      if (age == 0)
        st_same_frame.fetch_add(1, std::memory_order_relaxed);
      t_outcome = age == 0 ? 1 : 2;
      return kVisible;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!waiting) {
      waiting = true;
      start = now;
    } else if (now - start > std::chrono::microseconds(wait_us)) {
      st_not_ready.fetch_add(1, std::memory_order_relaxed);
      st_age_late[age_slot].fetch_add(1, std::memory_order_relaxed);
      NoteWait(uint64_t(wait_us));
      t_outcome = 2;
      return kVisible;
    }
    std::this_thread::yield();
  }
}
// ---- me2_occlusion_umbra_latent (all on the UE3 render thread: Umbra's commands and their GetData) ----------------
bool UmbraLatent() {
  static const bool on = REXCVAR_GET(me2_occlusion_umbra_latent) && Enabled();
  return on;
}
constexpr uint32_t kUmbraQueryTable = 0x833D6784u;   // [table]+4: the D3D query objects, by the record's index
constexpr uint32_t kUmbraVertices = 0x8339AB68u;     // opcode 131: float3 vertices (record +16 = count)
constexpr uint32_t kUmbraIndices = 0x8339A5C8u;      // opcode 131: 3 x u32 a triangle (record +12 = count)
constexpr uint32_t kUmbraKickCounter = 0x83406564u;  // (lis -31937 = 0x833F) opcode 129 counts, opcode 130 kicks the command buffer if != 0
Me2OcclusionViewHistory g_latent_view;
struct LatentIssue {
  uint64_t seq, key, frame;
  uint8_t cpu = 0;  // me2_occlusion_umbra_cpu: the CPU's answer to the same test (1 visible, 2 occluded), 0 = none
  Me2OcclusionBounds bounds;
  uint64_t epoch = 1;  // camera at the geometry issue, never the later collection/read
  uint8_t cpu_alt = 0;  // me2_occlusion_raster_dual: the other winding's answer (codes as cpu)
  uint32_t raster_token = 0;  // me2_occlusion_raster_trace: the rasterizer's record of an occluded answer
};
// me2_occlusion_umbra_cpu: CPU answers against the GPU counts of the same tests, [CPU occluded][GPU visible]; and
// the tests the GPU found occluded but the CPU not, by the CPU's reason (CpuTest's codes).
std::atomic<uint64_t> st_cpu_cmp[2][2];
std::atomic<uint64_t> st_cpu_miss[9];
std::atomic<uint64_t> st_raster_alt_cmp[2][2];  // me2_occlusion_raster_dual: the other winding, [occluded][GPU visible]
struct LatentResult {
  uint32_t pixels = 0;  // the answer: the count that last changed it or agreed with it
  uint64_t frame = 0;  // the frame of the newest issue counted
  uint64_t issued = 0;  // the frame of its last test actually drawn (me2_occlusion_umbra_retest_frames)
  uint32_t against = 0;  // independent fresh zero-count frames before starting occlusion
  uint64_t zero_frame = 0;  // last frame counted toward that confirmation (not a count of repeated reads)
  bool any = false;
  Me2OcclusionBounds bounds;  // the volume of the accepted GPU result, not just its centre cell
  uint64_t epoch = 0;
};
struct Latent {
  std::unordered_map<uint32_t, uint64_t> key_by_index;  // the record index's key since its last opcode 131
  std::unordered_map<uint32_t, Me2OcclusionBounds> bounds_by_index;
  std::unordered_map<uint32_t, uint64_t> epoch_by_index;
  std::unordered_map<uint64_t, uint64_t> last_seen[4];   // per key kind (me2_occlusion_umbra_latent_key_stats only)
  std::deque<LatentIssue> pending;
} g_latent;

// 2026-10-28 (console log 18:48): the results were an unordered_map that grew by ~1000-1500 keys a second (Umbra's
// boxes move a little with the view, so their centres make new keys), rehashed as it grew and was cleared whole at
// 65536 keys every 3-6 minutes (then every read answered 'visible' until counts came back). Now a fixed table: a key
// probes kLatentProbe slots and a new key takes an empty one or the one used longest ago; no growth, no clear.
struct LatentSlot {
  uint64_t key = 0;
  uint64_t seen = 0;  // frame of the key's last opcode 131
  LatentResult res;
  // me2_occlusion_umbra_flip_trace: the last answer read for the key (0 none, 1 visible, 2 occluded) and since when.
  uint8_t answered = 0;
  uint16_t flips = 0;  // answer changes since flips_from
  uint64_t answered_frame = 0, answer_since = 0, flips_from = 0;
  // Stale-count check: a read answered from an occluded count older than latent_frames waits for the count of the
  // box drawn at that read (the truth then) and is tallied by how far the camera had moved (st_stale_check).
  uint64_t cpu_occluded_frame = 0;  // me2_occlusion_cpu_hold: the CPU's last own 'occluded' for the key
  uint8_t cpu_visible_streak = 0;   // me2_occlusion_cpu_debounce: 'visible' answers held back since the last 'occluded' 
  uint8_t probe = 0;  // bucket + 1
  uint64_t probe_frame = 0;
};
// [rotation bucket: <=0.15, <=0.3, <=0.6, more or not kept][move bucket: <=200, <=500, more][0 still occluded, 1 visible]
std::atomic<uint64_t> st_stale_check[4][3][2];
constexpr uint32_t kLatentBits = 15, kLatentProbe = 8;
LatentSlot g_lat_slots[1u << kLatentBits];
uint32_t g_lat_used = 0;  // slots holding a key (report)

uint32_t LatentHome(uint64_t key) {
  return uint32_t((key * 0x9E3779B97F4A7C15ull) >> (64 - kLatentBits));
}
uint64_t LatentLastUse(const LatentSlot& s) {
  return std::max(s.seen, std::max(s.res.frame, s.res.issued));
}
LatentSlot* LatentFind(uint64_t key) {
  const uint32_t home = LatentHome(key);
  for (uint32_t i = 0; i < kLatentProbe; ++i) {
    LatentSlot& s = g_lat_slots[(home + i) & ((1u << kLatentBits) - 1)];
    if (s.key == key)
      return &s;
  }
  return nullptr;
}
// The key's slot, taken for it if it has none.
LatentSlot& LatentClaim(uint64_t key) {
  const uint32_t home = LatentHome(key);
  LatentSlot* victim = nullptr;
  for (uint32_t i = 0; i < kLatentProbe; ++i) {
    LatentSlot& s = g_lat_slots[(home + i) & ((1u << kLatentBits) - 1)];
    if (s.key == key)
      return s;
    if (!victim || (victim->key && (!s.key || LatentLastUse(s) < LatentLastUse(*victim))))
      victim = &s;
  }
  if (!victim->key)
    ++g_lat_used;
  *victim = LatentSlot{};
  victim->key = key;
  return *victim;
}
std::atomic<uint64_t> st_lat_keys{0}, st_lat_repeat{0}, st_lat_rep_kind[4], st_lat_dup_kind[4], st_lat_zero{0}, st_lat_visible{0}, st_lat_hidden{0}, st_lat_none{0},
    st_lat_collected{0}, st_lat_dropped{0};
std::atomic<uint64_t> st_lat_held{0};  // counts that did not change the answer yet (me2_occlusion_umbra_latent_confirm)
thread_local uint64_t t_latent_key = 0;  // the key of the record opcode 130 reads (GetData answers it)
Me2OcclusionBounds CurrentLatentBounds();  // uses the existing opcode-130 index; no additional native TLS object
std::atomic<uint64_t> st_lat_bounds_rejected{0};
std::atomic<uint64_t> st_retest_skipped{0}, st_retest_cancelled{0};  // me2_occlusion_umbra_retest_frames
std::atomic<uint64_t> st_retest_occ_skipped{0}, st_retest_occ_tested{0};  // me2_occlusion_umbra_retest_occluded
std::atomic<uint64_t> st_retest_occ_moved{0};  // ... re-tested because the node's box is not the counted one
// me2_occlusion_umbra_retest_occluded with its A/B half.
uint64_t RetestOccludedFrames() {
  static const uint64_t n = uint64_t(REXCVAR_GET(me2_occlusion_umbra_retest_occluded));
  static const int32_t ab_s = REXCVAR_GET(me2_occlusion_umbra_retest_occluded_ab_s);
  if (n > 1 && Me2AbOff(Me2Ab::kRetestOccluded))
    return 1;
  if (n <= 1 || ab_s <= 0)
    return n;
  static const auto start = std::chrono::steady_clock::now();
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
  return ((secs / ab_s) & 1) ? 1 : n;  // the "off" half of the A/B
}
// The query of the Umbra test whose box opcode 131 just skipped: its END (opcode 129 -> Issue) drops the carried BEGIN.
thread_local uint32_t t_umbra_skip_query = 0;

// Each record index's key since its last opcode 131 (2026-10-28: an array for the usual indices, no hashing), and
// whether that test was skipped (me2_occlusion_umbra_retest_frames).
struct IndexKey {
  uint32_t tag = 0;  // index + 1
  bool skipped = false;
  uint64_t key = 0;
  Me2OcclusionBounds bounds;
  uint64_t epoch = 0;
};
constexpr uint32_t kIndexKeys = 1u << 13;
IndexKey g_index_keys[kIndexKeys];
void SetIndexKey(uint32_t index, uint64_t key, bool skipped, const Me2OcclusionBounds& bounds);
bool GetIndexKey(uint32_t index, uint64_t& key, bool& skipped);

void SetIndexKey(uint32_t index, uint64_t key, bool skipped, const Me2OcclusionBounds& bounds) {
  if (index < kIndexKeys) {
    g_index_keys[index] = {index + 1, skipped, key, bounds, g_latent_view.epoch};
    return;
  }
  g_latent.key_by_index[index] = skipped ? 0 : key;  // rare: a skipped test of a large index is then not counted
  g_latent.bounds_by_index[index] = bounds;
  g_latent.epoch_by_index[index] = g_latent_view.epoch;
}
uint64_t IndexEpoch(uint32_t index) {
  if (index < kIndexKeys)
    return g_index_keys[index].tag == index + 1 ? g_index_keys[index].epoch : 0;
  const auto it = g_latent.epoch_by_index.find(index);
  return it != g_latent.epoch_by_index.end() ? it->second : 0;
}
Me2OcclusionBounds IndexBounds(uint32_t index) {
  if (index < kIndexKeys)
    return g_index_keys[index].tag == index + 1 ? g_index_keys[index].bounds : Me2OcclusionBounds{};
  const auto it = g_latent.bounds_by_index.find(index);
  return it != g_latent.bounds_by_index.end() ? it->second : Me2OcclusionBounds{};
}
bool GetIndexKey(uint32_t index, uint64_t& key, bool& skipped) {
  skipped = false;
  if (index < kIndexKeys) {
    const IndexKey& k = g_index_keys[index];
    if (k.tag != index + 1)
      return false;
    key = k.key;
    skipped = k.skipped;
    return true;
  }
  auto it = g_latent.key_by_index.find(index);
  if (it == g_latent.key_by_index.end())
    return false;
  key = it->second;
  return true;
}

uint32_t QueryOfIndex(const uint8_t* base, uint32_t index) {
  const uint32_t table = LoadBE(base, kUmbraQueryTable);
  if (!table)
    return 0;
  const uint32_t list = LoadBE(base, table + 4);
  return list ? LoadBE(base, list + index * 4) : 0;
}

Me2OcclusionBounds TestBounds(const uint8_t* base, uint32_t rec) {
  if (LoadBE(base, rec + 16) != 8 || LoadBE(base, rec + 12) != 12) return {};
  float vertices[8][3];
  for (uint32_t v = 0; v < 8; ++v)
    for (uint32_t c = 0; c < 3; ++c) {
      const uint32_t bits = LoadBE(base, kUmbraVertices + 12 * v + 4 * c);
      std::memcpy(&vertices[v][c], &bits, 4);
    }
  return Me2OcclusionBounds::FromCorners(vertices);
}

// Keep the stable centre lookup, but never use it as proof of volume identity.
uint64_t CentreKey(const Me2OcclusionBounds& bounds) {
  if (!bounds.Valid()) return 0;
  uint64_t key = uint64_t(3) << 62;
  for (uint32_t c = 0; c < 3; ++c) {
    const double cell = std::floor((double(bounds.lo[c]) + bounds.hi[c]) / 16.0 + 0.5);
    if (cell < -524288.0 || cell > 524287.0) return 0;  // no wrapping distant volumes onto one centre
    const int64_t q = int64_t(cell);
    key ^= uint64_t(q & 0xFFFFF) << (20 * c);
  }
  return key;
}

uint64_t GeometryKey(const uint8_t* base, uint32_t rec) {
  const uint32_t verts = LoadBE(base, rec + 16), tris = LoadBE(base, rec + 12);
  if (verts == 0 || verts > 4096 || tris > 8192)
    return 0;
  uint64_t h = 1469598103934665603ull ^ (uint64_t(verts) << 32 | tris);
  auto mix = [&](uint32_t va, uint32_t bytes) {
    const uint8_t* p = base + va;
    for (uint32_t i = 0; i + 8 <= bytes; i += 8) {
      uint64_t w;
      std::memcpy(&w, p + i, 8);
      h = (h ^ w) * 1099511628211ull;
      h ^= h >> 29;
    }
    if (bytes & 4) {
      uint32_t w;
      std::memcpy(&w, p + (bytes & ~7u), 4);
      h = (h ^ w) * 1099511628211ull;
    }
  };
  mix(kUmbraVertices, verts * 12);
  mix(kUmbraIndices, tris * 12);
  return h ? h : 1;
}

// One known failing node only. Do not infer collection age from a later read's
// cached age: log the entire issue -> collect -> answer chain independently.
constexpr uint64_t kWallTraceKey = (uint64_t(3) << 62) ^ (uint64_t(-2290ll) & 0xFFFFF) ^
                                   (uint64_t(30) << 20) ^ (uint64_t(663) << 40);
bool WallTraceAllowed(uint64_t key, uint64_t frame) {
  if (key != kWallTraceKey) return false;
  static const bool on = REXCVAR_GET(me2_occlusion_umbra_wall_trace);
  if (!on) return false;
  static uint64_t first = ~uint64_t(0);
  static uint32_t lines = 0;
  if (first == ~uint64_t(0)) {
    first = frame;
    REXLOG_INFO("[umbra-chain-v1] start frame {}: bounded wall diagnostic, fresh-policy build 20261029-chain1", frame);
  }
  if (frame < first || frame - first > 900 || lines >= 2000) return false;
  ++lines;
  return true;
}

void WallTraceCollect(const LatentIssue& issue, uint64_t frame, bool ready, uint32_t count,
                      const LatentResult& before, const LatentResult& after, bool accepted) {
  if (issue.key != kWallTraceKey) return;
  if (!ready) {
    // The queue head can be polled once for every other node in this frame.
    // Keep a single wait row so those polls cannot exhaust the useful trace.
    static uint64_t waited_frame = ~uint64_t(0), waited_seq = 0;
    if (waited_frame == frame && waited_seq == issue.seq) return;
    waited_frame = frame;
    waited_seq = issue.seq;
  }
  if (!WallTraceAllowed(issue.key, frame)) return;
  const Record& r = g_records[issue.seq & (kRecords - 1)];
  uint64_t q[4] = {};
  bool raw = false;
#if defined(PLUME_DEKO3D)
  if (renut::gpu::IsDeko3D())
    raw = plume::Deko3DOcclusionSnapshot(g_pool.load(std::memory_order_acquire), r.first.load(), q);
#endif
  REXLOG_INFO("[umbra-chain-v1] collect now {} issue {} seq {} record {}/{}/{} slots {}+{} ready {} px {} raw {} [{},{},{},{}] accepted {} cache {}/{}/{} -> {}/{}/{} candidate {}/{} box [{:.3f},{:.3f},{:.3f}]-[{:.3f},{:.3f},{:.3f}]",
              frame, issue.frame, issue.seq, r.seq.load(), r.frame.load(), r.state.load(),
              r.first.load(), r.count.load(), ready, count, raw, q[0], q[1], q[2], q[3], accepted,
              before.any, before.frame, before.pixels, after.any, after.frame, after.pixels,
              after.against, after.zero_frame, issue.bounds.lo[0], issue.bounds.lo[1], issue.bounds.lo[2],
              issue.bounds.hi[0], issue.bounds.hi[1], issue.bounds.hi[2]);
}

void WallTraceAnswer(uint64_t key, uint64_t frame, uint32_t count) {
  if (!WallTraceAllowed(key, frame)) return;
  const LatentSlot* slot = LatentFind(key);
  const Me2OcclusionBounds current = CurrentLatentBounds();
  REXLOG_INFO("[umbra-chain-v1] answer frame {} px {} cache {}/{}/{} bounds_ok {} current [{:.3f},{:.3f},{:.3f}]-[{:.3f},{:.3f},{:.3f}]",
              frame, count, slot && slot->res.any, slot ? slot->res.frame : 0, slot ? slot->res.pixels : 0,
              slot && slot->res.bounds.Contains(current), current.lo[0], current.lo[1], current.lo[2],
              current.hi[0], current.hi[1], current.hi[2]);
}

// A cached zero is weaker evidence than a currently visible result. The Citadel
// trace repeatedly changed a visible ~140k-pixel box to hidden on one 8-frame-old
// zero, then back to visible two frames later with an unchanged camera.
// Start hiding only after independent fresh zero results; once confirmed, the
// usual cache lifetime still applies. A positive result always wins immediately.
bool ApplyLatentCount(LatentResult& res, const LatentIssue& issue, uint32_t count,
                      uint64_t now, uint32_t confirm, uint64_t max_age) {
  if (issue.epoch != g_latent_view.epoch) return false;
  if (res.epoch != issue.epoch) {
    res = LatentResult{};
    res.epoch = issue.epoch;
  }
  if (issue.frame > now || issue.frame < res.frame || (res.against && issue.frame < res.zero_frame))
    return false;
  const bool confirmed_hidden = res.any && res.pixels == 0 && res.frame <= now && now - res.frame <= max_age;
  if (count == 0 && !confirmed_hidden) {
    constexpr uint64_t kFreshFrames = 2;
    if (now - issue.frame > kFreshFrames) {
      res.against = 0;
      return false;
    }
    // Umbra may test the same node only once every several frames. Independent
    // fresh results need not come from adjacent frames; a positive result or a
    // stale negative cancels the candidate instead.
    if (!res.against || issue.frame != res.zero_frame)
      ++res.against;
    res.zero_frame = issue.frame;
    if (res.against < std::max<uint32_t>(2, confirm))
      return false;
  }
  res.frame = issue.frame;
  res.pixels = count;
  res.bounds = issue.bounds;
  res.against = 0;
  res.any = true;
  return true;
}

// GPU counts of earlier issues that are ready now (in issue order; never waits).
void LatentCollect(uint64_t frame) {
  static const uint64_t max_age = uint64_t(REXCVAR_GET(me2_occlusion_umbra_latent_frames));
  while (!g_latent.pending.empty()) {
    const LatentIssue& i = g_latent.pending.front();
    const Record& r = g_records[i.seq & (kRecords - 1)];
    uint32_t count = 0;
    if (PreviousCount(i.seq, 0, count)) {
      LatentSlot& ls = LatentClaim(i.key);
      LatentResult& res = ls.res;
      if (i.cpu) {
        const bool occ = i.cpu == 2 || i.cpu == 7 || i.cpu == 9 || i.cpu == 10 || i.cpu == 11;
        st_cpu_cmp[occ][count != 0].fetch_add(1, std::memory_order_relaxed);
        if (!occ && count == 0 && i.cpu < 9)
          st_cpu_miss[i.cpu].fetch_add(1, std::memory_order_relaxed);
      }
      if (i.cpu_alt)
        st_raster_alt_cmp[i.cpu_alt == 2][count != 0].fetch_add(1, std::memory_order_relaxed);
      if (i.raster_token && count != 0)
        Me2RasterTraceFalse(i.raster_token, count, frame);
      if (ls.probe && i.frame >= ls.probe_frame) {
        const uint32_t b = ls.probe - 1;
        st_stale_check[b / 3][b % 3][count != 0].fetch_add(1, std::memory_order_relaxed);
        ls.probe = 0;
      }
      static const uint32_t confirm = uint32_t(REXCVAR_GET(me2_occlusion_umbra_latent_confirm));
      const LatentResult before = res;
      const bool accepted = ApplyLatentCount(res, i, count, frame, confirm, max_age);
      if (!accepted)
        st_lat_held.fetch_add(1, std::memory_order_relaxed);
      WallTraceCollect(i, frame, true, count, before, res, accepted);
      st_lat_collected.fetch_add(1, std::memory_order_relaxed);
    } else if (frame < i.frame + 8 && r.seq.load(std::memory_order_relaxed) == i.seq &&
               r.state.load(std::memory_order_relaxed) != kBroken &&
               !(r.state.load(std::memory_order_relaxed) == kClosed && r.count.load(std::memory_order_relaxed) == 0)) {
      WallTraceCollect(i, frame, false, 0, LatentResult{}, LatentResult{}, false);
      break;  // not on the GPU yet: the later ones neither
    } else {
      st_lat_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_latent.pending.pop_front();
  }
  if (g_latent.pending.size() > 65536)
    g_latent.pending.clear();
  for (auto& m : g_latent.last_seen)
    if (m.size() > 65536)
      m.clear();
}

// The view matrix of each frame (UE3 FMatrix, row-major: rows 0-2 the rotation, row 3 the translation), from the view
// UE3 hands Umbra (sub_8274AA38, r4 = view, +64). Written and read on the UE3 render thread.
struct ViewAt {
  uint64_t frame = ~uint64_t(0);
  float m[16];
  float p[16];  // me2-nx-deko (2026-11-04): the projection too (BoxOnScreenAt)
};
constexpr uint32_t kViewHist = 256;  // frames of view matrices kept (latent_still_frames reaches back this far)
ViewAt g_view_hist[kViewHist];
std::atomic<uint64_t> st_lat_moved{0};
std::atomic<uint64_t> st_lat_turn_kept{0};  // me2_occlusion_umbra_latent_turn: occluded answers kept through a turn
std::atomic<uint64_t> st_lat_turn_old{0};   // ... older than latent_frames (me2_occlusion_umbra_latent_turn_frames)

bool BoxOnScreenAt(const Me2OcclusionBounds& b, uint64_t f);
// Whether the box left the screen at some frame after 'then' and before 'now' (8 frames sampled; a frame no longer kept
// counts as not known: false).
bool BoxLeftScreenSince(const Me2OcclusionBounds& b, uint64_t then, uint64_t now) {
  if (now <= then + 1) return false;
  for (uint64_t k = 1; k <= 8; ++k) {
    const uint64_t f = then + (now - then) * k / 9;
    if (f <= then || f >= now) continue;
    const auto& v = g_view_hist[f & (kViewHist - 1)];
    if (v.frame != f) continue;
    if (!BoxOnScreenAt(b, f)) return true;
  }
  return false;
}

// Whether the box was wholly inside the view of frame 'f' (row vectors: clip = (p, 1) x View x Projection). Its GPU count
// then covered all of it: turning the camera cannot reveal a part the test did not see. False when 'f' is not kept.
bool BoxOnScreenAt(const Me2OcclusionBounds& b, uint64_t f) {
  if (!b.Valid()) return false;
  const ViewAt& v = g_view_hist[f & (kViewHist - 1)];
  if (v.frame != f) return false;
  for (unsigned corner = 0; corner < 8; ++corner) {
    const float p[3] = {(corner & 1) ? b.hi[0] : b.lo[0], (corner & 2) ? b.hi[1] : b.lo[1],
                        (corner & 4) ? b.hi[2] : b.lo[2]};
    float e[4];
    for (int j = 0; j < 4; ++j)
      e[j] = p[0] * v.m[0 * 4 + j] + p[1] * v.m[1 * 4 + j] + p[2] * v.m[2 * 4 + j] + v.m[3 * 4 + j];
    float c[4];
    for (int j = 0; j < 4; ++j)
      c[j] = e[0] * v.p[0 * 4 + j] + e[1] * v.p[1 * 4 + j] + e[2] * v.p[2 * 4 + j] + e[3] * v.p[3 * 4 + j];
    if (!(c[3] > 0.0f) || std::fabs(c[0]) > c[3] || std::fabs(c[1]) > c[3]) return false;
  }
  return true;
}

// The age in frames of the 'occluded' answers given (1-2, 3-5, 6-10, 11-20, 21+): the delay with which a node behind an
// occluder that moved away (a door, a character; the camera still) comes back, bounded by
// me2_occlusion_umbra_latent_frames.
std::atomic<uint64_t> st_lat_age[5];

// The camera position of a view matrix: -translation * rotation^T.
void CameraPos(const float* m, float out[3]) {
  for (int c = 0; c < 3; ++c)
    out[c] = -(m[12] * m[c * 4 + 0] + m[13] * m[c * 4 + 1] + m[14] * m[c * 4 + 2]);
}

// How far the camera of 'then' is from the current one: the largest change of a rotation element and the distance
// moved; false when either frame's view is no longer kept.
bool CameraDelta(uint64_t then, uint64_t now, float& rot, float& move) {
  const ViewAt& a = g_view_hist[then & (kViewHist - 1)];
  const ViewAt& b = g_view_hist[now & (kViewHist - 1)];
  if (a.frame != then || b.frame != now)
    return false;
  rot = 0.0f;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      rot = std::max(rot, std::fabs(a.m[r * 4 + c] - b.m[r * 4 + c]));
  float pa[3], pb[3];
  CameraPos(a.m, pa);
  CameraPos(b.m, pb);
  const float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
  move = std::sqrt(dx * dx + dy * dy + dz * dz);
  return true;
}

// Whether the camera of 'then' is within 'rot' / 'move' of the current one.
bool CameraNear(uint64_t then, uint64_t now, float rot, float move) {
  float r, m;
  return CameraDelta(then, now, r, m) && r <= rot && m <= move;
}

bool CameraStill(uint64_t then, uint64_t now) {
  static const float rot = float(REXCVAR_GET(me2_occlusion_umbra_latent_rot));
  static const float move = float(REXCVAR_GET(me2_occlusion_umbra_latent_move));
  return CameraNear(then, now, rot, move);
}

std::atomic<uint64_t> st_lat_still{0};  // occluded answers older than latent_frames kept by a still camera

// me2_occlusion_umbra_flip_trace: the box centre of a key of kind 3 (CentreKey: 20 bits a coordinate, 8 units a step).
int32_t CentreCell(uint64_t key, int c) {
  return int32_t(uint32_t(key >> (20 * c)) << 12) >> 12;
}
uint64_t CentreKeyOf(const int32_t cell[3]) {
  uint64_t key = uint64_t(3) << 62;
  for (int c = 0; c < 3; ++c)
    key ^= uint64_t(uint32_t(cell[c]) & 0xFFFFF) << (20 * c);
  return key;
}

// Why a read was answered as it was (the flip trace).
enum class Why { kNoSlot, kNoCount, kTooOld, kMoved, kCount, kStill, kCpu, kBounds };
const char* WhyName(Why w) {
  switch (w) {
    case Why::kNoSlot: return "no slot for the key (new key)";
    case Why::kNoCount: return "no count yet for the key";
    case Why::kTooOld: return "count too old";
    case Why::kMoved: return "occluded count but the camera moved";
    case Why::kCount: return "count";
    case Why::kStill: return "old occluded count kept, camera still";
    case Why::kCpu: return "CPU test";
    case Why::kBounds: return "occluded count belongs to a different volume";
  }
  return "?";
}

// 2026-10-28 (door prompt still blinking on the PC with latent_still_frames): every change of a key's answer
// (occluded <-> visible) with its reason, the count's age / pixels and how often Umbra reads the key; for a key with
// no count, whether a neighbouring centre cell (the box's centre moved across the 8-unit grid) holds an occluded count.
// 2026-10-28 21:5x: the interaction target's position (me2_selection.cpp, game thread); every Umbra answer for a node
// whose centre is within 600 units of it is logged ([umbra-door]), changes or not: is the door an Umbra node at all,
// and what does it answer in the frames it gets rendered.
std::atomic<int32_t> g_watch_pos[3] = {};
std::atomic<uint64_t> g_watch_frame{0};
void TraceWatch(uint64_t key, uint64_t frame, bool visible, Why why, uint64_t age, uint32_t pixels,
                const std::string& extra) {
  const uint64_t wf = g_watch_frame.load(std::memory_order_relaxed);
  if (!wf || frame - wf > 90 || (key >> 62) != 3)
    return;
  float d2 = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float d = float(CentreCell(key, i) * 8 - g_watch_pos[i].load(std::memory_order_relaxed));
    d2 += d * d;
  }
  if (d2 > 600.0f * 600.0f)
    return;
  static uint64_t window = ~uint64_t(0);
  static uint32_t lines = 0;
  if (frame / 30 != window) {
    window = frame / 30;
    lines = 0;
  }
  if (++lines > 40)
    return;
  REXLOG_INFO("[umbra-door] frame {} centre ({}, {}, {}) {:.0f} units from the target: {} - {} (count age {}, {} px){}",
              frame, CentreCell(key, 0) * 8, CentreCell(key, 1) * 8, CentreCell(key, 2) * 8, std::sqrt(d2),
              visible ? "VISIBLE" : "occluded", WhyName(why), age, pixels, extra);
}

void TraceFlip(LatentSlot* slot, uint64_t key, uint64_t frame, bool visible, Why why, uint64_t age, uint32_t pixels,
               const std::string& extra = {}) {
  TraceWatch(key, frame, visible, why, age, pixels, extra);
  if (!slot)
    return;
  const uint8_t now = visible ? 1 : 2;
  const uint8_t before = slot->answered;
  const uint64_t since_read = slot->answered_frame ? frame - slot->answered_frame : 0;
  if (before != now) {
    const uint64_t held = slot->answer_since ? frame - slot->answer_since : 0;
    slot->answer_since = frame;
    // Only nodes that keep changing (3+ changes within 300 frames): a blinking one, not the ordinary come-and-go.
    if (frame - slot->flips_from > 300) {
      slot->flips_from = frame;
      slot->flips = 0;
    }
    ++slot->flips;
    static uint64_t window = ~uint64_t(0);
    static uint32_t lines = 0;
    if (frame / 300 != window) {
      window = frame / 300;
      lines = 0;
    }
    if (before != 0 && slot->flips >= 3 && lines < 200 && (key >> 62) == 3) {
      ++lines;
      std::string nearby;
      if (visible && (why == Why::kNoCount || why == Why::kNoSlot)) {
        int32_t cell[3] = {CentreCell(key, 0), CentreCell(key, 1), CentreCell(key, 2)};
        for (int dx = -1; dx <= 1; ++dx)
          for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz) {
              if (!dx && !dy && !dz)
                continue;
              const int32_t n[3] = {cell[0] + dx, cell[1] + dy, cell[2] + dz};
              if (const LatentSlot* s = LatentFind(CentreKeyOf(n)); s && s->res.any && frame - s->res.frame <= 60)
                nearby += fmt::format(" [{:+d}{:+d}{:+d}: {} px, {} frames old]", dx, dy, dz, s->res.pixels,
                                    frame - s->res.frame);
            }
      }
      float cam[3] = {};
      if (g_view_hist[frame & (kViewHist - 1)].frame == frame)
        CameraPos(g_view_hist[frame & (kViewHist - 1)].m, cam);
      // The camera's change since the count was taken (what latent_still_rot / _move are compared with).
      float d_rot = 0.0f, d_move = 0.0f;
      std::string moved;
      if (age && CameraDelta(frame - age, frame, d_rot, d_move))
        moved = fmt::format("; camera since the count: rot {:.3f}, moved {:.0f}", d_rot, d_move);
      else if (age)
        moved = "; camera since the count: not kept";
      REXLOG_INFO("[umbra-flip] frame {} camera ({:.0f}, {:.0f}, {:.0f}) centre ({}, {}, {}): {} -> {} after {} frames; "
                  "{} (count age {}, {} px); read again after {} frames{}{}{}",
                  frame, cam[0], cam[1], cam[2], CentreCell(key, 0) * 8, CentreCell(key, 1) * 8, CentreCell(key, 2) * 8,
                  before == 1 ? "visible" : "occluded", visible ? "visible" : "occluded", held, WhyName(why), age,
                  pixels, since_read, moved, nearby.empty() ? "" : "; neighbour cells with a count:", nearby + extra);
    }
  }
  slot->answered = now;
  slot->answered_frame = frame;
}

// me2_occlusion_umbra_cpu (defined below): what the CPU test of opcode 130's record saw, for the flip trace.
std::string (*g_cpu_info_text)() = nullptr;

uint32_t LatentAnswer(uint64_t key) {
  if (Me2AbOff(Me2Ab::kOcclusion))
    return kVisible;  // me2-nx-deko (2026-11-04): the draw-count rotation's reference slot (no occlusion culling)
  static const uint64_t max_age = uint64_t(REXCVAR_GET(me2_occlusion_umbra_latent_frames));
  static const uint64_t still_age = uint64_t(REXCVAR_GET(me2_occlusion_umbra_latent_still_frames));
  static const float still_rot = float(REXCVAR_GET(me2_occlusion_umbra_latent_still_rot));
  static const float still_move = float(REXCVAR_GET(me2_occlusion_umbra_latent_still_move));
  static const bool trace = REXCVAR_GET(me2_occlusion_umbra_flip_trace);
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  LatentSlot* slot = key ? LatentFind(key) : nullptr;
  const std::string cpu = trace && g_cpu_info_text ? g_cpu_info_text() : std::string();
  if (!slot || !slot->res.any || slot->res.epoch != g_latent_view.epoch) {
    st_lat_none.fetch_add(1, std::memory_order_relaxed);
    if (trace)
      TraceFlip(slot, key, frame, true, slot ? Why::kNoCount : Why::kNoSlot, 0, 0, cpu);
    return kVisible;
  }
  const LatentResult& res = slot->res;
  const uint64_t age = frame - res.frame;
  static const bool centre_key = REXCVAR_GET(me2_occlusion_umbra_latent_key) == 3;
  if (centre_key && res.pixels == 0 && !res.bounds.Contains(CurrentLatentBounds())) {
    st_lat_bounds_rejected.fetch_add(1, std::memory_order_relaxed);
    if (trace) TraceFlip(slot, key, frame, true, Why::kBounds, age, 0, cpu);
    return kVisible;
  }
  Why why = Why::kCount;
  if (age > max_age && res.pixels == 0 && !slot->probe) {
    float d_rot = 1.0f, d_move = 1e9f;
    CameraDelta(res.frame, frame, d_rot, d_move);
    const uint32_t r = d_rot <= 0.15f ? 0 : d_rot <= 0.3f ? 1 : d_rot <= 0.6f ? 2 : 3;
    const uint32_t m = d_move <= 200.0f ? 0 : d_move <= 500.0f ? 1 : 2;
    slot->probe = uint8_t(1 + r * 3 + m);
    slot->probe_frame = frame;
  }
  // me2-nx-deko (2026-11-04, me2_occlusion_umbra_latent_turn_frames): an old 'occluded' of a box the camera turned away
  // from and back to, the camera in place: still right (occlusion depends on where the camera is).
  bool turn_old = false;
  if (age > max_age && res.pixels == 0) {
    static const bool turn = REXCVAR_GET(me2_occlusion_umbra_latent_turn);
    static const uint64_t turn_age = uint64_t(REXCVAR_GET(me2_occlusion_umbra_latent_turn_frames));
    static const float move_max = float(REXCVAR_GET(me2_occlusion_umbra_latent_move));
    float d_rot = 0.0f, d_move = 1e9f;
    if (turn && age <= turn_age && CameraDelta(res.frame, frame, d_rot, d_move) && d_move <= move_max &&
        BoxOnScreenAt(res.bounds, res.frame) && BoxLeftScreenSince(res.bounds, res.frame, frame)) {
      turn_old = true;
      st_lat_turn_old.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (turn_old) {
    why = Why::kStill;
  } else if (age > max_age) {
    // me2_occlusion_umbra_latent_still_frames: an old 'occluded' stays while the camera has not moved since.
    if (!(res.pixels == 0 && age <= still_age && CameraNear(res.frame, frame, still_rot, still_move))) {
      st_lat_none.fetch_add(1, std::memory_order_relaxed);
      if (trace)
        TraceFlip(slot, key, frame, true, Why::kTooOld, age, res.pixels, cpu);
      return kVisible;
    }
    st_lat_still.fetch_add(1, std::memory_order_relaxed);
    why = Why::kStill;
  } else if (res.pixels == 0 && !CameraStill(res.frame, frame)) {
    // me2-nx-deko (2026-11-04, me2_occlusion_umbra_latent_turn): a camera that only turned keeps the answer of a box
    // that was wholly on screen at its count.
    static const bool turn = REXCVAR_GET(me2_occlusion_umbra_latent_turn);
    static const float move_max = float(REXCVAR_GET(me2_occlusion_umbra_latent_move));
    float d_rot = 0.0f, d_move = 1e9f;
    if (turn && CameraDelta(res.frame, frame, d_rot, d_move) && d_move <= move_max && BoxOnScreenAt(res.bounds, res.frame)) {
      st_lat_turn_kept.fetch_add(1, std::memory_order_relaxed);
    } else {
      st_lat_moved.fetch_add(1, std::memory_order_relaxed);  // occluded from a view the camera has left: visible
      if (trace)
        TraceFlip(slot, key, frame, true, Why::kMoved, age, 0, cpu);
      return kVisible;
    }
  }
  (res.pixels ? st_lat_visible : st_lat_hidden).fetch_add(1, std::memory_order_relaxed);
  if (res.pixels == 0)
    st_lat_age[age <= 2 ? 0 : age <= 5 ? 1 : age <= 10 ? 2 : age <= 20 ? 3 : 4].fetch_add(1, std::memory_order_relaxed);
  if (trace)
    TraceFlip(slot, key, frame, res.pixels != 0, why, age, res.pixels, cpu);
  return res.pixels;
}

void StaleCheckReport() {
  static const char* kRot[4] = {"rot<=0.15", "rot<=0.3", "rot<=0.6", "rot>0.6"};
  static const char* kMove[3] = {"move<=200", "move<=500", "move>500"};
  std::string line;
  for (int r = 0; r < 4; ++r)
    for (int m = 0; m < 3; ++m) {
      const uint64_t occ = st_stale_check[r][m][0].exchange(0), vis = st_stale_check[r][m][1].exchange(0);
      if (occ + vis)
        line += fmt::format("; {} {}: {} still occluded / {} visible ({:.1f} % visible)", kRot[r], kMove[m], occ, vis,
                            100.0 * double(vis) / double(occ + vis));
    }
  REXLOG_INFO("[occlusion] Umbra stale occluded counts (older than latent_frames) against the box drawn at that read, "
              "by camera change since the count{}", line.empty() ? std::string(": none") : line);
}

// ---- me2_occlusion_umbra_cpu (2026-10-28): Umbra's tests answered on the CPU in the same frame ----------------------
// The latent answers lag one Umbra test of the node behind, and Umbra re-tests a node it was told is occluded only every
// 10-110 frames: an old 'occluded' is wrong after the camera moved, and dropping it showed occluded objects (a door's
// interface prompt blinking behind cover). Here, as engines do with a coverage buffer: the GPU reduces each frame's
// scene depth to a 128x72 grid (renut draw.cpp OcclusionDepthCaptureLocked, the farthest depth of each cell), the
// newest finished grid is reprojected at the start of Umbra's frame (sub_8274AA38) into the CURRENT camera, and each
// test box (opcode 131) is projected with the current view-projection: occluded when every grid cell under it holds an
// occluder nearer than the box's nearest point. Cells the reprojection did not reach count as 'no occluder'. Answers
// are current-camera ones, so moving / turning does not make them stale; only moving occluders lag (1-2 frames).
// 1 = compare only (the latent answers stay; the CPU answer of each test is counted against its GPU count),
// 2 = the CPU answers Umbra and the test box is not drawn (the latent path only while there is no grid yet).
REXCVAR_DEFINE_INT32(me2_occlusion_umbra_cpu, 0, "ME2",
                     "Umbra's occlusion tests on the CPU against the reprojected scene depth of an earlier frame: "
                     "0 = off, 1 = compare with the GPU counts only, 2 = answer Umbra (no test boxes drawn)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// PC grid dump 20:18: the scene depth holds 0.0015-0.1 where the projection (near 10, far 10000) gives 0.9-0.999, i.e.
// 1 - z: UE3 on the Xbox draws with the viewport's depth range flipped (the matrix itself is the usual one).
REXCVAR_DEFINE_BOOL(me2_occlusion_cpu_reversed, true, "ME2",
                    "me2_occlusion_umbra_cpu: the host depth runs 1 (near) -> 0 (far) instead of 0 -> 1 (ME2: true)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_cpu_bias, 0.0, "ME2",
                      "me2_occlusion_umbra_cpu: a box is occluded only if the occluders are nearer than its nearest point "
                      "by more than this (normalized depth)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_cpu_max_age, 6, "ME2",
                     "me2_occlusion_umbra_cpu: a depth grid older than this many frames is not used (tests answered "
                     "as without the CPU path)")
    .range(1, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(me2_occlusion_cpu_oldview, false, "ME2",
                    "me2_occlusion_umbra_cpu: test each box in the depth grid's own frame (its camera 2-3 frames ago) "
                    "instead of reprojecting the grid into the current frame every frame (console: ~2 ms a frame on "
                    "the UE3 render thread). No holes; the answer is as old as the grid")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_cpu_grid, 1, "ME2",
                     "me2_occlusion_umbra_cpu: the depth grid: 0 = 64x36 cells (1/4 the reprojection cost, coarser), "
                     "1 = 128x72, 2 = 256x144 (finer edges, ~4x the reprojection cost)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// PC compare 20:32 (256x144): most tests the GPU found occluded and the CPU not had a cell without data under the box
// (5-22 thousand / 10 s; an occluder cell too far 0.3-3.5 thousand, near plane 1-2 thousand). Clear cells (the sky:
// nothing drawn) are reprojected too, as a point ~100000 units away, so they are known 'no occluder' rather than
// holes; then a hole with at least 5 of its 8 neighbours known takes their farthest depth, this many passes.
REXCVAR_DEFINE_INT32(me2_occlusion_cpu_fill, 2, "ME2",
                     "me2_occlusion_umbra_cpu: passes filling reprojection holes from their neighbours (0 = none)")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-28 20:4x (user: the moon vanished when looked at): a sky dome that writes depth is an 'occluder' to the full
// scene depth (the GPU's latent tests ran against the prepass, which has no sky dome). Grid depths farther than this are
// treated as nothing drawn. The projection is infinite-far: normalized depth = 0.999 - 9.99 / distance.
REXCVAR_DEFINE_DOUBLE(me2_occlusion_cpu_occluder_max, 30000.0, "ME2",
                      "me2_occlusion_umbra_cpu: occluders farther than this (world units) are ignored (sky domes, "
                      "backdrops)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_cpu_capture, 1, "ME2",
                     "me2_occlusion_umbra_cpu: which full-size depth resolve of a frame makes the grid: 1 = the first "
                     "(after UE3's depth prepass: static occluders, no characters), N = the N-th, 0 = the frame's last")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-28 21:1x (flip trace + offline check of a grid): turning with the stick held, the camera turns so far in the
// grid's 2-3 frames that a band ~1/5 of the screen wide at the leading edge was never seen; Umbra's node boxes are large
// and reach into it, so a node whose seen part is all behind walls got 'visible' (unknown) and its objects in the middle
// (a door's prompt) were drawn. With this on, such a box is occluded when every cell seen is occluded and they are at
// least half of it. Cost: an object first entering the screen at its edge during a fast turn shows 2-3 frames late.
REXCVAR_DEFINE_BOOL(me2_occlusion_cpu_unseen, true, "ME2",
                    "me2_occlusion_umbra_cpu: a box whose seen cells are all occluded (at least half of it) counts as "
                    "occluded although part of it was off the earlier frame's screen")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_cpu_hold, 4, "ME2",
                     "me2_occlusion_umbra_cpu: a test the grid cannot decide (hole, near plane) keeps the CPU's own "
                     "'occluded' for its node from up to this many frames before (0 = off)")
    .range(0, 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-28 21:2x (flip trace): 210 of the occluded -> visible changes came after a single 'occluded' frame (noise at
// the grid's resolution), while Umbra keeps a node it was told is visible for ~10 frames (p90 49) before asking again:
// one noisy frame = a third of a second of a prompt behind a wall. A node the CPU found occluded within the last 2 frames
// becomes visible only after this many further 'visible' tests in a row (Umbra re-tests such a node every frame, so a
// real reveal is late by this many frames).
REXCVAR_DEFINE_INT32(me2_occlusion_cpu_debounce, 1, "ME2",
                     "me2_occlusion_umbra_cpu: frames a node the CPU just found occluded keeps that answer against a "
                     "new 'visible' (0 = off)")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_cpu_unseen_min, 0.0, "ME2",
                      "me2_occlusion_cpu_unseen: the share of a box's cells (0..1) that must have been seen, all "
                      "occluded, for it to count as occluded; 0 = any one seen cell (a box seen nowhere stays visible); "
                      "0.5 = the 21:14 rule")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_cpu_diag, false, "ME2",
                    "me2_occlusion_umbra_cpu diagnostic: every 300 frames log the grid's depth percentiles and write it "
                    "to occlusion_grid.bin (next to the executable's working directory), and log some tests in detail")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

int CpuMode() {
  static const int mode = UmbraLatent() ? REXCVAR_GET(me2_occlusion_umbra_cpu) : 0;
  return mode;
}

// Mode 2: the CPU answers Umbra (no box drawn for a decided test). The draw-count rotation's "raster" cut (2026-11-05,
// me2_ab_cycle = "raster") turns the CPU side OFF for its slot: no rasterizer walk / drawing / tests, boxes drawn, the
// latent GPU answers used = the plain GPU path (mode 0) with its own cost (the first console A/B kept the CPU tests
// running in the GPU slots: ~7 ms a frame on the UE3 render thread counted against the GPU path). Latched once a view
// before Umbra's traversal (sub_8274AA38 hook), so a test and its read in that traversal agree.
std::atomic<bool> g_cpu_slot_on{true};
bool CpuSlotOn() { return g_cpu_slot_on.load(std::memory_order_relaxed); }
bool CpuAnswers() { return CpuMode() == 2 && CpuSlotOn(); }
std::atomic<uint64_t> g_raster_view_frame{~uint64_t(0)};  // the frame whose Umbra view the rasterizer answers

// me2-nx-deko (2026-11-05): where the CPU tests' depth comes from. 0 = the GPU's scene depth of an earlier frame,
// reprojected (Oct 28; holes where that frame did not look); 1 = the occlusion rasterizer (me2_occlusion_raster.cpp:
// the static depth draw lists' meshes drawn on the CPU with this frame's camera before Umbra's traversal).
REXCVAR_DEFINE_INT32(me2_occlusion_cpu_source, 0, "ME2",
                     "me2_occlusion_umbra_cpu: 0 = the reprojected GPU depth of an earlier frame, 1 = the occlusion "
                     "rasterizer (occluders drawn on the CPU with the current camera)")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
int CpuSource() {
  static const int source = REXCVAR_GET(me2_occlusion_cpu_source);
  return source;
}
thread_local uint8_t t_raster_alt = 0;  // me2_occlusion_raster_dual: the other winding's answer to the last test
uint8_t g_raster_alt[kIndexKeys];       // ... per record index (opcode 131 -> its END)
uint32_t g_raster_token[kIndexKeys];    // me2_occlusion_raster_trace: the record of the test's occluded answer

// 4x4 inverse (row-major); false if singular.
bool Invert4(const float* m, float* out) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-30f)
    return false;
  for (int i = 0; i < 16; ++i)
    out[i] = inv[i] / det;
  return true;
}
void Mul4(const float* a, const float* b, float* out) {  // out = a * b
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] +
                       a[r * 4 + 3] * b[3 * 4 + c];
}

struct CpuOcclusion {
  float vp[16] = {};  // this frame's view-projection (sub_8274AA38 view +256)
  uint64_t vp_frame = ~uint64_t(0);
  bool ready = false;  // 'buf' holds this frame's reprojected grid
  uint32_t w = 0, h = 0;
  std::vector<float> buf;  // per cell: the farthest occluder depth in the current view, < 0 = none
  std::vector<float> corner;  // (w+1)*(h+1) x 4: the grid corners' x * M0 + y * M1 + M3
  // 2026-10-28 22:5x (console: the tests cost 2-11 ms a frame on the UE3 render thread): per 8x8 cells the farthest
  // value of 'buf', +inf where a cell has no data. A box whose tiles are all nearer than it is occluded at once.
  uint32_t tw = 0, th = 0;
  std::vector<float> tile_max;
  std::vector<float> old_tile_max;  // me2_occlusion_cpu_oldview: the same over the grid itself (its own frame)
  std::shared_ptr<const renut::gpu::OcclusionDepthGrid> grid;
  float back[16] = {};  // current clip -> the grid's clip (inv(vp) * grid vp): a hole cell looked up in the old frame
  bool back_ok = false;
} g_cpu;
struct CpuAnswer {
  uint32_t tag = 0;  // index + 1
  uint64_t frame = 0;
  uint8_t answer = 0;  // 1 visible, 2 occluded
};
CpuAnswer g_cpu_answers[kIndexKeys];
thread_local uint8_t t_cpu_answer = 0;  // opcode 130's record: the CPU's answer for GetData (mode 2), 0 = none
// me2_occlusion_umbra_flip_trace: what the CPU test of each record index saw (logged when its answer flips).
struct CpuTestInfo {
  int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
  float min_z = 0.0f, lo = 0.0f, hi = 0.0f;  // the box's nearest depth; the grid cells' depth range under it
  int holes = 0, verts = 0;
  uint8_t code = 0;
};
thread_local CpuTestInfo t_cpu_info;
CpuTestInfo g_cpu_info[kIndexKeys];
thread_local uint32_t t_cpu_index = UINT32_MAX;  // opcode 130's record index
Me2OcclusionBounds CurrentLatentBounds() { return IndexBounds(t_cpu_index); }
std::atomic<uint64_t> st_cpu_tests{0}, st_cpu_occluded{0}, st_cpu_nogrid{0}, st_cpu_offscreen{0}, st_cpu_near{0},
    st_cpu_builds{0}, st_cpu_build_ns{0}, st_cpu_grid_age{0}, st_cpu_cells_filled{0}, st_cpu_answered{0};
std::atomic<uint64_t> st_cpu_sky_far{0}, st_cpu_sky_near{0}, st_cpu_grid_cells{0};
std::atomic<uint64_t> st_cpu_test_ns{0};  // the tests' own time on the UE3 render thread
// Mode 2 reads (opcode 130): answered from a CPU test of an earlier frame / left to the latent path (of them: the
// index's CPU answer was more than 8 frames old).
std::atomic<uint64_t> st_cpu_late_reads{0}, st_cpu_latent_reads{0}, st_cpu_stale_reads{0};
std::atomic<uint64_t> st_cpu_old_occ{0}, st_cpu_old_vis{0};  // holes decided in the grid's own frame
std::atomic<uint64_t> st_cpu_unseen{0};  // me2_occlusion_cpu_unseen: occluded with a part never seen
std::atomic<uint64_t> st_cpu_held{0};    // me2_occlusion_cpu_hold: undecided tests kept occluded
std::atomic<uint64_t> st_cpu_debounced{0};  // me2_occlusion_cpu_debounce: 'visible' held back a frame

constexpr float kSkyDepth = 2.0f;  // a grid cell where nothing was drawn: behind everything, occludes nothing
// At the start of Umbra's frame (sub_8274AA38): the newest grid reprojected into this frame's camera.
void CpuBuildFrame(const float* vp, uint64_t frame) {
  std::memcpy(g_cpu.vp, vp, sizeof(g_cpu.vp));
  g_cpu.vp_frame = frame;
  g_cpu.ready = false;
  static const bool reversed = REXCVAR_GET(me2_occlusion_cpu_reversed);
  static const uint64_t max_age = uint64_t(REXCVAR_GET(me2_occlusion_cpu_max_age));
  auto grid = renut::gpu::OcclusionDepthLatest();
  if (!grid || grid->cells.empty() || frame < grid->frame || frame - grid->frame > max_age)
    return;
  const auto start = std::chrono::steady_clock::now();
  float inv_old[16], m[16];
  if (!Invert4(grid->view_projection, inv_old))
    return;
  Mul4(inv_old, vp, m);  // old clip -> current clip (row vectors)
  float inv_cur[16];
  g_cpu.back_ok = Invert4(vp, inv_cur);
  if (g_cpu.back_ok)
    Mul4(inv_cur, grid->view_projection, g_cpu.back);
  const uint32_t w = grid->width, h = grid->height;
  static const bool oldview = REXCVAR_GET(me2_occlusion_cpu_oldview);
  if (oldview) {
    // No reprojection: the boxes are projected with the grid's own camera (CpuTestOldView). Per 8x8 cells the
    // farthest occluder depth of the grid, +inf for the sky / no occluder.
    if (g_cpu.grid != grid) {
      static const float occluder_max_z =
          0.999f - 9.99f / float(std::max(20.0, REXCVAR_GET(me2_occlusion_cpu_occluder_max)));
      const float zmax = std::min(0.9989f, occluder_max_z);
      g_cpu.tw = (w + 7) / 8;
      g_cpu.th = (h + 7) / 8;
      g_cpu.old_tile_max.assign(size_t(g_cpu.tw) * g_cpu.th, -1.0f);
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const float* cell = &grid->cells[(size_t(y) * w + x) * 2];
          const float z = reversed ? 1.0f - cell[0] : cell[1];
          float& t = g_cpu.old_tile_max[size_t(y / 8) * g_cpu.tw + x / 8];
          t = std::max(t, (z > 0.0f && z < zmax) ? z : 1e30f);
        }
      g_cpu.grid = grid;
    }
    g_cpu.w = w;
    g_cpu.h = h;
    g_cpu.ready = true;
    st_cpu_builds.fetch_add(1, std::memory_order_relaxed);
    st_cpu_grid_age.fetch_add(frame - grid->frame, std::memory_order_relaxed);
    st_cpu_build_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - start).count()),
                              std::memory_order_relaxed);
    return;
  }
  if (g_cpu.grid != grid) {
    // Depth statistics of a new grid (the convention check): cells whose farthest is the far plane / whose nearest
    // is 0.
    uint64_t far_cells = 0, near_cells = 0;
    for (uint32_t i = 0; i < w * h; ++i) {
      far_cells += grid->cells[2 * i + 1] >= 0.99999f;
      near_cells += grid->cells[2 * i] <= 0.00001f;
    }
    st_cpu_sky_far.fetch_add(far_cells, std::memory_order_relaxed);
    st_cpu_sky_near.fetch_add(near_cells, std::memory_order_relaxed);
    st_cpu_grid_cells.fetch_add(w * h, std::memory_order_relaxed);
    g_cpu.grid = grid;
    static const bool diag = REXCVAR_GET(me2_occlusion_cpu_diag);
    static uint64_t diag_window = ~uint64_t(0);
    if (diag && frame / 300 != diag_window) {
      diag_window = frame / 300;
      std::vector<float> nearest(w * h), farthest(w * h);
      for (uint32_t i = 0; i < w * h; ++i) {
        nearest[i] = grid->cells[2 * i];
        farthest[i] = grid->cells[2 * i + 1];
      }
      std::sort(nearest.begin(), nearest.end());
      std::sort(farthest.begin(), farthest.end());
      auto pct = [&](const std::vector<float>& v) {
        const size_t n = v.size();
        return fmt::format("{:.6g} / {:.6g} / {:.6g} / {:.6g} / {:.6g} / {:.6g} / {:.6g}", v[0], v[n / 100], v[n / 10],
                           v[n / 2], v[n * 9 / 10], v[n * 99 / 100], v[n - 1]);
      };
      REXLOG_INFO("[occlusion-cpu] grid of frame {} (now {}): nearest min/1/10/50/90/99 %/max {}; farthest {}; vp row 3 "
                  "{:.4g} {:.4g} {:.4g} {:.4g}", grid->frame, frame, pct(nearest), pct(farthest),
                  grid->view_projection[12], grid->view_projection[13], grid->view_projection[14],
                  grid->view_projection[15]);
      if (FILE* f = std::fopen("occlusion_grid.bin", "wb")) {
        const uint32_t head[2] = {w, h};
        std::fwrite(head, 4, 2, f);
        std::fwrite(grid->view_projection, 4, 16, f);
        std::fwrite(vp, 4, 16, f);
        std::fwrite(grid->cells.data(), 4, grid->cells.size(), f);
        std::fclose(f);
      }
    }
  }
  g_cpu.w = w;
  g_cpu.h = h;
  g_cpu.buf.assign(size_t(w) * h, -1.0f);
  g_cpu.corner.resize(size_t(w + 1) * (h + 1) * 4);
  for (uint32_t j = 0; j <= h; ++j) {
    const float y = 1.0f - 2.0f * float(j) / float(h);
    for (uint32_t i = 0; i <= w; ++i) {
      const float x = 2.0f * float(i) / float(w) - 1.0f;
      float* c = &g_cpu.corner[(size_t(j) * (w + 1) + i) * 4];
      for (int k = 0; k < 4; ++k)
        c[k] = x * m[0 * 4 + k] + y * m[1 * 4 + k] + m[3 * 4 + k];
    }
  }
  uint64_t filled = 0;
  for (uint32_t j = 0; j < h; ++j) {
    for (uint32_t i = 0; i < w; ++i) {
      const float* cell = &grid->cells[(size_t(j) * w + i) * 2];
      float z = reversed ? 1.0f - cell[0] : cell[1];  // the farthest point of the cell, as normalized depth
      // Nothing drawn there (a clear: the sky): placed as a point ~100000 units away, but written as kSkyDepth, which
      // occludes nothing at any distance. 2026-10-28 20:4x (user): written as that point's depth, the moon and the main
      // menu's / save screen's backdrops (farther than 100000 units) were 'occluded' by the sky.
      static const float occluder_max_z =
          0.999f - 9.99f / float(std::max(20.0, REXCVAR_GET(me2_occlusion_cpu_occluder_max)));
      const bool sky = !(z > 0.0f && z < std::min(0.9989f, occluder_max_z));
      if (sky)
        z = 0.9989f;
      float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f, max_z = -1e30f;
      bool behind = false;
      const uint32_t corners[4][2] = {{i, j}, {i + 1, j}, {i, j + 1}, {i + 1, j + 1}};
      for (const auto& cc : corners) {
        const float* b = &g_cpu.corner[(size_t(cc[1]) * (w + 1) + cc[0]) * 4];
        const float qx = b[0] + z * m[2 * 4 + 0], qy = b[1] + z * m[2 * 4 + 1];
        const float qz = b[2] + z * m[2 * 4 + 2], qw = b[3] + z * m[2 * 4 + 3];
        if (qw <= 1e-3f) {
          behind = true;
          break;
        }
        const float iw = 1.0f / qw;
        const float gx = (qx * iw + 1.0f) * 0.5f * float(w), gy = (1.0f - qy * iw) * 0.5f * float(h);
        min_x = std::min(min_x, gx);
        max_x = std::max(max_x, gx);
        min_y = std::min(min_y, gy);
        max_y = std::max(max_y, gy);
        max_z = std::max(max_z, qz * iw);
      }
      if (behind || max_z >= 1.0f || max_z <= 0.0f)
        continue;
      // The cells whose centres the reprojected cell covers; a cell grown past 8 cells (the camera came very close,
      // or a grazing angle) is left out rather than smeared.
      const int x0 = std::max(0, int(std::ceil(min_x - 0.5f))), x1 = std::min(int(w) - 1, int(std::floor(max_x - 0.5f)));
      const int y0 = std::max(0, int(std::ceil(min_y - 0.5f))), y1 = std::min(int(h) - 1, int(std::floor(max_y - 0.5f)));
      if (x1 < x0 || y1 < y0 || x1 - x0 > 8 || y1 - y0 > 8)
        continue;
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          float& v = g_cpu.buf[size_t(y) * w + x];
          if (v < 0.0f)
            ++filled;
          v = std::max(v, sky ? kSkyDepth : max_z);
        }
    }
  }
  // Holes (cells nothing was reprojected into) with at least 5 known neighbours: their farthest depth.
  static const int fill_passes = REXCVAR_GET(me2_occlusion_cpu_fill);
  static std::vector<float> next;
  for (int pass = 0; pass < fill_passes; ++pass) {
    next = g_cpu.buf;
    bool any = false;
    for (int y = 0; y < int(h); ++y)
      for (int x = 0; x < int(w); ++x) {
        if (g_cpu.buf[size_t(y) * w + x] >= 0.0f)
          continue;
        int known = 0;
        float farthest = -1.0f;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= int(w) || ny >= int(h))
              continue;
            const float v = g_cpu.buf[size_t(ny) * w + nx];
            if (v >= 0.0f) {
              ++known;
              farthest = std::max(farthest, v);
            }
          }
        if (known >= 5) {
          next[size_t(y) * w + x] = farthest;
          ++filled;
          any = true;
        }
      }
    g_cpu.buf.swap(next);
    if (!any)
      break;
  }
  g_cpu.tw = (w + 7) / 8;
  g_cpu.th = (h + 7) / 8;
  g_cpu.tile_max.assign(size_t(g_cpu.tw) * g_cpu.th, -1.0f);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const float v = g_cpu.buf[size_t(y) * w + x];
      float& t = g_cpu.tile_max[size_t(y / 8) * g_cpu.tw + x / 8];
      t = std::max(t, v < 0.0f ? 1e30f : v);
    }
  g_cpu.ready = true;
  st_cpu_builds.fetch_add(1, std::memory_order_relaxed);
  st_cpu_cells_filled.fetch_add(filled, std::memory_order_relaxed);
  st_cpu_grid_age.fetch_add(frame - grid->frame, std::memory_order_relaxed);
  st_cpu_build_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start).count()),
                            std::memory_order_relaxed);
}

// 2026-10-28 20:5x (user: prompts still flash when turning / walking; mode-2 reads left to the latent path were all
// reprojection holes): a box over a hole is tested in the grid's OWN frame instead, its camera 2-3 frames ago, against
// that grid as it was (no holes): 7 occluded / 8 visible there, 0 off that frame's screen or at its near plane.
uint8_t CpuTestOldView(const uint8_t* base, uint32_t verts) {
  const auto& grid = g_cpu.grid;
  if (!grid)
    return 0;
  static const bool reversed = REXCVAR_GET(me2_occlusion_cpu_reversed);
  static const float bias = float(REXCVAR_GET(me2_occlusion_cpu_bias));
  static const float occluder_max_z =
      0.999f - 9.99f / float(std::max(20.0, REXCVAR_GET(me2_occlusion_cpu_occluder_max)));
  const float* m = grid->view_projection;
  const uint32_t w = grid->width, h = grid->height;
  float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f, min_z = 1e30f;
  for (uint32_t v = 0; v < verts; ++v) {
    float p[3];
    for (uint32_t c = 0; c < 3; ++c) {
      const uint32_t bits = LoadBE(base, kUmbraVertices + 12 * v + 4 * c);
      std::memcpy(&p[c], &bits, 4);
    }
    const float qx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    const float qy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    const float qz = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
    const float qw = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
    if (qw <= 1.0f)
      return 0;
    const float iw = 1.0f / qw;
    const float gx = (qx * iw + 1.0f) * 0.5f * float(w), gy = (1.0f - qy * iw) * 0.5f * float(h);
    min_x = std::min(min_x, gx);
    max_x = std::max(max_x, gx);
    min_y = std::min(min_y, gy);
    max_y = std::max(max_y, gy);
    min_z = std::min(min_z, qz * iw);
  }
  // Only a box wholly on that frame's screen: what lay beyond its edge was never seen.
  if (min_x < 0.0f || min_y < 0.0f || max_x >= float(w) || max_y >= float(h))
    return 0;
  const int x0 = int(min_x), x1 = int(max_x), y0 = int(min_y), y1 = int(max_y);
  static const bool oldview = REXCVAR_GET(me2_occlusion_cpu_oldview);
  if (oldview && g_cpu.old_tile_max.size() == size_t(g_cpu.tw) * g_cpu.th && g_cpu.tw == (w + 7) / 8) {
    bool all = true;  // every tile nearer than the box: the cell loop below would answer occluded too
    for (int ty = y0 / 8; all && ty <= y1 / 8; ++ty)
      for (int tx = x0 / 8; tx <= x1 / 8; ++tx)
        if (!(g_cpu.old_tile_max[size_t(ty) * g_cpu.tw + tx] < min_z - bias)) {
          all = false;
          break;
        }
    if (all)
      return 7;
  }
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float* cell = &grid->cells[(size_t(y) * w + x) * 2];
      const float z = reversed ? 1.0f - cell[0] : cell[1];
      if (!(z > 0.0f && z < std::min(0.9989f, occluder_max_z)) || z >= min_z - bias)
        return 8;
    }
  return 7;
}

// 2026-10-28 21:0x: a hole cell of the current grid, at normalized depth z (the box's nearest), looked up in the grid's
// own frame: 2 occluded there, 1 visible there, 0 not on that frame's screen (or behind its camera).
uint8_t OldCell(int x, int y, float z) {
  if (!g_cpu.back_ok || !g_cpu.grid)
    return 0;
  static const bool reversed = REXCVAR_GET(me2_occlusion_cpu_reversed);
  static const float bias = float(REXCVAR_GET(me2_occlusion_cpu_bias));
  static const float occluder_max_z =
      0.999f - 9.99f / float(std::max(20.0, REXCVAR_GET(me2_occlusion_cpu_occluder_max)));
  const float cx = (float(x) + 0.5f) / float(g_cpu.w) * 2.0f - 1.0f;
  const float cy = 1.0f - (float(y) + 0.5f) / float(g_cpu.h) * 2.0f;
  const float* b = g_cpu.back;
  const float qx = cx * b[0] + cy * b[4] + z * b[8] + b[12];
  const float qy = cx * b[1] + cy * b[5] + z * b[9] + b[13];
  const float qz = cx * b[2] + cy * b[6] + z * b[10] + b[14];
  const float qw = cx * b[3] + cy * b[7] + z * b[11] + b[15];
  if (qw <= 1e-6f)
    return 0;
  const float iw = 1.0f / qw;
  const auto& grid = g_cpu.grid;
  const float gx = (qx * iw + 1.0f) * 0.5f * float(grid->width), gy = (1.0f - qy * iw) * 0.5f * float(grid->height);
  if (!(gx >= 0.0f && gy >= 0.0f && gx < float(grid->width) && gy < float(grid->height)))
    return 0;
  const float* cell = &grid->cells[(size_t(gy) * grid->width + size_t(gx)) * 2];
  const float old_z = reversed ? 1.0f - cell[0] : cell[1];
  if (!(old_z > 0.0f && old_z < std::min(0.9989f, occluder_max_z)))
    return 1;  // the sky there: nothing in front
  return old_z < qz * iw - bias ? 2 : 1;
}

// 2026-10-28 21:1x: the box's silhouette on the grid (convex hull of its projected vertices). Its screen rectangle is
// much larger for a box seen at an angle; the rectangle's corners reached background cells and answered 'visible' where
// the GPU, drawing the faces, saw nothing (371 prompt flips in one run).
struct Hull {
  float x[64], y[64];
  int n = 0;
  float cx = 0.0f, cy = 0.0f;  // a point inside
};
float Cross(float ax, float ay, float bx, float by, float px, float py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}
// Monotone chain over the projected points; false (use the rectangle) when degenerate or too many points.
bool BuildHull(const std::vector<std::pair<float, float>>& in, Hull& h) {
  if (in.size() < 3 || in.size() > 4096)
    return false;
  static thread_local std::vector<std::pair<float, float>> p, out;
  p = in;
  std::sort(p.begin(), p.end());
  out.assign(2 * p.size(), {});
  size_t k = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    while (k >= 2 && Cross(out[k - 2].first, out[k - 2].second, out[k - 1].first, out[k - 1].second, p[i].first,
                           p[i].second) <= 0.0f)
      --k;
    out[k++] = p[i];
  }
  for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
    while (k >= t && Cross(out[k - 2].first, out[k - 2].second, out[k - 1].first, out[k - 1].second, p[i - 1].first,
                           p[i - 1].second) <= 0.0f)
      --k;
    out[k++] = p[i - 1];
  }
  const int n = int(k) - 1;
  if (n < 3 || n > 64)
    return false;
  h.n = n;
  h.cx = h.cy = 0.0f;
  for (int i = 0; i < n; ++i) {
    h.x[i] = out[i].first;
    h.y[i] = out[i].second;
    h.cx += h.x[i];
    h.cy += h.y[i];
  }
  h.cx /= float(n);
  h.cy /= float(n);
  return true;
}
// Whether the cell [x, x+1] x [y, y+1] touches the hull (separating-axis test on the hull's edges).
[[maybe_unused]] bool CellIn(const Hull& h, int x, int y) {
  const float rx[4] = {float(x), float(x + 1), float(x), float(x + 1)};
  const float ry[4] = {float(y), float(y), float(y + 1), float(y + 1)};
  for (int i = 0; i < h.n; ++i) {
    const int j = (i + 1) % h.n;
    const float inside = Cross(h.x[i], h.y[i], h.x[j], h.y[j], h.cx, h.cy);
    bool all_out = true;
    for (int c = 0; c < 4 && all_out; ++c) {
      const float side = Cross(h.x[i], h.y[i], h.x[j], h.y[j], rx[c], ry[c]);
      if (side * inside >= 0.0f)
        all_out = false;
    }
    if (all_out)
      return false;
  }
  return true;
}

// Opcode 131's test geometry (in Umbra's buffers) against this frame's grid: 2 occluded, 0 no grid, else visible and
// why: 1 an occluder cell too far (or a bad box), 3 at the near plane, 4 off the screen, 5 a cell without data (and
// the old frame could not tell either); a hole decided in the grid's own frame: 7 occluded, 8 visible.
uint8_t CpuTest(const uint8_t* base, uint32_t rec, uint64_t frame) {
  if (CpuSource() == 1) {
    // me2_occlusion_cpu_source 1: against this frame's rasterized occluders (no holes, no old frames).
    t_cpu_info = {};
    const uint32_t verts = LoadBE(base, rec + 16);
    t_cpu_info.verts = int(verts);
    const uint8_t answer =
        Me2RasterTest(base, kUmbraVertices, verts, kUmbraIndices, LoadBE(base, rec + 12), frame, &t_raster_alt);
    switch (answer) {
      case 0: st_cpu_nogrid.fetch_add(1, std::memory_order_relaxed); break;
      case 2: st_cpu_occluded.fetch_add(1, std::memory_order_relaxed); break;
      case 3: st_cpu_near.fetch_add(1, std::memory_order_relaxed); break;
      case 4: st_cpu_offscreen.fetch_add(1, std::memory_order_relaxed); break;
      default: break;
    }
    if (answer)
      st_cpu_tests.fetch_add(1, std::memory_order_relaxed);
    return answer;
  }
  if (!g_cpu.ready || g_cpu.vp_frame != frame) {
    st_cpu_nogrid.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  st_cpu_tests.fetch_add(1, std::memory_order_relaxed);
  static const float bias = float(REXCVAR_GET(me2_occlusion_cpu_bias));
  const uint32_t verts = LoadBE(base, rec + 16);
  t_cpu_info = {};
  t_cpu_info.verts = int(verts);
  if (verts == 0 || verts > 4096)
    return 1;
  static const bool oldview = REXCVAR_GET(me2_occlusion_cpu_oldview);
  if (oldview) {
    const uint8_t old = CpuTestOldView(base, verts);
    if (old == 7) {
      st_cpu_occluded.fetch_add(1, std::memory_order_relaxed);
      st_cpu_old_occ.fetch_add(1, std::memory_order_relaxed);
      return 7;
    }
    if (old == 8) {
      st_cpu_old_vis.fetch_add(1, std::memory_order_relaxed);
      return 8;
    }
    return 5;  // off that frame's screen / at its near plane: the box is drawn, the latent answer decides
  }
  const float* m = g_cpu.vp;
  float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f, min_z = 1e30f;
  static thread_local std::vector<std::pair<float, float>> projected;
  projected.clear();
  for (uint32_t v = 0; v < verts; ++v) {
    float p[3];
    for (uint32_t c = 0; c < 3; ++c) {
      const uint32_t bits = LoadBE(base, kUmbraVertices + 12 * v + 4 * c);
      std::memcpy(&p[c], &bits, 4);
    }
    const float qx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    const float qy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    const float qz = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
    const float qw = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
    if (qw <= 1.0f) {  // at or behind the near plane: the camera is inside / at the box
      st_cpu_near.fetch_add(1, std::memory_order_relaxed);
      return 3;
    }
    const float iw = 1.0f / qw;
    const float gx = (qx * iw + 1.0f) * 0.5f * float(g_cpu.w), gy = (1.0f - qy * iw) * 0.5f * float(g_cpu.h);
    projected.emplace_back(gx, gy);
    min_x = std::min(min_x, gx);
    max_x = std::max(max_x, gx);
    min_y = std::min(min_y, gy);
    max_y = std::max(max_y, gy);
    min_z = std::min(min_z, qz * iw);
  }
  const int x0 = std::max(0, int(std::floor(min_x))), x1 = std::min(int(g_cpu.w) - 1, int(std::floor(max_x)));
  const int y0 = std::max(0, int(std::floor(min_y))), y1 = std::min(int(g_cpu.h) - 1, int(std::floor(max_y)));
  if (x1 < x0 || y1 < y0) {
    st_cpu_offscreen.fetch_add(1, std::memory_order_relaxed);
    return 4;  // off the screen: Umbra's own frustum test decides
  }
  static const bool flip_trace = REXCVAR_GET(me2_occlusion_umbra_flip_trace);
  if (flip_trace) {
    CpuTestInfo& info = t_cpu_info;
    info.x0 = x0;
    info.x1 = x1;
    info.y0 = y0;
    info.y1 = y1;
    info.min_z = min_z;
    info.lo = 1e30f;
    info.hi = -1e30f;
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const float v = g_cpu.buf[size_t(y) * g_cpu.w + x];
        if (v < 0.0f) {
          ++info.holes;
        } else {
          info.lo = std::min(info.lo, v);
          info.hi = std::max(info.hi, v);
        }
      }
  }
  static const bool diag = REXCVAR_GET(me2_occlusion_cpu_diag);
  static uint64_t diag_n = 0;
  if (diag && (++diag_n % 3000) == 0) {
    float lo = 1e30f, hi = -1e30f;
    int none = 0;
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const float v = g_cpu.buf[size_t(y) * g_cpu.w + x];
        if (v < 0.0f)
          ++none;
        else {
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
      }
    REXLOG_INFO("[occlusion-cpu] test: {} verts, cells x {}..{} y {}..{} ({:.2f}..{:.2f}, {:.2f}..{:.2f}), box nearest "
                "{:.6g}; cells: {} without data, depth {:.6g}..{:.6g}", verts, x0, x1, y0, y1, min_x, max_x, min_y,
                max_y, min_z, none, lo, hi);
  }
  // Each cell: the reprojected grid; a hole: the same cell looked up in the grid's own frame (OldCell). Any cell that
  // shows the box decides 'visible'; a hole that frame never saw leaves the box to the whole-box old-frame test.
  // Every tile under the rectangle nearer than the box, no cell without data: occluded (the cell loop below would
  // find the same: no cell at or behind the box, no hole).
  {
    const float limit = min_z - bias;
    bool all = true;
    for (int ty = y0 / 8; all && ty <= y1 / 8; ++ty)
      for (int tx = x0 / 8; tx <= x1 / 8; ++tx)
        if (!(g_cpu.tile_max[size_t(ty) * g_cpu.tw + tx] < limit)) {
          all = false;
          break;
        }
    if (all) {
      st_cpu_occluded.fetch_add(1, std::memory_order_relaxed);
      return 2;
    }
  }
  bool used_old = false, unknown = false;
  int seen = 0, unseen = 0;
  Hull hull;
  const bool use_hull = BuildHull(projected, hull);
  for (int y = y0; y <= y1; ++y) {
    // The hull's x extent within the row's strip [y, y+1] (vertices inside it + edge crossings of its two lines): the
    // cells of the row that touch the hull, as CellIn decided cell by cell.
    int rx0 = x0, rx1 = x1;
    if (use_hull) {
      const float ya = float(y), yb = float(y + 1);
      float lo = 1e30f, hi = -1e30f;
      for (int i = 0; i < hull.n; ++i) {
        const int j = (i + 1) % hull.n;
        const float xi = hull.x[i], yi = hull.y[i], xj = hull.x[j], yj = hull.y[j];
        if (yi >= ya && yi <= yb) {
          lo = std::min(lo, xi);
          hi = std::max(hi, xi);
        }
        for (const float yl : {ya, yb})
          if ((yi - yl) * (yj - yl) < 0.0f) {
            const float xc = xi + (xj - xi) * (yl - yi) / (yj - yi);
            lo = std::min(lo, xc);
            hi = std::max(hi, xc);
          }
      }
      if (hi < lo)
        continue;
      rx0 = std::max(x0, int(std::ceil(lo)) - 1);
      rx1 = std::min(x1, int(std::floor(hi)));
    }
    for (int x = rx0; x <= rx1; ++x) {
      const float v = g_cpu.buf[size_t(y) * g_cpu.w + x];
      if (v >= 0.0f) {
        if (v >= min_z - bias)
          return 1;
        ++seen;
        continue;
      }
      const uint8_t old = OldCell(x, y, min_z);
      if (old == 1) {
        st_cpu_old_vis.fetch_add(1, std::memory_order_relaxed);
        return 8;
      }
      if (old == 2) {
        used_old = true;
        ++seen;
      } else {
        unknown = true;
        ++unseen;
      }
    }
  }
  static const bool assume_unseen = REXCVAR_GET(me2_occlusion_cpu_unseen);
  // 2026-10-28 21:3x (debounce + hold active, still 691 prompt flips from holes left to the latent path, which answers
  // 'visible'): the share of a box's cells that must have been seen, all occluded, for it to count as occluded.
  static const float min_seen = float(REXCVAR_GET(me2_occlusion_cpu_unseen_min));
  if (unknown && assume_unseen && seen > 0 && float(seen) >= min_seen * float(seen + unseen)) {
    st_cpu_occluded.fetch_add(1, std::memory_order_relaxed);
    st_cpu_unseen.fetch_add(1, std::memory_order_relaxed);
    return 9;
  }
  if (unknown) {
    const uint8_t old = CpuTestOldView(base, verts);
    if (old == 7) {
      st_cpu_occluded.fetch_add(1, std::memory_order_relaxed);
      st_cpu_old_occ.fetch_add(1, std::memory_order_relaxed);
    } else if (old == 8) {
      st_cpu_old_vis.fetch_add(1, std::memory_order_relaxed);
    }
    return old ? old : 5;
  }
  st_cpu_occluded.fetch_add(1, std::memory_order_relaxed);
  if (used_old) {
    st_cpu_old_occ.fetch_add(1, std::memory_order_relaxed);
    return 7;
  }
  return 2;
}

// CpuTest's answers the grid decides (the rest go to the latent path: no grid, a hole, the near plane).
bool CpuDecided(uint8_t answer) {
  return answer == 1 || answer == 2 || answer == 4 || answer >= 7;
}
bool CpuOccluded(uint8_t answer) {
  return answer == 2 || answer == 7 || answer == 9 || answer == 10 || answer == 11;
}

uint32_t CpuAnswered(uint8_t answer) {
  st_cpu_answered.fetch_add(1, std::memory_order_relaxed);
  return CpuOccluded(answer) ? 0u : kVisible;
}

// What the CPU test of opcode 130's record saw (the flip trace's tail).
std::string CpuInfoText() {
  if (!CpuMode() || t_cpu_index >= kIndexKeys)
    return {};
  const CpuTestInfo info = g_cpu_info[t_cpu_index];
  static const char* kCode[12] = {"no grid", "visible: a cell's depth behind the box", "occluded", "near plane",
                                  "off the screen", "hole", "?", "occluded (old frame)", "visible (old frame)",
                                  "occluded (part never seen)", "occluded (held)", "occluded (visible debounced)"};
  return fmt::format(
      "; CPU: {} - box cells x {}..{} y {}..{} of {}x{}, box nearest {:.6f}, cell depths {:.6f}..{:.6f}, {} holes, "
      "{} verts", kCode[std::min<int>(info.code, 11)], info.x0, info.x1, info.y0, info.y1, g_cpu.w, g_cpu.h,
      info.min_z, info.lo, info.hi, info.holes, info.verts);
}


// The CPU's answer to opcode 130's read, and (me2_occlusion_umbra_flip_trace) its flips with what the test saw.
uint32_t CpuAnsweredTraced(uint8_t answer) {
  static const bool trace = REXCVAR_GET(me2_occlusion_umbra_flip_trace);
  if (trace && t_latent_key) {
    const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
    TraceFlip(LatentFind(t_latent_key), t_latent_key, frame, !CpuOccluded(answer), Why::kCpu, 0, 0, CpuInfoText());
  }
  return CpuAnswered(answer);
}

void CpuReport() {
  if (!CpuMode())
    return;
  if (CpuSource() == 1) {
    Me2RasterReport();
    uint64_t alt[2][2];
    for (int a = 0; a < 2; ++a)
      for (int b = 0; b < 2; ++b)
        alt[a][b] = st_raster_alt_cmp[a][b].exchange(0);
    if (alt[0][0] + alt[0][1] + alt[1][0] + alt[1][1])
      REXLOG_INFO("[occlusion] raster winding (the OTHER winding, me2_occlusion_raster_dual) against the GPU counts: "
                  "occluded / GPU occluded {}, occluded / GPU VISIBLE {}, visible / GPU occluded {}, both visible {}",
                  alt[1][0], alt[1][1], alt[0][0], alt[0][1]);
  }
  const uint64_t tests = st_cpu_tests.exchange(0), occluded = st_cpu_occluded.exchange(0);
  const uint64_t builds = st_cpu_builds.exchange(0);
  const uint64_t cells = st_cpu_grid_cells.exchange(0);
  const uint64_t filled = st_cpu_cells_filled.exchange(0), age = st_cpu_grid_age.exchange(0);
  const uint64_t build_ns = st_cpu_build_ns.exchange(0);
  uint64_t cmp[2][2];
  for (int a = 0; a < 2; ++a)
    for (int b = 0; b < 2; ++b)
      cmp[a][b] = st_cpu_cmp[a][b].exchange(0);
  REXLOG_INFO("[occlusion] CPU tests (me2_occlusion_umbra_cpu {}): {} tests, {} occluded ({:.1f} %), {} without a grid, "
              "{} at the near plane, {} off the screen, {} answered Umbra; {} frames reprojected (grid {:.1f} frames old, "
              "{:.1f} % of the cells reached, {:.0f} us a frame); new grids: {:.1f} % cells at the far plane, {:.1f} % at 0; "
              "against the GPU counts: CPU occluded / GPU occluded {}, CPU occluded / GPU VISIBLE {}, CPU visible / GPU "
              "occluded {}, both visible {}",
              CpuMode(), tests, occluded, tests ? 100.0 * double(occluded) / double(tests) : 0.0, st_cpu_nogrid.exchange(0),
              st_cpu_near.exchange(0), st_cpu_offscreen.exchange(0), st_cpu_answered.exchange(0), builds,
              builds ? double(age) / double(builds) : 0.0,
              builds && g_cpu.w ? 100.0 * double(filled) / double(builds) / double(g_cpu.w * g_cpu.h) : 0.0,
              builds ? double(build_ns) / 1000.0 / double(builds) : 0.0,
              cells ? 100.0 * double(st_cpu_sky_far.exchange(0)) / double(cells) : 0.0,
              cells ? 100.0 * double(st_cpu_sky_near.exchange(0)) / double(cells) : 0.0, cmp[1][0], cmp[1][1], cmp[0][0],
              cmp[0][1]);
  REXLOG_INFO("[occlusion] CPU cost on the UE3 render thread: tests {:.0f} us a frame ({} frames); reads answered "
              "from a test of an earlier frame {}, reads left to the latent path {} (the index's CPU answer older than "
              "8 frames: {})",
              builds ? double(st_cpu_test_ns.exchange(0)) / 1000.0 / double(builds) : 0.0, builds,
              st_cpu_late_reads.exchange(0), st_cpu_latent_reads.exchange(0), st_cpu_stale_reads.exchange(0));
  REXLOG_INFO("[occlusion] CPU misses (GPU occluded, CPU visible) by reason: occluder cell too far {}, a cell without "
              "data {}, near plane {}, off the screen {}, hole judged visible in the old frame {} (grid {}x{}); holes "
              "decided in the old frame: {} occluded, {} visible; occluded with a part never seen {}, held occluded {}, visible debounced {}",
              st_cpu_miss[1].exchange(0), st_cpu_miss[5].exchange(0), st_cpu_miss[3].exchange(0),
              st_cpu_miss[4].exchange(0), st_cpu_miss[8].exchange(0), g_cpu.w, g_cpu.h, st_cpu_old_occ.exchange(0),
              st_cpu_old_vis.exchange(0), st_cpu_unseen.exchange(0), st_cpu_held.exchange(0), st_cpu_debounced.exchange(0));
}

void LatentReport(int64_t seconds) {
  REXLOG_INFO("[occlusion] Umbra volume guard {} s: {} occluded answers rejected (current box outside tested box)",
              seconds, st_lat_bounds_rejected.exchange(0));
  CpuReport();
  StaleCheckReport();
  uint64_t ages[5];
  for (int i = 0; i < 5; ++i)
    ages[i] = st_lat_age[i].exchange(0);
  REXLOG_INFO("[occlusion] Umbra latent occluded answers by age in frames: 1-2 {}, 3-5 {}, 6-10 {}, 11-20 {}, 21+ {}; "
              "{} counts not yet confirmed (me2_occlusion_umbra_latent_confirm), the answer kept; {} kept past "
              "latent_frames by a still camera (me2_occlusion_umbra_latent_still_frames)",
              ages[0], ages[1], ages[2], ages[3], ages[4], st_lat_held.exchange(0), st_lat_still.exchange(0));
  const uint64_t keys = st_lat_keys.exchange(0), repeated = st_lat_repeat.exchange(0);
  double kinds[4];
  double dups[4];
  for (int k = 0; k < 4; ++k) {
    kinds[k] = keys ? 100.0 * double(st_lat_rep_kind[k].exchange(0)) / double(keys) : 0.0;
    dups[k] = keys ? 100.0 * double(st_lat_dup_kind[k].exchange(0)) / double(keys) : 0.0;
  }
  REXLOG_INFO("[occlusion] Umbra latent keys seen the frame before / again in the same frame: geometry {:.1f} / {:.1f} "
              "%, command +0 {:.1f} / {:.1f} %, +0/+4 {:.1f} / {:.1f} %, box centre {:.1f} / {:.1f} %; {} null command "
              "keys", kinds[0], dups[0], kinds[1], dups[1], kinds[2], dups[2], kinds[3], dups[3],
              st_lat_zero.exchange(0));
  REXLOG_INFO("[occlusion] Umbra retest (me2_occlusion_umbra_retest_frames {}): {} tests skipped (no box), {} of their "
              "issues dropped before the render thread", REXCVAR_GET(me2_occlusion_umbra_retest_frames),
              st_retest_skipped.exchange(0), st_retest_cancelled.exchange(0));
  {
    const uint64_t occ = RetestOccludedFrames();
    REXLOG_INFO("[occlusion] Umbra occluded retest (me2_occlusion_umbra_retest_occluded {}, now {}): {} occluded-node tests "
                "skipped, {} tested ({} of them because the node's box moved since its count)",
                REXCVAR_GET(me2_occlusion_umbra_retest_occluded), occ > 1 ? "on" : "off",
                st_retest_occ_skipped.exchange(0), st_retest_occ_tested.exchange(0), st_retest_occ_moved.exchange(0));
  }
  REXLOG_INFO("[occlusion] Umbra latent {} s: {} test geometries, {:.1f} % of their keys also seen the frame before; "
              "reads answered {} visible / {} occluded from earlier frames, {} without a count (visible), {} occluded "
              "but the camera moved since (visible); {} counts collected, {} dropped; {} keys with a count",
              seconds, keys, keys ? 100.0 * double(repeated) / double(keys) : 0.0, st_lat_visible.exchange(0),
              st_lat_hidden.exchange(0), st_lat_none.exchange(0), st_lat_moved.exchange(0), st_lat_collected.exchange(0),
              st_lat_dropped.exchange(0), g_lat_used);
  REXLOG_INFO("[occlusion] Umbra turn: {} occluded answers kept through a camera turn (box wholly on screen at the "
              "count, me2_occlusion_umbra_latent_turn), {} of them older than latent_frames (turned away and back, "
              "me2_occlusion_umbra_latent_turn_frames); camera cuts so far: epoch {}",
              st_lat_turn_kept.exchange(0) + st_lat_turn_old.load(), st_lat_turn_old.exchange(0), g_latent_view.epoch);
}
}  // namespace

// me2_prepass.cpp (2026-11-05): whether this frame's Umbra view is answered by the occlusion rasterizer (mode 2,
// source 1, not a GPU slot of the "raster" rotation). Then Umbra draws no test boxes and needs no prepass depth. The
// view is set up (sub_8274AA38) before FSceneRenderer::Render draws the prepass in the same frame; a level without
// Umbra never sets it, so its prepass stays.
extern "C" bool Me2OcclusionRasterAnswersFrame() {
  return CpuAnswers() && CpuSource() == 1 &&
         g_raster_view_frame.load(std::memory_order_relaxed) == g_sa2_game_frames.load(std::memory_order_relaxed);
}

// ---- me2_occlusion_umbra_defer: render thread only (under State().mutex) --------------------------------------------
namespace {
bool UmbraDefer() {
  static const bool on = UmbraLatent() && REXCVAR_GET(me2_occlusion_umbra_defer);
  return on;
}
struct DeferItem {
  int kind;  // 0 BEGIN, 1 draw, 2 END
  uint64_t seq;
  std::function<void(bool, uint32_t)> draw;
};
std::vector<DeferItem> g_defer;
bool g_defer_open = false;
uint32_t g_defer_depth = 0;  // the depth target of the held-back boxes (the first one's)
std::atomic<uint64_t> st_defer_replayed{0}, st_defer_dropped{0}, st_defer_draws{0};

bool DeferDraws() { return g_defer_open; }
void DeferPush(std::function<void(bool, uint32_t)> draw, uint32_t depth_va) {
  if (!g_defer_depth)
    g_defer_depth = depth_va;
  g_defer.push_back({1, 0, std::move(draw)});
}
void DeferBegin(uint64_t seq) {
  g_defer.push_back({0, seq, {}});
  g_defer_open = true;
}
void DeferEnd(uint64_t seq) {
  g_defer.push_back({2, seq, {}});
  g_defer_open = false;
}
// draw true: BEGIN / draws / END now, in order; false: every query closed without a draw (answered 'visible').
void DeferFlush(bool draw, uint32_t retarget = 0) {
  std::vector<DeferItem> items;
  items.swap(g_defer);
  g_defer_depth = 0;
  const bool was_open = g_defer_open;
  g_defer_open = false;
  uint64_t queries = 0, draws = 0;
  for (DeferItem& i : items) {
    if (i.kind == 0) {
      BeginLocked(i.seq);
      ++queries;
    } else if (i.kind == 1) {
      i.draw(draw, retarget);
      ++draws;
    } else {
      EndLocked(i.seq);
    }
  }
  CloseOpenLocked(was_open);  // a BEGIN whose END has not come yet: not answered from the GPU
  (draw ? st_defer_replayed : st_defer_dropped).fetch_add(queries, std::memory_order_relaxed);
  if (draw)
    st_defer_draws.fetch_add(draws, std::memory_order_relaxed);
}
uint32_t g_defer_matches = 0;  // resolves of the boxes' depth target in this frame
uint64_t g_trace_frame = 0;
void DeferBeforeDepthResolve(uint32_t depth_va) {
  static const uint32_t nth = uint32_t(REXCVAR_GET(me2_occlusion_umbra_defer_resolve));
  static const bool trace = REXCVAR_GET(me2_occlusion_umbra_defer_trace);
  static const bool retarget = REXCVAR_GET(me2_occlusion_umbra_defer_retarget);
  const bool match = !g_defer.empty() && depth_va == g_defer_depth && !g_defer_open;
  if (match)
    ++g_defer_matches;
  if (retarget) {
    const bool now = !g_defer.empty() && !g_defer_open && g_defer_matches >= nth && depth_va && depth_va != g_defer_depth;
    if (trace && g_trace_frame % 300 == 0 && g_trace_frame / 300 <= 30)
      REXLOG_INFO("[defer-trace] frame {} depth resolve {:08X} ({} of the boxes' target {:08X}, its resolves {}), {} "
                  "draws since the present, {} boxes held back{}",
                  g_trace_frame, depth_va, depth_va == g_defer_depth ? "same" : "other", g_defer_depth,
                  g_defer_matches, g_frame_draws, g_defer.size(), now ? " -> drawn into it now" : "");
    if (now)
      DeferFlush(true, depth_va);
    return;
  }
  if (trace && g_trace_frame % 300 == 0 && g_trace_frame / 300 <= 30)
    REXLOG_INFO("[defer-trace] frame {} depth resolve {:08X}: {} (match #{} of the boxes' target {:08X}), {} draws since "
                "the present, {} boxes held back{}",
                g_trace_frame, depth_va, depth_va == g_defer_depth ? "same target" : "other target", g_defer_matches,
                g_defer_depth, g_frame_draws, g_defer.size(), match && g_defer_matches == nth ? " -> drawn now" : "");
  if (match && g_defer_matches == nth)
    DeferFlush(true);
}
void DeferFrameEnd() {
  static const bool trace = REXCVAR_GET(me2_occlusion_umbra_defer_trace);
  if (trace && g_trace_frame % 300 == 0 && g_trace_frame / 300 <= 30)
    REXLOG_INFO("[defer-trace] frame {} present: {} draws, {} boxes still held back ({} resolves of their target)",
                g_trace_frame, g_frame_draws, g_defer.size(), g_defer_matches);
  ++g_trace_frame;
  g_frame_draws = 0;
  g_defer_matches = 0;
  if (!g_defer.empty())
    DeferFlush(false);
}
}  // namespace

// [occlusion] report line of me2_occlusion_umbra_latent (called from Report).
void Me2UmbraLatentReport(int64_t seconds) {
  if (!UmbraLatent())
    return;
  LatentReport(seconds);
  if (UmbraDefer())
    REXLOG_INFO("[occlusion] Umbra deferred boxes {} s: {} queries drawn at the depth resolve ({} draws), {} dropped "
                "at the present (no resolve of their depth target)",
                seconds, st_defer_replayed.exchange(0), st_defer_draws.exchange(0), st_defer_dropped.exchange(0));
}

// The render thread's half of a carried END / BEGIN (render_thread::SetCarriedOp; State().mutex held).
static void CarriedOcclusion(uint64_t end_seq, uint64_t begin_seq) {
  if (end_seq)
    EndLocked(end_seq);
  if (begin_seq)
    BeginLocked(begin_seq);
}

// IDirect3DQuery9::Issue(query, flags) [sub_82A90458]
REX_EXTERN(__imp__sub_82A90458);
REX_HOOK_RAW(sub_82A90458) {
#if SA2_RENUT_RENDERER
  const uint32_t query = ctx.r3.u32, flags = ctx.r4.u32;
  if (query && (flags & 3) && Enabled() && banjo::renut_gpu::Active() && LoadBE(base, query + 4) == 9) {
    static bool installed = [] {
      g_occlusion_before_draw = BeforeDraw;
      g_occlusion_after_draw = AfterDraw;
      render_thread::SetCarriedOp(CarriedOcclusion);
      if (UmbraDefer()) {
        g_defer_draws = DeferDraws;
        g_defer_push = DeferPush;
        g_before_depth_resolve = DeferBeforeDepthResolve;
        g_frame_end = DeferFrameEnd;
      }
      return true;
    }();
    const bool defer = t_tag == 5 && UmbraDefer();  // one of Umbra's queries (sub_827486B8 opcodes 128 / 129)
    static const bool carry_on = REXCVAR_GET(me2_occlusion_carry);
    const bool carry = carry_on && !defer;
    (void)installed;
    const uint32_t device = LoadBE(base, query);
    if (device && !IsRecordingCommandBuffer(device)) {
      FlushDeferredVertices(device);  // a Begin*Vertices draw before the BEGIN / END belongs before it
      if (flags & 2) {
        const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
        Record& r = g_records[seq & (kRecords - 1)];
        r.state.store(kOpen, std::memory_order_relaxed);
        r.count.store(0, std::memory_order_relaxed);
        r.first_poll_us.store(0, std::memory_order_relaxed);
        r.frame.store(g_sa2_game_frames.load(std::memory_order_relaxed), std::memory_order_relaxed);
        r.seq.store(seq, std::memory_order_release);
        const auto newer_frame = [&](uint64_t latest) {
          const Record& old = g_records[latest & (kRecords - 1)];
          return old.seq.load(std::memory_order_relaxed) == latest &&
                 old.frame.load(std::memory_order_relaxed) < r.frame.load(std::memory_order_relaxed);
        };
        if (QuerySlot* slot = FindQuerySlot(query, true)) {
          const uint64_t latest = slot->latest.load(std::memory_order_relaxed);
          if (latest && newer_frame(latest))
            slot->previous.store(latest, std::memory_order_relaxed);
          slot->latest.store(seq, std::memory_order_release);
        } else {
          std::lock_guard<std::mutex> lock(g_map_mutex);
          g_map_used.store(true, std::memory_order_release);
          uint64_t& latest = g_latest[query];
          if (latest && newer_frame(latest))
            g_previous[query] = latest;
          latest = seq;
        }
        st_begun.fetch_add(1, std::memory_order_relaxed);
        if (carry)
          render_thread::CarryBegin(seq);
        else
          render_thread::Queue([seq, defer] { defer ? DeferBegin(seq) : BeginLocked(seq); });
      }
      if (flags & 1) {
        const uint64_t seq = LatestIssue(query);
        if (seq && carry && t_umbra_skip_query == query && render_thread::CancelCarriedBegin(seq)) {
          // me2_occlusion_umbra_retest_frames: a skipped test (no box drawn): the issue never reaches the render
          // thread; closed here with no draws (the latent read answers the node's last count).
          Record& r = g_records[seq & (kRecords - 1)];
          if (r.seq.load(std::memory_order_relaxed) == seq)
            r.state.store(kBroken, std::memory_order_release);
          st_retest_cancelled.fetch_add(1, std::memory_order_relaxed);
        } else if (seq) {
          if (carry)
            render_thread::CarryEnd(seq);
          else
            render_thread::Queue([seq, defer] { defer ? DeferEnd(seq) : EndLocked(seq); });
        }
        if (t_umbra_skip_query == query)
          t_umbra_skip_query = 0;
      }
    }
  }
#endif
  __imp__sub_82A90458(ctx, base);
}

// HRESULT IDirect3DQuery9::GetData(D3DQuery*, void *pData, DWORD size, DWORD flags) [sub_82A906E0]: for an occlusion
// query (type word +4 = 9) the count from the host queries (see above), else "visible" (a large count); S_OK. The
// other query types keep the original.
REX_EXTERN(__imp__sub_82A906E0);
REX_HOOK_RAW(sub_82A906E0) {
#if SA2_RENUT_RENDERER
  if (banjo::renut_gpu::Active()) {
    const uint32_t query = ctx.r3.u32, out = ctx.r4.u32;
    if (query && LoadBE(base, query + 4) == 9) {
      t_outcome = 0;
      // Umbra (reader tag 5, sub_827486B8 opcode 130) polls without waiting: RHIGetOcclusionQueryResult with
      // bWait = 0 returns "no result" at S_FALSE and Umbra asks again in a later frame, when the query is on the GPU.
      static const bool umbra_async = REXCVAR_GET(me2_occlusion_umbra_async) && renut::gpu::IsDeko3D();
      uint32_t count = UmbraLatent() && t_tag == 5 ? (t_cpu_answer ? CpuAnsweredTraced(t_cpu_answer) : LatentAnswer(t_latent_key))
                             : Enabled() ? Answer(query, umbra_async && t_tag == 5 && t_umbra_nowait)
                                         : kVisible;
      if (UmbraLatent() && t_tag == 5)
        WallTraceAnswer(t_latent_key, g_sa2_game_frames.load(std::memory_order_relaxed), count);
      // me2_occlusion_cut_visible_frames: right after a camera cut, UE3's own "no pixels" / "not ready" answers (from
      // the previous camera) are visible.
      if (t_tag >= 1 && t_tag <= 4 && (count == 0 || count == kNotReady) && CutVisibleFrames() > 0) {
        const uint64_t cut = g_cut_frame.load(std::memory_order_relaxed);
        const uint64_t now = g_sa2_game_frames.load(std::memory_order_relaxed);
        if (cut && now >= cut && now - cut < uint64_t(CutVisibleFrames())) {
          count = kVisible;
          g_cut_overrides.fetch_add(1, std::memory_order_relaxed);
        }
      }
      st_tag[t_tag][t_outcome].fetch_add(1, std::memory_order_relaxed);
      if (count == kNotReady) {
        ctx.r3.u64 = 1;  // S_FALSE: not ready, nothing written
        if (Enabled())
          Report();
        return;
      }
      if (out)
        *reinterpret_cast<uint32_t*>(base + out) = __builtin_bswap32(count);
      ctx.r3.u64 = 0;  // S_OK
      if (Enabled())
        Report();
      return;
    }
  }
#endif
  __imp__sub_82A906E0(ctx, base);
}

// The readers' tags (diagnostic, see st_tag).
#define ME2_OCCLUSION_READER(addr, tag)                   REX_EXTERN(__imp__sub_##addr);                          REX_HOOK_RAW(sub_##addr) {                                const int saved = t_tag;                                t_tag = tag;                                            __imp__sub_##addr(ctx, base);                           t_tag = saved;                                        }
ME2_OCCLUSION_READER(82695EE0, 1)
REX_EXTERN(__imp__sub_82696410);
REX_HOOK_RAW(sub_82696410) {
  // r4 = the primitive's compact scene entry (sub_82689710: +16 origin, +28 box extent, big-endian floats)
  const uint32_t ce = ctx.r4.u32;
  bool ok = ce >= 0x10000u && ce < 0xF0000000u;
  if (ok) {
    for (int k = 0; k < 3; ++k) {
      uint32_t ob = LoadBE(base, ce + 16 + 4 * uint32_t(k)), eb = LoadBE(base, ce + 28 + 4 * uint32_t(k));
      float o, e;
      std::memcpy(&o, &ob, 4);
      std::memcpy(&e, &eb, 4);
      ok = ok && std::isfinite(o) && std::isfinite(e) && e >= 0.0f;
      t_reader_mn[k] = o - e;
      t_reader_mx[k] = o + e;
    }
  }
  const int saved = t_tag;
  const bool saved_box = t_reader_box;
  t_tag = 2;
  t_reader_box = ok;
  __imp__sub_82696410(ctx, base);
  t_tag = saved;
  t_reader_box = saved_box;
}
ME2_OCCLUSION_READER(82696780, 3)
ME2_OCCLUSION_READER(826A7690, 4)
// sub_827486B8: a render-command interpreter (vtable 0x82193330 slot 1, (this, opcode)); opcodes 128 / 129 = Issue
// BEGIN / END of the query table [0x833D6784]+4 entry named by the command's record, 130 = read it back into the
// record (+4 byte "result available", +8 pixel count; +5 byte = wait). Reader tag 5, plus the diagnostic below
// (me2_occlusion_diag_cmds, 2026-10-27): which system issues these same-frame visibility tests.
REXCVAR_DEFINE_BOOL(me2_occlusion_diag_cmds, false, "ME2",
                    "Diagnostic: log the occlusion commands 128-130 of sub_827486B8 (record, query, answer, the memory "
                    "around the record)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REX_EXTERN(__imp__sub_827486B8);
static void UmbraCommandHook(PPCContext& ctx, uint8_t* base) {
  // me2-nx-deko (2026-11-02, me2_cull_distance.cpp): command 50 = Umbra's per-instance draw-distance question; small
  // primitives far away are answered "skip_instance" there (drawn in no pass).
  if (ctx.r4.u32 == 50) {
    me2_lod_distance_scale_umbra(ctx, base);  // me2-nx-deko (2026-11-04): me2_lod_distance_scale_mode 1
    if (me2_umbra_size_cull(ctx, base))
      return;
  }
  // Command 0 copies the view's LOD distance factor for Umbra: me2_lod_distance_scale applies first.
  if (ctx.r4.u32 == 0)
    me2_lod_distance_scale_view(ctx, base);
  static const bool skip = REXCVAR_GET(me2_occlusion_umbra_skip) && !UmbraLatent() &&
                           !(REXCVAR_GET(me2_occlusion_umbra_async) && renut::gpu::IsDeko3D());
  if (UmbraLatent() && ctx.r4.u32 >= 129 && ctx.r4.u32 <= 131 && ctx.r3.u32) {
    const uint32_t self = ctx.r3.u32, op = ctx.r4.u32;
    const uint32_t cmd = LoadBE(base, self + 4);
    const uint32_t rec = cmd ? LoadBE(base, cmd + 52) : 0;
    const uint32_t index = rec ? LoadBE(base, rec) : 0;
    const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
    if (rec && op == 131) {
      // The node's keys: its test geometry (in Umbra's buffers before the original copies it), Umbra's command words.
      static const int kind = REXCVAR_GET(me2_occlusion_umbra_latent_key);
      static const bool key_stats = REXCVAR_GET(me2_occlusion_umbra_latent_key_stats);
      const uint32_t w0 = LoadBE(base, cmd), w4 = LoadBE(base, cmd + 4);
      // 2026-10-28: only the key in use is computed unless the key-kind statistics are wanted (the geometry hash
      // read the whole box and four hash maps were updated for every test on the UE3 render thread).
      uint64_t keys[4] = {};
      const Me2OcclusionBounds bounds = key_stats || kind == 3 ? TestBounds(base, rec) : Me2OcclusionBounds{};
      if (key_stats || kind == 0)
        keys[0] = GeometryKey(base, rec);
      if (key_stats || kind == 1)
        keys[1] = w0 ? (uint64_t(1) << 63) | w0 : 0;
      if (key_stats || kind == 2)
        keys[2] = w0 ? (uint64_t(1) << 62) | (uint64_t(w4) << 32) | w0 : 0;
      if (key_stats || kind == 3)
        keys[3] = CentreKey(bounds);
      if (!w0)
        st_lat_zero.fetch_add(1, std::memory_order_relaxed);
      static const bool dump = REXCVAR_GET(me2_occlusion_umbra_latent_dump);
      static uint64_t dump_window = ~uint64_t(0);
      static int dumped = 0;
      if (dump) {
        const uint64_t window = frame / 300;
        if (window != dump_window) {
          dump_window = window;
          dumped = 0;
        }
        if (dumped < 30) {
          ++dumped;
          std::string cw, rw, vw;
          for (uint32_t i = 0; i < 24; ++i)
            cw += fmt::format(" {:08x}", LoadBE(base, cmd + 4 * i));
          for (uint32_t i = 0; i < 8; ++i)
            rw += fmt::format(" {:08x}", LoadBE(base, rec + 4 * i));
          const uint32_t verts = std::min<uint32_t>(LoadBE(base, rec + 16), 4);
          for (uint32_t i = 0; i < verts * 3; ++i) {
            const uint32_t bits = LoadBE(base, kUmbraVertices + 4 * i);
            float f;
            std::memcpy(&f, &bits, 4);
            vw += fmt::format(" {:.2f}", f);
          }
          REXLOG_INFO("[umbra-dump] frame {} self {:08x} cmd {:08x} rec {:08x} | cmd:{} | rec:{} | v:{}", frame, self,
                      cmd, rec, cw, rw, vw);
        }
      }
      const uint64_t key = keys[kind];
      if (WallTraceAllowed(key, frame)) {
        const uint32_t view = LoadBE(base, self + 36);
        float camera[3] = {};
        for (unsigned c = 0; view && c < 3; ++c) {
          const uint32_t bits = LoadBE(base, view + 384 + c * 4);
          std::memcpy(&camera[c], &bits, 4);
        }
        REXLOG_INFO("[umbra-chain-v1] geometry frame {} self {:08X} view {:08X} rec {:08X} index {} query {:08X} seq {} hash {:016X} camera ({:.3f},{:.3f},{:.3f}) box [{:.3f},{:.3f},{:.3f}]-[{:.3f},{:.3f},{:.3f}]",
                    frame, self, view, rec, index, QueryOfIndex(base, index),
                    LatestIssue(QueryOfIndex(base, index)), GeometryKey(base, rec), camera[0], camera[1], camera[2],
                    bounds.lo[0], bounds.lo[1], bounds.lo[2], bounds.hi[0], bounds.hi[1], bounds.hi[2]);
      }
      // me2_occlusion_umbra_retest_frames: a node with a count is tested on its own frame of every N.
      static const uint64_t retest = uint64_t(REXCVAR_GET(me2_occlusion_umbra_retest_frames));
      bool skip_test = false;
      if (key) {
        LatentSlot& slot = LatentClaim(key);
        if (slot.seen && slot.seen + 1 == frame)
          st_lat_repeat.fetch_add(1, std::memory_order_relaxed);
        slot.seen = frame;
        LatentResult& res = slot.res;
        const uint64_t phase = retest > 1 ? (frame + ((key * 0x9E3779B97F4A7C15ull) >> 56)) % retest : 0;
        // 2026-10-28 23:5x (console: what a door opens on appeared late): only a node last counted VISIBLE skips a re-test;
        // an occluded one is re-tested by Umbra only now and then, and a skipped test answered the old "occluded" again.
        if (retest > 1 && phase != 0 && res.any && res.epoch == g_latent_view.epoch &&
            res.pixels != 0 && frame - res.issued < retest)
          skip_test = true;
        // me2_occlusion_umbra_retest_occluded: an occluded node, its last test recent, its count fresh, the camera still.
        // 2026-11-04 (user, PC: squadmates standing still lost their bodies, heads floating): only the very box that was
        // counted (Me2OcclusionBounds::Same). Level geometry tests the same box every frame; an animated character's box
        // moves a little each frame and is always tested.
        else if (const uint64_t occ = RetestOccludedFrames();
                 occ > 1 && res.any && res.epoch == g_latent_view.epoch && res.pixels == 0 &&
                 frame - res.issued < occ && frame - res.frame <= occ + 2 &&
                 CameraNear(res.frame, frame, 0.05f, 50.0f) &&  // ~3 degrees, 50 units: turning or walking re-tests
                 (res.bounds.Same(bounds) ||
                  (st_retest_occ_moved.fetch_add(1, std::memory_order_relaxed), false))) {
          skip_test = true;
          st_retest_occ_skipped.fetch_add(1, std::memory_order_relaxed);
        } else {
          res.issued = frame;
          if (res.any && res.pixels == 0)
            st_retest_occ_tested.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // me2_occlusion_umbra_cpu: the CPU's answer to this test; with mode 2 it answers Umbra and no box is drawn.
      if (CpuMode() && CpuSlotOn()) {
        const auto test_start = std::chrono::steady_clock::now();
        uint8_t answer = CpuTest(base, rec, frame);
        if (index < kIndexKeys) {
          g_raster_alt[index] = CpuSource() == 1 ? t_raster_alt : 0;
          g_raster_token[index] = CpuSource() == 1 ? Me2RasterTakeToken() : 0;
        }
        // me2_occlusion_cpu_hold: an undecided test (a hole, the near plane) of a key the CPU itself found occluded
        // within the last N frames stays occluded (10) until the grid covers it again. (Grid noise rules: not for the
        // rasterizer, me2_occlusion_cpu_source 1.)
        if (key && CpuSource() == 0) {
          static const uint64_t hold = uint64_t(REXCVAR_GET(me2_occlusion_cpu_hold));
          if (LatentSlot* held = LatentFind(key)) {
            static const uint8_t debounce = uint8_t(REXCVAR_GET(me2_occlusion_cpu_debounce));
            if (answer == 2 || answer == 7 || answer == 9) {
              held->cpu_occluded_frame = frame;
              held->cpu_visible_streak = 0;
            } else if (!CpuDecided(answer) && hold && held->cpu_occluded_frame &&
                       frame - held->cpu_occluded_frame <= hold) {
              answer = 10;
              st_cpu_held.fetch_add(1, std::memory_order_relaxed);
            } else if ((answer == 1 || answer == 8) && debounce && held->cpu_occluded_frame &&
                       frame - held->cpu_occluded_frame <= uint64_t(debounce) + 1 &&
                       held->cpu_visible_streak < debounce) {
              ++held->cpu_visible_streak;
              answer = 11;
              st_cpu_debounced.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
        if (index < kIndexKeys) {
          t_cpu_info.code = answer;
          g_cpu_info[index] = t_cpu_info;
        }
        st_cpu_test_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - test_start).count()),
                                 std::memory_order_relaxed);
        if (index < kIndexKeys)
          g_cpu_answers[index] = {index + 1, frame, answer};
        // 2026-10-28 20:36 (PC compare: most misses were reprojection holes, mostly what was off the screen 2-3
        // frames ago): only a test the grid can decide (occluded, visible by depth, off the screen) is answered by the
        // CPU and has no box; a hole or the near plane keeps the box and the latent answer (the GPU's count).
        if (CpuAnswers() && CpuDecided(answer))
          skip_test = true;
      }
      SetIndexKey(index, key, skip_test, bounds);
      st_lat_keys.fetch_add(1, std::memory_order_relaxed);
      for (int k = 0; k < 4 && key_stats; ++k) {
        if (!keys[k])
          continue;
        uint64_t& seen = g_latent.last_seen[k][keys[k]];
        const bool again = seen && seen + 1 == frame;
        if (seen == frame)
          st_lat_dup_kind[k].fetch_add(1, std::memory_order_relaxed);
        if (again)
          st_lat_rep_kind[k].fetch_add(1, std::memory_order_relaxed);
        seen = frame;
      }
      if (skip_test) {
        // No box: the original copies the geometry and draws it (827DC2F0); Umbra only reads the answer later.
        st_retest_skipped.fetch_add(1, std::memory_order_relaxed);
        t_umbra_skip_query = QueryOfIndex(base, index);
        return;
      }
    }
    if (rec && op == 130) {
      LatentCollect(frame);
      *reinterpret_cast<uint32_t*>(base + kUmbraKickCounter) = 0;  // no forced kick
      uint64_t key = 0;
      bool skipped;
      t_latent_key = GetIndexKey(index, key, skipped) ? key : 0;
      t_cpu_answer = 0;
      t_cpu_index = index;
      // 2026-10-28 20:5x: the read may come frames after its test (opcode 131); its CPU answer stays good for that
      // read (the test drew no box, so the latent path would have only an old count: 'visible', a prompt blinking).
      if (CpuAnswers() && index < kIndexKeys && g_cpu_answers[index].tag == index + 1 &&
          IndexEpoch(index) == g_latent_view.epoch && frame - g_cpu_answers[index].frame <= 8 &&
          CpuDecided(g_cpu_answers[index].answer)) {
        t_cpu_answer = g_cpu_answers[index].answer;
        if (g_cpu_answers[index].frame != frame)
          st_cpu_late_reads.fetch_add(1, std::memory_order_relaxed);
      } else if (CpuAnswers()) {
        st_cpu_latent_reads.fetch_add(1, std::memory_order_relaxed);
        if (index < kIndexKeys && g_cpu_answers[index].tag == index + 1 && CpuDecided(g_cpu_answers[index].answer))
          st_cpu_stale_reads.fetch_add(1, std::memory_order_relaxed);
      }
    }
    const int saved = t_tag;
    t_tag = 5;
    __imp__sub_827486B8(ctx, base);
    t_tag = saved;
    if (rec && op == 129) {
      // END: this issue's count is collected later under the node's key (a skipped test has none).
      t_umbra_skip_query = 0;
      uint64_t key = 0;
      bool skipped = false;
      const bool known = GetIndexKey(index, key, skipped);
      const uint32_t query = QueryOfIndex(base, index);
      const uint64_t seq = query ? LatestIssue(query) : 0;
      if (seq && known && key && !skipped) {
        const uint8_t cpu = index < kIndexKeys && g_cpu_answers[index].tag == index + 1 &&
                                    g_cpu_answers[index].frame == frame
                                ? g_cpu_answers[index].answer
                                : 0;
        const uint8_t cpu_alt = cpu && index < kIndexKeys ? g_raster_alt[index] : 0;
        const uint32_t raster_token = cpu && index < kIndexKeys ? g_raster_token[index] : 0;
        g_latent.pending.push_back(
            {seq, key, frame, cpu, IndexBounds(index), IndexEpoch(index), cpu_alt, raster_token});
        if (WallTraceAllowed(key, frame))
          REXLOG_INFO("[umbra-chain-v1] issued end frame {} self {:08X} rec {:08X} index {} query {:08X} seq {} beginframe {}",
                      frame, self, rec, index, query, seq, g_records[seq & (kRecords - 1)].frame.load());
      }
    }
    return;
  }
  if (skip && (ctx.r4.u32 == 128 || ctx.r4.u32 == 129 || ctx.r4.u32 == 131)) {
    st_umbra_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int saved = t_tag;
  t_tag = 5;
  static const bool diag = REXCVAR_GET(me2_occlusion_diag_cmds);
  const uint32_t op = ctx.r4.u32, self = ctx.r3.u32;
  uint32_t rec = 0;
  if (op >= 128 && op <= 130 && self) {
    const uint32_t cmd = LoadBE(base, self + 4);
    rec = cmd ? LoadBE(base, cmd + 52) : 0;
  }
  const bool saved_nowait = t_umbra_nowait;
  t_umbra_nowait = op == 130 && rec && ((LoadBE(base, rec + 4) >> 16) & 0xFF) == 0;  // the guest reads byte +5 too
  __imp__sub_827486B8(ctx, base);
  t_umbra_nowait = saved_nowait;
  t_tag = saved;
  if (!diag)
    rec = 0;
  if (rec) {
    static std::atomic<uint64_t> n{0};
    const uint64_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 120 || (k % 2000) == 0) {
      const uint32_t cmd = LoadBE(base, self + 4);
      const uint32_t index = LoadBE(base, rec);
      const uint32_t table = LoadBE(base, 0x833D6784u);
      const uint32_t query = table ? LoadBE(base, LoadBE(base, table + 4) + index * 4) : 0;
      // Only the record's own 16 bytes: words around it may lie on an unmapped page (crashed at 0x80BF0004).
      std::string around;
      for (int i = 0; i < 4; ++i)
        around += fmt::format(" {:08x}", LoadBE(base, rec + 4 * i));
      std::string cmdw;
      for (int i = 0; i < 14; ++i)  // up to +52, the record pointer the guest reads
        cmdw += fmt::format(" {:08x}", LoadBE(base, cmd + 4 * i));
      REXLOG_INFO("[occlusion-cmd] #{} op {} self {:08x} (vt {:08x}) cmd {:08x} rec {:08x} index {} query {:08x} "
                  "wait {} -> avail {} pixels {}; cmd words:{}; rec words:{}",
                  k, op, self, LoadBE(base, self), cmd, rec, index, query, (LoadBE(base, rec + 4) >> 16) & 0xFF,
                  LoadBE(base, rec + 4) >> 24, LoadBE(base, rec + 8), cmdw, around);
    }
  }
}

// me2-nx-deko (2026-11-05, stage 0 of the native static draw lists): which of Umbra's render commands draw. On Umbra
// levels the visible objects are drawn from inside Umbra's traversal through this interpreter (console stacks
// 2026-10-27), not through TStaticMeshDrawList::DrawVisible (me2_drawlist_diag.cpp: those drew almost nothing in the
// prologue). me2_umbra_cmd_diag: per opcode its own calls, draws and time (nested commands taken off their parent),
// the top ones every 10 s as "[umbra-cmd]" (UE3 render thread).
REXCVAR_DEFINE_BOOL(me2_umbra_cmd_diag, false, "ME2/Diagnostics",
                    "Count calls, draws and time of each Umbra render command (sub_827486B8 opcodes), \"[umbra-cmd]\" "
                    "lines every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern std::atomic<uint64_t> g_me2_draws_total;  // renut d3d hooks/draw.cpp: every draw
namespace {
struct UmbraCmdStat {
  uint64_t calls = 0, draws = 0, ns = 0;
};
UmbraCmdStat g_cmd[256];
struct UmbraCmdFrame {
  uint64_t child_ns = 0, child_draws = 0;
};
UmbraCmdFrame g_cmd_stack[64];
int g_cmd_depth = 0;
std::chrono::steady_clock::time_point g_cmd_window{};
uint64_t g_cmd_frames0 = 0, g_cmd_all0 = 0;
void UmbraCmdReport(std::chrono::steady_clock::time_point now) {
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const double f = double(std::max<uint64_t>(1, frame - g_cmd_frames0));
  const uint64_t all = g_me2_draws_total.load(std::memory_order_relaxed) - g_cmd_all0;
  std::array<int, 256> order{};
  for (int i = 0; i < 256; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [](int a, int b) {
    return g_cmd[a].draws != g_cmd[b].draws ? g_cmd[a].draws > g_cmd[b].draws : g_cmd[a].ns > g_cmd[b].ns;
  });
  std::string by;
  uint64_t total_draws = 0, total_ns = 0;
  for (int i = 0; i < 256; ++i) {
    total_draws += g_cmd[i].draws;
    total_ns += g_cmd[i].ns;
  }
  int shown = 0;
  for (int i : order) {
    const UmbraCmdStat& c = g_cmd[i];
    if (!c.calls || shown >= 14) continue;
    ++shown;
    by += fmt::format("; op {} {:.0f} calls {:.0f} draws {:.3f} ms", i, double(c.calls) / f, double(c.draws) / f,
                      double(c.ns) / 1e6 / f);
  }
  REXLOG_INFO("[umbra-cmd] {:.0f} s: Umbra's commands drew {:.0f} draws a frame ({:.1f} % of all draws), {:.2f} ms a "
              "frame of their own time; per frame by opcode (own draws / time){}",
              std::chrono::duration<double>(now - g_cmd_window).count(), double(total_draws) / f,
              all ? 100.0 * double(total_draws) / double(all) : 0.0, double(total_ns) / 1e6 / f, by);
  for (auto& c : g_cmd) c = UmbraCmdStat{};
  g_cmd_window = now;
  g_cmd_frames0 = frame;
  g_cmd_all0 = g_me2_draws_total.load(std::memory_order_relaxed);
}
}  // namespace

// me2-nx-deko (2026-11-05, native visibility step 1): me2_umbra_record_s = N records, every N seconds, one Umbra query
// (from its op 0 to its op 1 on the render thread) command by command: op, nesting depth, the command record's current
// instance (cmd = [this + 4], index [cmd + 48]; wrapper = [[0x833960E4 + 4] + 4 * index]: +0 the primitive scene info,
// +8..+20 its bounding sphere) and the draws each command made. Consecutive commands of the same op and depth are
// written as one run ("[umbra-rec]" lines).
REXCVAR_DEFINE_INT32(me2_umbra_record_s, 0, "ME2/Diagnostics",
                     "Every this many seconds, record one Umbra query command by command (\"[umbra-rec]\" lines); "
                     "0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace {
constexpr uint32_t kUmbraInstanceTable = 0x833960E4u;  // TU2: +4 the wrapper pointers (82DDEC08)
struct RecItem {
  uint8_t op = 0, depth = 0;
  int32_t inst = -1;
  uint32_t wrapper = 0, psi = 0, proxy_vt = 0, draws = 0;
  float radius = 0;
  uint32_t mask = 0xFFFFFFFFu;  // op 48: the batch mask Umbra computed (sub_82DE5318 -> [[0x83396130 + 4] + 4 * index])
};
std::vector<RecItem> g_rec;
bool g_rec_active = false;
int g_rec_depth = 0;
uint64_t g_rec_count = 0;
std::chrono::steady_clock::time_point g_rec_next{};
inline uint32_t RecL32(const uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}
inline bool RecOk(uint32_t va) { return va >= 0x10000u && va < 0xF0000000u; }
void RecDump(const uint8_t* base) {
  ++g_rec_count;
  std::unordered_map<uint32_t, int> distinct48;
  uint64_t n48 = 0, n50 = 0, draws = 0;
  for (const RecItem& r : g_rec) {
    if (r.op == 48) ++n48, distinct48[uint32_t(r.inst)]++;
    if (r.op == 50) ++n50;
    draws += r.draws;
  }
  {
    std::unordered_map<uint32_t, int> masks;
    for (const RecItem& r : g_rec)
      if (r.op == 48) masks[r.mask]++;
    std::string mh;
    for (auto& kv : masks) mh += fmt::format(" {:X}:{}", kv.first, kv.second);
    REXLOG_INFO("[umbra-rec] op 48 batch masks (mask:count):{}; planes {} (source {}), gate {:08X}", mh,
                RecL32(base, 0x8339AFC4u), RecL32(base, 0x8339AFC4u), RecL32(base, 0x83395FA0u));
    std::string pl;
    const uint32_t n = std::min<uint32_t>(RecL32(base, 0x8339AFC4u), 8);
    for (uint32_t k = 0; k < n; ++k) {
      float v[4];
      for (int c = 0; c < 4; ++c) {
        const uint32_t w = RecL32(base, 0x8339AFC8u + 16 * k + 4 * c);
        std::memcpy(&v[c], &w, 4);
      }
      pl += fmt::format(" ({:.3f} {:.3f} {:.3f} {:.0f})", v[0], v[1], v[2], v[3]);
    }
    if (!pl.empty()) REXLOG_INFO("[umbra-rec] planes:{}", pl);
  }
  REXLOG_INFO("[umbra-rec] query {}: {} commands, op 48 x{} ({} distinct instances), op 50 x{}, {} draws; instance "
              "table words {:08X} {:08X} {:08X} {:08X}",
              g_rec_count, g_rec.size(), n48, distinct48.size(), n50, draws, RecL32(base, kUmbraInstanceTable),
              RecL32(base, kUmbraInstanceTable + 4), RecL32(base, kUmbraInstanceTable + 8),
              RecL32(base, kUmbraInstanceTable + 12));
  std::string line;
  for (size_t i = 0; i < g_rec.size();) {
    size_t j = i;
    uint64_t run_draws = 0;
    std::string insts;
    while (j < g_rec.size() && g_rec[j].op == g_rec[i].op && g_rec[j].depth == g_rec[i].depth) {
      run_draws += g_rec[j].draws;
      if (g_rec[j].inst >= 0 && j - i < 8)
        insts += fmt::format(" {}:{:08X}/{:08X}/r{:.0f}", g_rec[j].inst, g_rec[j].psi, g_rec[j].proxy_vt,
                             g_rec[j].radius);
      ++j;
    }
    line += fmt::format("{}[d{} op{} x{}{}{}{}]", line.empty() ? "" : " ", g_rec[i].depth, g_rec[i].op, j - i,
                        run_draws ? fmt::format(" draws {}", run_draws) : "", insts, j - i > 8 && !insts.empty() ? " ..." : "");
    if (line.size() > 1500) {
      REXLOG_INFO("[umbra-rec] {}", line);
      line.clear();
    }
    i = j;
  }
  if (!line.empty()) REXLOG_INFO("[umbra-rec] {}", line);
  g_rec.clear();
}
// Before a command: starts a recording at a top-level op 0 when due, adds the command.
int RecBefore(PPCContext& ctx, uint8_t* base, uint32_t op) {
  static const int32_t every_s = REXCVAR_GET(me2_umbra_record_s);
  if (every_s <= 0) return -1;
  if (g_rec_active && op == 0 && g_rec_depth == 0 && !g_rec.empty()) {  // no op 1 seen: the next query begins
    g_rec_active = false;
    RecDump(base);
  }
  if (!g_rec_active && op == 0 && g_rec_depth == 0) {
    const auto now = std::chrono::steady_clock::now();
    if (g_rec_next == std::chrono::steady_clock::time_point{}) g_rec_next = now + std::chrono::seconds(every_s);
    if (now >= g_rec_next) {
      g_rec_active = true;
      g_rec.clear();
      g_rec_next = now + std::chrono::seconds(every_s);
    }
  }
  if (!g_rec_active || g_rec.size() >= 20000) return -1;
  RecItem r;
  r.op = uint8_t(op);
  r.depth = uint8_t(std::min(g_rec_depth, 255));
  const uint32_t self = ctx.r3.u32;
  const uint32_t cmd = RecOk(self) ? RecL32(base, self + 4) : 0;
  if (RecOk(cmd)) {
    r.inst = int32_t(RecL32(base, cmd + 48));
    const uint32_t table = RecL32(base, kUmbraInstanceTable + 4);
    if (r.inst >= 0 && RecOk(table)) {
      r.wrapper = RecL32(base, table + 4u * uint32_t(r.inst));
      if (RecL32(base, 0x83395FA0u) != 0 && RecOk(RecL32(base, 0x83396134u)))
        r.mask = RecL32(base, RecL32(base, 0x83396134u) + 4u * uint32_t(r.inst));
      if (RecOk(r.wrapper)) {
        r.psi = RecL32(base, r.wrapper);
        const uint32_t rb = RecL32(base, r.wrapper + 20);
        std::memcpy(&r.radius, &rb, 4);
        const uint32_t proxy = RecOk(r.psi) ? RecL32(base, r.psi + 8) : 0;
        r.proxy_vt = RecOk(proxy) ? RecL32(base, proxy) : 0;
      }
    }
  }
  g_rec.push_back(r);
  return int(g_rec.size() - 1);
}
}  // namespace

// me2-nx-deko (2026-11-05, native visibility step 2, SHADOW): me2_native_vis_shadow computes, at each top-level Umbra
// query start (op 0) with a rasterizer buffer for the frame, our own visible set: every instance of Umbra's table
// (wrapper +0 = the primitive scene info; its bounds at +48: origin, box extent, sphere radius) tested as a box against
// the frustum and the CPU depth buffer (Me2RasterTestBox). During the query Umbra's reports are noted per instance (op
// 50 = asked for the draw distance, op 48 = reported visible); at its op 1 the two sets are compared. Nothing that is
// drawn changes. "[native-vis]" lines every 10 s.
REXCVAR_DEFINE_BOOL(me2_native_vis_shadow, true, "ME2/Diagnostics",
                    "Native visibility shadow mode: compute our own visible set per Umbra query and compare it with "
                    "Umbra's (\"[native-vis]\" lines); changes nothing drawn")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Step 2.5 (2026-11-05): with the shadow set computed, an instance Umbra reports (ops 50 / 48 / 144) that our box test
// calls occluded (code 2) is not passed on: on screen the native result for those instances. Frustum disagreements and
// instances without usable bounds keep Umbra's verdict. "native_vis" in me2_ab_cycle turns it off in its slots.
// Step 3 (2026-11-05): me2_native_vis replaces Umbra's traversal (sub_82DFD790, called by the query sub_82DF0A88 between
// its commands 160 and 161; before it the query's setup and op 0, after it the delivery sub_82DF08D8 -> sub_82DECC28
// with the drawing commands 162 / 164 / 165 / 163 / 1, all still Umbra's and UE3's own). In its place, for every
// instance of our visible set (codes 1 visible, 3 near plane, 6 no usable bounds, and 5 when the bounds lie far from
// the primitive's own transform): the command record's instance (cmd = [[query + 432] + 4], index at cmd + 48), the
// skip byte cmd + 108 cleared, op 50 (UE3's draw distance: "skip_instance" sets the byte), then unless skipped op 48
// and op 144; then op 145 (the state restore Umbra sends after its test rounds). Needs me2_native_vis_shadow (the set
// is computed at op 0). "native_vis" in me2_ab_cycle gives Umbra's traversal back in its slots.
REXCVAR_DEFINE_BOOL(me2_native_vis, true, "ME2",
                    "Replace Umbra's traversal with the native visible set (needs me2_native_vis_shadow; default on "
                    "since the console A/Bs of 2026-11-05: +9-14 % fps in Omega, nothing visibly wrong)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_native_vis_filter, false, "ME2",
                    "TEST: drop Umbra's reports of instances that the native visibility test calls occluded (needs "
                    "me2_native_vis_shadow)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace {
struct NvQuery {
  bool active = false;
  uint32_t count = 0;
  std::vector<uint8_t> ours;   // per instance: our code (0 = not tested, 1 visible, 2 occluded, 3 near, 5 frustum,
                               // 6 no usable bounds, 7 no primitive, 8 frustum but bounds far from the transform)
  bool native = false;         // this query's traversal was ours (no comparison)
  std::vector<uint8_t> umbra;  // per instance: bit 0 op 50, bit 1 op 48, bit 2 op 144
};
NvQuery g_nv;
struct NvStats {
  uint64_t queries = 0, no_buffer = 0, instances = 0, ns = 0, passes = 0;
  uint64_t worker_passes = 0, worker_reused = 0, worker_retested = 0, worker_ns = 0;  // me2_native_vis_worker
  uint64_t umbra_vis = 0, ours_vis = 0, both = 0;
  uint64_t umbra_only[8] = {};  // Umbra reported it (op 48), we did not: by our code
  uint64_t ours_only_dist = 0;  // we: visible, Umbra: op 50 without op 48 (UE3's draw distance skipped it)
  uint64_t ours_only_culled = 0;  // we: visible, Umbra: never asked (its frustum / occlusion / cells culled it)
  uint64_t culled_by_code[8] = {};  // those by our code (1 visible, 3 near, 6 no bounds)
  uint64_t umbra_noflag = 0;         // op 48 instances that Umbra never asked op 50 for
  uint64_t filtered48 = 0, filtered_cmds = 0;  // me2_native_vis_filter: op 48 reports dropped, all reports dropped
  uint64_t native_queries = 0, native_emitted = 0, native_skipped = 0, native_ns = 0, frustum_kept = 0;
  uint64_t umbra_trav_queries = 0, umbra_trav_ns = 0;  // Umbra's own traversal (sub_82DFD790) in Umbra queries
  uint64_t umbra_handler_ns = 0, starve_ns = 0, own_ns = 0;  // handlers 50/48/144 inside Umbra's traversal; the starved
                                                             // traversal; our reports (handlers + masks)
  uint64_t ours_tests[8] = {};
  std::chrono::steady_clock::time_point start{};
  uint64_t frames0 = 0;
  std::string examples;  // a few Umbra-only instances that we called occluded
  int nexamples = 0;
} g_nvs;
float NvLF(const uint8_t* base, uint32_t va) {
  const uint32_t v = RecL32(base, va);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
// The batch planes (predicated tiling: the world plane through the tile split). Umbra's traversal sets them per query
// (sub_82DE87D0 -> sub_82DF06D8: each source plane at 0x8339B1D0, count at 0x8339AFC4, transformed into the plane set at
// 0x8339AFC8 that sub_82DE5318 tests each instance's box against for its batch mask). The native traversal has to set
// them itself: the source plane (camera space) through the view matrix UE3 hands Umbra (view + 64), normalised.
constexpr uint32_t kUmbraBoxTable = 0x833960FCu;  // Umbra's per-instance world AABBs (see NvBegin)
constexpr uint32_t kPlaneCount = 0x8339AFC4u, kPlaneSet = 0x8339AFC8u, kPlaneSource = 0x8339B1D0u;
uint32_t g_nv_view = 0;  // the view of the current sub_8274AA38 call
// Diagnostics per mode ([0] Umbra's traversal, [1] native): op 48 batch masks, op 164 flags at entry and draws.
struct NvModeDiag {
  uint64_t op48 = 0, mask[4] = {}, mask_other = 0;  // masks 0..3, other values
  uint64_t op164 = 0, op164_flag_set = 0, op164_draws = 0, op164_drawing = 0;
};
NvModeDiag g_nvd[2];
// The starved traversal (2026-11-05, after the first native try drew nothing: Umbra's delivery sub_82DF08D8 ->
// sub_82DECC28 sends the drawing commands 162 / 164 / 165 from state its traversal builds, so the traversal must run).
// While g_nv_starve: Umbra's own reports (50 / 48 / 144) and test boxes (128 / 129 / 131) are dropped and its result
// reads (130) answered "hidden" at once, so it ends after its first round; t_nv_own marks our own commands.
bool g_nv_starve = false;
bool g_nv_in_umbra = false;  // inside Umbra's own (timed) traversal
thread_local bool t_nv_own = false;
uint64_t g_starve_dropped = 0, g_starve_hidden = 0;
double g_plane_err = 0;  // the largest difference to Umbra's own plane so far this window (Umbra queries)
uint64_t g_plane_checks = 0;
// Our world planes (up to 8) into out[k][4]; the count, or -1 without a view.
int NvPlanes(const uint8_t* base, float out[8][4]) {
  if (!RecOk(g_nv_view)) return -1;
  const int n = int(std::min<uint32_t>(RecL32(base, kPlaneCount), 8));
  float v[16];
  for (int i = 0; i < 16; ++i) v[i] = NvLF(base, g_nv_view + 64 + 4 * uint32_t(i));
  for (int k = 0; k < n; ++k) {
    float sp[4];
    for (int c = 0; c < 4; ++c) sp[c] = NvLF(base, kPlaneSource + 16 * uint32_t(k) + 4 * uint32_t(c));
    float w[4];
    for (int i = 0; i < 3; ++i) w[i] = v[i * 4 + 0] * sp[0] + v[i * 4 + 1] * sp[1] + v[i * 4 + 2] * sp[2];
    w[3] = sp[3] + v[12] * sp[0] + v[13] * sp[1] + v[14] * sp[2];
    const float len = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    for (int c = 0; c < 4; ++c) out[k][c] = len > 0 ? w[c] / len : w[c];
  }
  return n;
}
void NvCheckPlanes(const uint8_t* base) {
  float ours[8][4];
  const int n = NvPlanes(base, ours);
  for (int k = 0; k < n; ++k)
    for (int c = 0; c < 4; ++c) {
      const float um = NvLF(base, kPlaneSet + 16 * uint32_t(k) + 4 * uint32_t(c));
      const double scale = c == 3 ? std::max(1.0, std::fabs(double(um))) : 1.0;
      g_plane_err = std::max(g_plane_err, std::fabs(double(ours[k][c]) - double(um)) / scale);
    }
  if (n > 0) ++g_plane_checks;
}
void NvBegin(const uint8_t* base, uint32_t commander) {
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const uint32_t count = RecL32(base, kUmbraInstanceTable), data = RecL32(base, kUmbraInstanceTable + 4);
  g_nv.active = false;
  if (count == 0 || count > 65536 || !RecOk(data)) return;
  const float probe_mn[3] = {0, 0, 0}, probe_mx[3] = {0, 0, 0};
  if (Me2RasterTestBox(probe_mn, probe_mx, frame) == 0) {
    ++g_nvs.no_buffer;
    return;
  }
  // The scene op 48 files instances into ([commander + 44]); its compact primitive array (+1280 data, +1284 count,
  // 56 bytes, +0 the primitive): an instance is live for this query only if its primitive is that array's entry at the
  // primitive's own index (+32). 2026-11-05: a main-menu run crashed in op 48 on a slot whose primitive was gone.
  const uint32_t scene = RecOk(commander) ? RecL32(base, commander + 44) : 0;
  const uint32_t compact = RecOk(scene) ? RecL32(base, scene + 1280) : 0;
  const uint32_t ncompact = RecOk(scene) ? RecL32(base, scene + 1284) : 0;
  if (!RecOk(compact) || ncompact == 0 || ncompact > 1000000) return;
  const auto t0 = std::chrono::steady_clock::now();
  g_nv.active = true;
  g_nv.count = count;
  g_nv.ours.assign(count, 0);
  g_nv.umbra.assign(count, 0);
  ++g_nvs.passes;
  // 2026-11-05 (console: 3.6 ms a query for ~4200 instances, mostly cache misses of the per-instance pointer chase):
  // the box (Umbra's own per-instance world AABB table, 0x833960FC: +0 count, +4 data, 24 bytes = min xyz, max xyz;
  // what sub_82DE5318 tests) and the wrapper table are read in order; the frustum / depth test comes first, and only an
  // instance it keeps gets the liveness checks (its primitive, proxy, and the scene's compact entry at the primitive's
  // own index; an instance failing them is never reported: the first native run crashed in op 48 on a dead slot).
  const uint32_t boxes = RecL32(base, kUmbraBoxTable + 4), nboxes = RecL32(base, kUmbraBoxTable);
  const bool boxes_ok = RecOk(boxes) && nboxes <= 65536;
  // me2_native_vis_worker: the worker's codes for every instance whose wrapper and box are unchanged since its copy.
  const uint8_t* w_boxes = nullptr;
  const uint32_t* w_wraps = nullptr;
  const uint8_t* w_codes = nullptr;
  uint32_t w_count = 0;
  uint64_t w_ns = 0;
  const bool worker = boxes_ok && Me2RasterBoxPass(frame, &w_boxes, &w_wraps, &w_codes, &w_count, &w_ns);
  if (worker) ++g_nvs.worker_passes, g_nvs.worker_ns += w_ns;
  for (uint32_t i = 0; i < count; ++i) {
    if (worker && i < w_count && i < nboxes && std::memcmp(&w_wraps[i], base + data + 4 * i, 4) == 0 &&
        std::memcmp(w_boxes + size_t(i) * 24, base + boxes + 24 * i, 24) == 0) {
      uint8_t code = w_codes[i];
      ++g_nvs.worker_reused;
      if (code == 1 || code == 3) {
        const uint32_t w = RecL32(base, data + 4 * i);
        const uint32_t psi = RecL32(base, w);
        const uint32_t proxy = RecOk(psi) ? RecL32(base, psi + 8) : 0;
        const uint32_t pidx = RecOk(psi) ? RecL32(base, psi + 32) : 0xFFFFFFFFu;
        if (!RecOk(psi)) code = 7;
        else if (!RecOk(proxy) || !RecOk(RecL32(base, proxy)) || pidx >= ncompact ||
                 RecL32(base, compact + 56 * pidx) != psi)
          code = 6;
      }
      g_nv.ours[i] = code;
      continue;
    }
    if (worker) ++g_nvs.worker_retested;
    const uint32_t w = RecL32(base, data + 4 * i);
    if (!RecOk(w) || !boxes_ok || i >= nboxes) {
      g_nv.ours[i] = RecOk(w) ? 6 : 7;
      continue;
    }
    float mn[3], mx[3];
    bool ok = true;
    for (int k = 0; k < 3; ++k) mn[k] = NvLF(base, boxes + 24 * i + 4 * k), mx[k] = NvLF(base, boxes + 24 * i + 12 + 4 * k);
    for (int k = 0; k < 3; ++k) ok = ok && std::isfinite(mn[k]) && std::isfinite(mx[k]) && mn[k] <= mx[k];
    if (!ok) {
      g_nv.ours[i] = 6;
      continue;
    }
    uint8_t code = Me2RasterTestBox(mn, mx, frame);
    if (code == 1 || code == 3) {
      const uint32_t psi = RecL32(base, w);
      const uint32_t proxy = RecOk(psi) ? RecL32(base, psi + 8) : 0;
      const uint32_t pidx = RecOk(psi) ? RecL32(base, psi + 32) : 0xFFFFFFFFu;
      if (!RecOk(psi)) code = 7;
      else if (!RecOk(proxy) || !RecOk(RecL32(base, proxy)) || pidx >= ncompact ||
               RecL32(base, compact + 56 * pidx) != psi)
        code = 6;
    }
    g_nv.ours[i] = code;
  }
  g_nvs.ns += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}
// Returns true when the command is to be dropped (me2_native_vis_filter).
bool NvNote(PPCContext& ctx, uint8_t* base, uint32_t op) {
  if (!g_nv.active || (op != 48 && op != 50 && op != 144)) return false;
  const uint32_t self = ctx.r3.u32;
  const uint32_t cmd = RecOk(self) ? RecL32(base, self + 4) : 0;
  if (!RecOk(cmd)) return false;
  const int32_t inst = int32_t(RecL32(base, cmd + 48));
  if (inst < 0 || uint32_t(inst) >= g_nv.count) return false;
  g_nv.umbra[size_t(inst)] |= op == 50 ? 1 : op == 48 ? 2 : 4;
  if (op == 48) {
    NvModeDiag& dg = g_nvd[g_nv.native ? 1 : 0];
    ++dg.op48;
    const uint32_t mt = RecL32(base, 0x83396134u);
    const uint32_t m = RecOk(mt) ? RecL32(base, mt + 4u * uint32_t(inst)) : 0xFFFFFFFFu;
    if (m < 4) ++dg.mask[m];
    else ++dg.mask_other;
  }
  static const bool filter = REXCVAR_GET(me2_native_vis_filter);
  if (!filter || g_nv.ours[size_t(inst)] != 2 || Me2AbOff(Me2Ab::kNativeVis)) return false;
  ++g_nvs.filtered_cmds;
  if (op == 48) ++g_nvs.filtered48;
  return true;
}
void NvReport(std::chrono::steady_clock::time_point now) {
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const double q = double(std::max<uint64_t>(1, g_nvs.queries));
  REXLOG_INFO("[native-vis] 10 s: {} queries compared ({} without a raster buffer), {:.0f} instances a query; Umbra "
              "visible {:.1f}, ours {:.1f}, both {:.1f} a query; native pass {:.0f} us a query",
              g_nvs.queries, g_nvs.no_buffer, g_nvs.instances / q, g_nvs.umbra_vis / q, g_nvs.ours_vis / q,
              g_nvs.both / q, double(g_nvs.ns) / 1e3 / double(std::max<uint64_t>(1, g_nvs.passes)));
  REXLOG_INFO("[native-vis]   Umbra visible but not ours (would vanish): occluded {:.2f}, outside the frustum {:.2f}, "
              "other {:.2f} a query | ours visible but not Umbra's: draw-distance skip {:.1f}, Umbra culled {:.1f} a "
              "query | our codes a query: visible {:.0f} occluded {:.0f} near {:.0f} frustum {:.0f} no bounds {:.0f} "
              "no primitive {:.0f}",
              g_nvs.umbra_only[2] / q, g_nvs.umbra_only[5] / q,
              (g_nvs.umbra_only[0] + g_nvs.umbra_only[6] + g_nvs.umbra_only[7]) / q, g_nvs.ours_only_dist / q,
              g_nvs.ours_only_culled / q, g_nvs.ours_tests[1] / q, g_nvs.ours_tests[2] / q, g_nvs.ours_tests[3] / q,
              g_nvs.ours_tests[5] / q, g_nvs.ours_tests[6] / q, g_nvs.ours_tests[7] / q);
  REXLOG_INFO("[native-vis]   ours visible, Umbra culled, by our code: visible {:.1f}, near plane {:.1f}, no bounds "
              "{:.1f} a query; op 48 without op 50 {:.1f} a query",
              g_nvs.culled_by_code[1] / q, g_nvs.culled_by_code[3] / q, g_nvs.culled_by_code[6] / q,
              g_nvs.umbra_noflag / q);
  for (int md = 0; md < 2; ++md) {
    const NvModeDiag& dg = g_nvd[md];
    if (!dg.op48 && !dg.op164) continue;
    REXLOG_INFO("[native-vis]   {}: op 48 x{} masks 0:{} 1:{} 2:{} 3:{} other {}; op 164 x{}, {} with its batch flag "
                "set at entry, {} drew ({} draws)",
                md ? "native" : "Umbra ", dg.op48, dg.mask[0], dg.mask[1], dg.mask[2], dg.mask[3], dg.mask_other,
                dg.op164, dg.op164_flag_set, dg.op164_drawing, dg.op164_draws);
  }
  g_nvd[0] = g_nvd[1] = NvModeDiag{};
  REXLOG_INFO("[native-vis]   batch planes: {} checks, largest difference ours vs Umbra's {:.5f} (normal; d relative)",
              g_plane_checks, g_plane_err);
  g_plane_err = 0;
  g_plane_checks = 0;
  if (g_starve_dropped || g_starve_hidden)
    REXLOG_INFO("[native-vis]   starved Umbra: {} of its commands dropped, {} test reads answered hidden",
                g_starve_dropped, g_starve_hidden);
  g_starve_dropped = g_starve_hidden = 0;
  if (g_nvs.worker_passes)
    REXLOG_INFO("[native-vis]   worker box pass: {} of {} passes ready in time, {:.0f} us a pass on core 3; {:.1f} "
                "instances a pass taken from it, {:.1f} re-tested (moved since)",
                g_nvs.worker_passes, g_nvs.passes, double(g_nvs.worker_ns) / 1e3 / double(g_nvs.worker_passes),
                double(g_nvs.worker_reused) / double(g_nvs.worker_passes),
                double(g_nvs.worker_retested) / double(g_nvs.worker_passes));
  if (g_nvs.umbra_trav_queries)
    REXLOG_INFO("[native-vis]   UMBRA traversal: {} queries, {:.0f} us a query (its tests answered by the rasterizer "
                "included); of it UE3's handlers of its reports (50 / 48 / 144) {:.0f} us",
                g_nvs.umbra_trav_queries, double(g_nvs.umbra_trav_ns) / 1e3 / double(g_nvs.umbra_trav_queries),
                double(g_nvs.umbra_handler_ns) / 1e3 / double(g_nvs.umbra_trav_queries));
  if (g_nvs.native_queries)
    REXLOG_INFO("[native-vis]   NATIVE split: starved Umbra {:.0f} us, our reports (masks + handlers) {:.0f} us a query",
                double(g_nvs.starve_ns) / 1e3 / double(g_nvs.native_queries),
                double(g_nvs.own_ns) / 1e3 / double(g_nvs.native_queries));
  if (g_nvs.native_queries)
    REXLOG_INFO("[native-vis]   NATIVE traversal: {} queries, {:.1f} instances sent a query ({:.1f} skipped by the draw "
                "distance), {:.0f} us a query for the commands; {:.1f} frustum-rejected kept for far bounds a query",
                g_nvs.native_queries, double(g_nvs.native_emitted) / double(g_nvs.native_queries),
                double(g_nvs.native_skipped) / double(g_nvs.native_queries),
                double(g_nvs.native_ns) / 1e3 / double(g_nvs.native_queries),
                double(g_nvs.frustum_kept) / double(std::max<uint64_t>(1, g_nvs.native_queries + g_nvs.queries)));
  if (g_nvs.filtered_cmds)
    REXLOG_INFO("[native-vis]   filter: {:.1f} visible reports (op 48) dropped a query ({} commands in all)",
                g_nvs.filtered48 / q, g_nvs.filtered_cmds);
  if (!g_nvs.examples.empty()) REXLOG_INFO("[native-vis]   Umbra-visible, our occluded / frustum:{}", g_nvs.examples);
  (void)frame;
  g_nvs = NvStats{};
  g_nvs.start = now;
}
void NvReportMaybe();
void NvEnd(const uint8_t* base) {
  if (!g_nv.active) return;
  g_nv.active = false;
  if (g_nv.native) {
    g_nv.native = false;
    NvReportMaybe();
    return;
  }
  NvCheckPlanes(base);
  ++g_nvs.queries;
  g_nvs.instances += g_nv.count;
  const uint32_t data = RecL32(base, kUmbraInstanceTable + 4);
  for (uint32_t i = 0; i < g_nv.count; ++i) {
    const uint8_t c = g_nv.ours[i];
    const bool ours = c == 1 || c == 3;
    const bool um = (g_nv.umbra[i] & 2) != 0;
    ++g_nvs.ours_tests[c == 8 ? 1 : (c & 7)];
    g_nvs.umbra_vis += um;
    g_nvs.ours_vis += ours;
    g_nvs.both += um && ours;
    if (um && !ours) {
      ++g_nvs.umbra_only[c & 7];
      if ((c == 2 || c == 5) && g_nvs.nexamples < 6) {
        ++g_nvs.nexamples;
        const uint32_t w = RecL32(base, data + 4 * i);
        const uint32_t psi = RecOk(w) ? RecL32(base, w) : 0;
        float o[3] = {}, e[3] = {};
        if (RecOk(psi))
          for (int k = 0; k < 3; ++k) o[k] = NvLF(base, psi + 48 + 4 * k), e[k] = NvLF(base, psi + 60 + 4 * k);
        g_nvs.examples += fmt::format(" {}:{}/psi {:08X} r {:.0f} o ({:.0f} {:.0f} {:.0f}) e ({:.0f} {:.0f} {:.0f}) "
                                      "proxy vt {:08X}",
                                      i, c == 2 ? "occ" : "frustum", psi, RecOk(psi) ? NvLF(base, psi + 72) : 0.0f,
                                      o[0], o[1], o[2], e[0], e[1], e[2],
                                      RecOk(psi) && RecOk(RecL32(base, psi + 8)) ? RecL32(base, RecL32(base, psi + 8)) : 0);
      }
    }
    if (ours && !um) {
      if (g_nv.umbra[i] & 1) ++g_nvs.ours_only_dist;
      else ++g_nvs.ours_only_culled, ++g_nvs.culled_by_code[c & 7];
    }
    if (um && !(g_nv.umbra[i] & 1)) ++g_nvs.umbra_noflag;
  }
  NvReportMaybe();
}
void NvReportMaybe() {
  const auto now = std::chrono::steady_clock::now();
  if (g_nvs.start == std::chrono::steady_clock::time_point{}) g_nvs.start = now;
  else if (now - g_nvs.start >= std::chrono::seconds(10)) NvReport(now);
}
}  // namespace

REX_HOOK_RAW(sub_827486B8) {
  static const bool diag = REXCVAR_GET(me2_umbra_cmd_diag);
  static const bool nv = REXCVAR_GET(me2_native_vis_shadow);
  const uint32_t rec_op = ctx.r4.u32 & 255;
  if (g_nv_starve && !t_nv_own) {
    if (rec_op == 50 || rec_op == 48 || rec_op == 144 || rec_op == 128 || rec_op == 129 || rec_op == 131) {
      ++g_starve_dropped;
      return;
    }
    if (rec_op == 130) {
      const uint32_t self = ctx.r3.u32;
      const uint32_t cmd = RecOk(self) ? RecL32(base, self + 4) : 0;
      const uint32_t rec = RecOk(cmd) ? RecL32(base, cmd + 52) : 0;
      if (RecOk(rec)) {
        base[rec + 4] = 1;  // available
        std::memset(base + rec + 8, 0, 4);  // 0 pixels: hidden
        ++g_starve_hidden;
        return;
      }
    }
  }
  if (g_nv_in_umbra && (rec_op == 50 || rec_op == 48 || rec_op == 144)) {
    const auto h0 = std::chrono::steady_clock::now();
    g_nv_in_umbra = false;
    NvNote(ctx, base, rec_op);
    ++g_rec_depth;
    UmbraCommandHook(ctx, base);
    --g_rec_depth;
    g_nv_in_umbra = true;
    g_nvs.umbra_handler_ns += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - h0).count());
    return;
  }
  if (nv) {
    if (rec_op == 0 && g_rec_depth == 0) NvBegin(base, ctx.r3.u32);
    // (our own native reports need no notes: 0.5 % of the console's render thread went to them, 2026-11-05)
    if (!t_nv_own && NvNote(ctx, base, rec_op)) return;  // filtered: the command is not passed on
    if (rec_op == 164 && g_nv.active) {
      NvModeDiag& dg = g_nvd[g_nv.native ? 1 : 0];
      ++dg.op164;
      // the flags op 164 tests: 0x8340655C[0 / 1] (it picks one by Umbra's current batch)
      if (RecL32(base, 0x8340655Cu) || RecL32(base, 0x83406560u)) ++dg.op164_flag_set;
      const uint64_t d0 = g_me2_draws_total.load(std::memory_order_relaxed);
      ++g_rec_depth;
      UmbraCommandHook(ctx, base);
      --g_rec_depth;
      const uint64_t d = g_me2_draws_total.load(std::memory_order_relaxed) - d0;
      dg.op164_draws += d;
      dg.op164_drawing += d ? 1 : 0;
      return;
    }
    if (rec_op == 1 && g_rec_depth == 0) {
      ++g_rec_depth;
      UmbraCommandHook(ctx, base);
      --g_rec_depth;
      NvEnd(base);
      return;
    }
  }
  const int rec = RecBefore(ctx, base, rec_op);
  if (rec >= 0) {
    const uint64_t d0 = g_me2_draws_total.load(std::memory_order_relaxed);
    ++g_rec_depth;
    if (!diag || g_cmd_depth >= 63) UmbraCommandHook(ctx, base);
    else {
      g_cmd_stack[++g_cmd_depth] = UmbraCmdFrame{};
      UmbraCommandHook(ctx, base);
      --g_cmd_depth;
    }
    --g_rec_depth;
    if (size_t(rec) < g_rec.size()) g_rec[size_t(rec)].draws = uint32_t(g_me2_draws_total.load(std::memory_order_relaxed) - d0);
    if (rec_op == 1 && g_rec_depth == 0 && g_rec_active) {
      g_rec_active = false;
      RecDump(base);
    }
    return;
  }
  if (!diag || g_cmd_depth >= 63) {
    ++g_rec_depth;
    UmbraCommandHook(ctx, base);
    --g_rec_depth;
    return;
  }
  const uint32_t op = ctx.r4.u32 & 255;
  const auto t0 = std::chrono::steady_clock::now();
  if (g_cmd_window == std::chrono::steady_clock::time_point{}) {
    g_cmd_window = t0;
    g_cmd_frames0 = g_sa2_game_frames.load(std::memory_order_relaxed);
    g_cmd_all0 = g_me2_draws_total.load(std::memory_order_relaxed);
  }
  const uint64_t d0 = g_me2_draws_total.load(std::memory_order_relaxed);
  g_cmd_stack[++g_cmd_depth] = UmbraCmdFrame{};
  UmbraCommandHook(ctx, base);
  const UmbraCmdFrame mine = g_cmd_stack[g_cmd_depth--];
  const auto t1 = std::chrono::steady_clock::now();
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  const uint64_t d = g_me2_draws_total.load(std::memory_order_relaxed) - d0;
  UmbraCmdStat& c = g_cmd[op];
  ++c.calls;
  c.ns += ns - std::min(ns, mine.child_ns);
  c.draws += d - std::min(d, mine.child_draws);
  if (g_cmd_depth > 0) {
    g_cmd_stack[g_cmd_depth].child_ns += ns;
    g_cmd_stack[g_cmd_depth].child_draws += d;
  } else if (t1 - g_cmd_window >= std::chrono::seconds(10)) {
    UmbraCmdReport(t1);
  }
}

// sub_8274AA38(r3, r4 = view, ...): UE3's per-view Umbra visibility (camera setup + resolveVisibility); its view +64
// is the view matrix it hands Umbra (copied, inverted, set as the camera transform). Kept per frame for the latent
// answers' camera check.
REX_EXTERN(__imp__sub_8274AA38);
REXCVAR_DEFINE_INT32(me2_occlusion_view_dump, 0, "ME2",
                     "Diagnostic: every this many game frames, log the first 768 bytes of each view sub_8274AA38 gets "
                     "in that frame (as floats; [view-dump]); 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REX_HOOK_RAW(sub_8274AA38) {
  g_nv_view = ctx.r4.u32;  // native visibility: the view whose matrix Umbra gets as its camera
  if (static const int32_t dump_every = REXCVAR_GET(me2_occlusion_view_dump); dump_every > 0 && ctx.r4.u32) {
    const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
    if (frame % uint64_t(dump_every) == 0) {
      std::string rows;
      for (uint32_t row = 0; row < 48; ++row) {
        rows += fmt::format("\n  +{:3d}:", row * 16);
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t bits = LoadBE(base, ctx.r4.u32 + row * 16 + c * 4);
          float f;
          std::memcpy(&f, &bits, 4);
          rows += fmt::format(" {:12.5g} ({:08x})", f, bits);
        }
      }
      REXLOG_INFO("[view-dump] frame {} r3 {:08x} view {:08x}:{}", frame, ctx.r3.u32, ctx.r4.u32, rows);
    }
  }
  if ((UmbraLatent() || CutVisibleFrames() > 0) && ctx.r4.u32) {
    const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
    Me2OcclusionView current;
    for (unsigned i = 0; i < 16; ++i) {
      const uint32_t view_bits = LoadBE(base, ctx.r4.u32 + 64 + 4 * i);
      const uint32_t projection_bits = LoadBE(base, ctx.r4.u32 + 128 + 4 * i);
      std::memcpy(&current.view[i], &view_bits, 4);
      std::memcpy(&current.projection[i], &projection_bits, 4);
    }
    static const float cut_rot = float(REXCVAR_GET(me2_occlusion_cut_rot));
    g_latent_view.rot_cut = cut_rot;  // me2-nx-deko (2026-11-04)
    const unsigned reason = g_latent_view.Observe(current, frame);
    ViewAt& v = g_view_hist[frame & (kViewHist - 1)];
    v.frame = frame;
    std::memcpy(v.m, current.view, sizeof(v.m));
    std::memcpy(v.p, current.projection, sizeof(v.p));
    if (reason) {
      // No table clear and no GPU wait. Epoch tags also reject late old-view
      // results and prevent two negative candidates spanning a camera cut.
      // me2_occlusion_cut_visible_frames: a cut is the camera turning or moving at once (or a frame gap), not a
      // projection change alone (run 6: 1,424 of 1,714 detections, zooms / FOV moves with the camera in place).
      if (reason & (Me2OcclusionViewHistory::kRotation | Me2OcclusionViewHistory::kPosition |
                    Me2OcclusionViewHistory::kGap))
        g_cut_frame.store(frame, std::memory_order_relaxed);
      static uint64_t window = 0;
      static unsigned lines = 0;
      if (window != frame / 300) { window = frame / 300; lines = 0; }
      if (lines++ < 20)
        REXLOG_INFO("[occlusion-camera] frame {} epoch {} reason {}: prior-view answers invalidated; {} reads "
                    "answered visible after the cuts so far (me2_occlusion_cut_visible_frames {})",
                    frame, g_latent_view.epoch, reason, g_cut_overrides.load(std::memory_order_relaxed),
                    CutVisibleFrames());
    }
  }
  if (CpuMode() == 2) g_cpu_slot_on.store(!Me2AbOff(Me2Ab::kRaster), std::memory_order_relaxed);
  if (UmbraLatent() && ctx.r4.u32 && CpuMode() && CpuSource() == 1 && CpuSlotOn()) {
    // me2_occlusion_cpu_source 1: the occluders drawn with this view's view-projection (view +256) before Umbra's
    // traversal; r6 = the FScene (sub_8274AF90 passes [renderer +0]).
    const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
    float vp[16];
    for (int i = 0; i < 16; ++i) {
      const uint32_t bits = LoadBE(base, ctx.r4.u32 + 256 + 4 * i);
      std::memcpy(&vp[i], &bits, 4);
    }
    float cam[3] = {};
    const ViewAt& v = g_view_hist[frame & (kViewHist - 1)];
    if (v.frame == frame)
      CameraPos(v.m, cam);
    Me2RasterFrame(base, ctx.r6.u32, vp, cam, frame, ctx.r7.u32);  // r7 = the FSceneRenderer (sub_8274AF90)
    if (CpuAnswers()) g_raster_view_frame.store(frame, std::memory_order_relaxed);
  } else if (UmbraLatent() && ctx.r4.u32 && CpuMode() && CpuSource() == 0 && CpuSlotOn()) {
    // me2_occlusion_umbra_cpu: this frame's view-projection (view +256) to the render thread (its depth grid is taken
    // with it) and the newest finished grid reprojected into it, before Umbra's traversal tests anything.
    if (!renut::gpu::g_occlusion_depth_on) {
      const int32_t g = REXCVAR_GET(me2_occlusion_cpu_grid);
      renut::gpu::g_occlusion_grid_w = g == 2 ? 256 : g == 0 ? 64 : 128;
      renut::gpu::g_occlusion_grid_h = g == 2 ? 144 : g == 0 ? 36 : 72;
      renut::gpu::g_occlusion_capture_resolve = uint32_t(REXCVAR_GET(me2_occlusion_cpu_capture));
      g_cpu_info_text = CpuInfoText;
      renut::gpu::g_occlusion_depth_on = true;
    }
    const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
    std::array<float, 16> vp;
    for (int i = 0; i < 16; ++i) {
      const uint32_t bits = LoadBE(base, ctx.r4.u32 + 256 + 4 * i);
      std::memcpy(&vp[i], &bits, 4);
    }
    render_thread::Queue([vp, frame] { renut::gpu::OcclusionDepthViewLocked(vp.data(), frame); });
    CpuBuildFrame(vp.data(), frame);
  }
  __imp__sub_8274AA38(ctx, base);
}

REX_EXTERN(__imp__sub_8269B220);
REX_HOOK_RAW(sub_8269B220) {  // InitViews (the render stage executor's caller of the static-mesh visibility loop)
  st_init_views.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_8269B220(ctx, base);
}

// The interaction target's position for the [umbra-door] lines (me2_selection.cpp, game thread).
extern "C" void me2_occlusion_watch(int32_t x, int32_t y, int32_t z, uint64_t frame) {
  g_watch_pos[0].store(x, std::memory_order_relaxed);
  g_watch_pos[1].store(y, std::memory_order_relaxed);
  g_watch_pos[2].store(z, std::memory_order_relaxed);
  g_watch_frame.store(frame, std::memory_order_relaxed);
}

// The camera position of the newest view Umbra got (sub_8274AA38), for me2_prepass_small_factor (me2_cull_distance.cpp).
// UE3 render thread. False when no view of this or the previous frame is kept.
extern "C" bool me2_occlusion_camera(float out[3]) {
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  for (uint64_t f : {frame, frame - 1}) {
    const ViewAt& v = g_view_hist[f & (kViewHist - 1)];
    if (v.frame == f) {
      CameraPos(v.m, out);
      return true;
    }
  }
  return false;
}

// Step 3: Umbra's traversal (sub_82DFD790(query r3, f1)), replaced by the native visible set when me2_native_vis is on,
// the shadow set of this query is ready and the A/B slot does not give it back to Umbra.
REX_EXTERN(__imp__sub_82DFD790);
REX_EXTERN(sub_82DE5318);  // Umbra: an instance's batch mask (its box against the batch planes)
REX_HOOK_RAW(sub_82DFD790) {
  static const bool on = REXCVAR_GET(me2_native_vis);
  if (!on || !g_nv.active || Me2AbOff(Me2Ab::kNativeVis)) {
    // Umbra's traversal, timed when the shadow set is computed (the cost the native one replaces).
    const bool timed = g_nv.active;
    const auto u0 = std::chrono::steady_clock::now();
    g_nv_in_umbra = timed;
    __imp__sub_82DFD790(ctx, base);
    g_nv_in_umbra = false;
    if (timed) {
      ++g_nvs.umbra_trav_queries;
      g_nvs.umbra_trav_ns += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - u0).count());
    }
    return;
  }
  const uint32_t query = ctx.r3.u32;
  const uint32_t commander = RecOk(query) ? RecL32(base, query + 432) : 0;
  const uint32_t cmd = RecOk(commander) ? RecL32(base, commander + 4) : 0;
  if (!RecOk(cmd)) {
    __imp__sub_82DFD790(ctx, base);
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  g_nv.native = true;
  ++g_nvs.native_queries;
  const uint32_t sp = ctx.r1.u32;
  // Umbra's traversal, starved: it sets the query's batch planes and the delivery state, reports nothing.
  g_nv_starve = true;
  __imp__sub_82DFD790(ctx, base);
  g_nv_starve = false;
  const auto t1 = std::chrono::steady_clock::now();
  g_nvs.starve_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  ctx.r1.u32 = sp;
  auto send = [&](uint32_t op) {
    ctx.r1.u32 = sp;
    ctx.r3.u64 = commander;
    ctx.r4.u64 = op;
    t_nv_own = true;
    sub_827486B8(ctx, base);
    t_nv_own = false;
  };
  auto st32 = [&](uint32_t va, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + va, &v, 4);
  };
  // (the batch planes: set by the starved traversal itself)
  for (uint32_t i = 0; i < g_nv.count; ++i) {
    const uint8_t c = g_nv.ours[i];
    if (!(c == 1 || c == 3)) continue;
    // The instance's batch mask, as the traversal computes it (against the planes just set).
    ctx.r1.u32 = sp;
    ctx.r3.u64 = i;
    sub_82DE5318(ctx, base);
    st32(cmd + 48, i);
    base[cmd + 108] = 0;
    send(50);
    ++g_nvs.native_emitted;
    if (base[cmd + 108]) {
      base[cmd + 108] = 0;
      ++g_nvs.native_skipped;
      continue;
    }
    send(48);
    send(144);
  }
  st32(cmd + 48, 0xFFFFFFFFu);
  ctx.r1.u32 = sp;
  g_nvs.own_ns += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t1).count());
  g_nvs.native_ns += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}
