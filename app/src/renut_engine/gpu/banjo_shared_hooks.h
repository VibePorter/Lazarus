// me2-nx-deko: Mass Effect 2's XDK 2.0.8955, shared with app/me2_src/me2_hooks.cpp (the hooks that hand over to these).
#pragma once
#include <cstdint>
struct PPCContext;
extern "C" void renut_native_sub_82A9EFD0(PPCContext&, uint8_t*);  // D3DDevice_DrawVertices
extern "C" void renut_native_sub_82A9F3C0(PPCContext&, uint8_t*);  // D3DDevice_DrawIndexedVertices
extern "C" void renut_native_sub_82AA5238(PPCContext&, uint8_t*);  // D3DDevice_Swap
