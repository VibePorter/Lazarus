// me2-nx-deko (2026-11-06): draw-setup replay, stage 1 (compare mode). While UE3's per-mesh base-pass state setup
// (SetMeshRenderState, five variants) runs, the calls it makes into the low-level setters are recorded (constant
// writes, rasterizer state, vertex factory, matrix setter) and compared with the same mesh's previous record. See
// me2_replay.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace me2::replay {

// Non-null while a SetMeshRenderState call is being recorded on this thread.
extern thread_local void* t_rec;
// Nesting of observed setters: only the outermost one is recorded (a setter that calls another counts once).
extern thread_local int t_depth;

void Note(uint32_t fn, const PPCContext& ctx, const uint8_t* base);

struct Scope {
  bool on;
  Scope(bool o) : on(o) {}  // NOLINT: implicit, for `return {false}`
  Scope(const Scope&) = delete;
  ~Scope() {
    if (on) --t_depth;
  }
};
// At the entry of an observed setter's hook: records the call (its arguments and the data they point to) when a
// SetMeshRenderState is being recorded and no other observed setter is running; keep the result alive for the call.
inline Scope Enter(uint32_t fn, const PPCContext& ctx, const uint8_t* base) {
  if (!t_rec) return {false};
  if (t_depth == 0) Note(fn, ctx, base);
  ++t_depth;
  return {true};
}

// The observed setters (Note decodes exactly these).
constexpr bool Observed(uint32_t a) {
  return a == 0x82A92C50u || a == 0x82A92D28u || a == 0x822EB710u || a == 0x822C8C10u || a == 0x822C8AC8u ||
         a == 0x827E16A8u || a == 0x8266F820u;
}
// For generic hook macros: nothing at all for an address that is not observed.
template <uint32_t A>
inline Scope EnterAt(const PPCContext& ctx, const uint8_t* base) {
  if constexpr (Observed(A)) {
    return Enter(A, ctx, base);
  } else {
    (void)ctx, (void)base;
    return {false};
  }
}

}  // namespace me2::replay
