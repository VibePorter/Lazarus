// ME2 (2026-11-12, backported from the Dunwall port): shader constant blocks.
//
// Besides the device's constant table, an Xbox 360 shader carries constant data in its GPU memory, next to its code.
// At every draw Direct3D's shader flush (TU2 sub_82AA7408) calls sub_82AA7188(device, table, physical base) for the
// bound pixel shader (table = shader + 40, base = [shader + 24]) and vertex shader (shader + 872, [shader + 32]). That
// function writes the table's blocks into the command buffer, either inline (a SET_CONSTANT packet with the data copied
// from the shader's memory) or as LOAD_ALU_CONSTANT packets (the GPU loads the registers from the shader's memory):
// either way the registers never go through the device's table. Renut reads only the device's table, so those
// constants reached the renderer as zeros or stale values (Dunwall: FXAA's thresholds and direction weights c3-c6,
// bright outlines along every edge).
//
// Table layout as sub_82AA7188 reads it (the same as Dunwall's sub_82B05208, XDK 21076): header at
// table + [table + 20] (0: no blocks); entries from header + 20, [header + 16] bytes of them: u16 first register (4-dword
// units), u16 count (dwords; 0 ends), u32 offset from the base. Here, before each draw reaches the renderer, the blocks
// are copied into the device's constant table (VS then PS float constants, contiguous from +1920 as the GPU numbers
// them: m_Constants +0x480, VertexShaderF +0x300) and their 4-register groups marked in m_Pending (+0: vertex, +8:
// pixel), as SetVertexShaderConstantF would.
//
// The base is a GPU address: sub_82AA7188 reads the inline copy's data at 0xC0000000 + (address & 0x1FFFFFFF), plus
// 0x1000 for an address in the 0xE0000000 window (the same mapping here).

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(me2_shader_constant_blocks, true, "ME2",
                    "Load each bound shader's own constant blocks (the console's LOAD_ALU_CONSTANT / inline constants "
                    "at the shader flush) into the device's constant table before a draw. Off: the renderer sees only "
                    "the table")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_shader_constant_blocks_log, 0, "ME2",
                     "Diagnostic: log the constant block tables of the first N distinct shaders ([me2-cblocks])");

namespace {
constexpr uint32_t kDevicePixelShader = 12700, kDeviceVertexShader = 12704;  // d3d.h kDeviceOffset*Shader (ME2)
constexpr uint32_t kDeviceFloatConstants = 0x480 + 0x300;                    // m_Constants.VertexShaderF (1920)
constexpr uint32_t kFloatRegisters = 512;

uint32_t Load32(const uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}
uint16_t Load16(const uint8_t* base, uint32_t va) {
  uint16_t v;
  std::memcpy(&v, base + va, 2);
  return __builtin_bswap16(v);
}
// sub_82AA7188's mapping of a GPU address to the guest address it copies from.
uint32_t GpuToGuest(uint32_t address) {
  return 0xC0000000u + (address & 0x1FFFFFFFu) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

void MarkPending(uint8_t* base, uint32_t device, uint32_t first_reg, uint32_t regs) {
  for (uint32_t r = first_reg; r < first_reg + regs && r < kFloatRegisters; r += 4) {
    const uint32_t word = r < 256 ? 0 : 1;  // m_Pending[0] vertex, [1] pixel float constants
    const uint32_t group = (r % 256) / 4;   // bit 63 - group, as the renderer reads it (queued.cpp)
    uint8_t* p = base + device + word * 8;
    uint64_t v;
    std::memcpy(&v, p, 8);
    v = __builtin_bswap64(__builtin_bswap64(v) | (1ull << (63 - group)));
    std::memcpy(p, &v, 8);
  }
}

void LoadConstantBlocks(uint8_t* base, uint32_t device, uint32_t shader, uint32_t table, uint32_t physical,
                        bool pixel) {
  if (!table || !physical) return;
  const uint32_t rel = Load32(base, table + 20);
  if (!rel) return;
  const uint32_t header = table + rel;
  uint32_t p = header + 20;
  const uint32_t end = p + Load32(base, header + 16);
  static const int32_t log_max = REXCVAR_GET(me2_shader_constant_blocks_log);
  static int32_t logged = 0;
  static uint32_t last_logged = 0;
  const bool log = log_max > 0 && logged < log_max && shader != last_logged;
  if (log) {
    last_logged = shader;
    ++logged;
  }
  for (int guard = 0; p < end && guard < 64; ++guard) {
    const uint32_t reg = Load16(base, p), count = Load16(base, p + 2);
    if (!count) break;
    const uint32_t offset = Load32(base, p + 4);
    p += 8;
    // The packet takes the register times 4 (a dword index into the 512-register file) and the count as it is: dwords.
    const uint32_t regs = (count + 3) / 4;
    const uint32_t from = GpuToGuest(physical + offset);
    if (log)
      REXLOG_INFO("[me2-cblocks] {} shader {:08X}: register {} count {} ({} registers) from {:08X} + {:X} ({:08X}); "
                  "first {:08X} {:08X} {:08X} {:08X}", pixel ? "pixel" : "vertex", shader, reg, count, regs, physical,
                  offset, from, Load32(base, from), Load32(base, from + 4), Load32(base, from + 8),
                  Load32(base, from + 12));
    if (reg >= kFloatRegisters || reg + regs > kFloatRegisters) continue;
    std::memcpy(base + device + kDeviceFloatConstants + reg * 16, base + from, count * 4);
    MarkPending(base, device, reg, regs);
  }
}
}  // namespace

// Called by the draw hooks (me2_hooks.cpp) and, through g_me2_draw_prepare, Renut's Begin*Vertices hooks.
extern "C" void Me2LoadShaderConstantBlocks(uint8_t* base, uint32_t device) {
  static const bool on = REXCVAR_GET(me2_shader_constant_blocks);
  if (!on || !device) return;
  if (const uint32_t ps = Load32(base, device + kDevicePixelShader))
    LoadConstantBlocks(base, device, ps, ps + 40, Load32(base, ps + 24), true);
  if (const uint32_t vs = Load32(base, device + kDeviceVertexShader))
    LoadConstantBlocks(base, device, vs, vs + 872, Load32(base, vs + 32), false);
}

// Renut's Begin*Vertices hooks call this (hooks/draw.cpp, null in a build without the game port's code).
extern void (*g_me2_draw_prepare)(uint8_t* base, uint32_t device);
static const bool g_me2_draw_prepare_set = (g_me2_draw_prepare = Me2LoadShaderConstantBlocks, true);
