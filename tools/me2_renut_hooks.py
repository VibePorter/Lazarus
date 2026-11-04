"""Audit Mass Effect 2's (XDK 2.0.8955) Renut hooks against its generated PowerPC listings and write the runtime guard.

Each hooked or guest-called Direct3D function is identified by instructions that establish its interface or the device
layout it uses (read in ME2's own code: docs/d3d-map.md; base-game addresses). --write creates
app/src/renut_engine/gpu/d3d/me2_hook_map.h: an FNV-1a hash of each function's first bytes in the loaded image, which
Direct3D_CreateDevice checks before it substitutes anything (a different executable is refused).

  python tools/me2_renut_hooks.py --image out/default_image.bin [--write]
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x82000000
MAP = [
    (0x82A8FC18, "Direct3D_CreateDevice", ["li r5,24064", "stw r3,10888(r31)", "stw r3,16728(r31)", "bl 0x82aa8a20"]),
    (0x82AA0BA8, "KickOff", ["lwz r11,13232(r31)", "bl 0x82aa0668", "lwz r3,48(r31)"]),
    (0x82AA0950, "KickOffSegment", ["lwz r30,14928(r3)", "bl 0x82aa0160"]),
    (0x82AA0160, "NewSegment", ["lwz r30,13516(r31)", "lwz r11,23980(r31)", "lwz r4,14940(r31)"]),
    (0x82A9F9E8, "BeginRingBig", ["rlwinm r30,r4,2,0,29", "bl 0x82aa0ba8", "bl 0x82aa0160"]),
    (0x82A9FD90, "RingBufferDeviceAllocate", ["lwz r11,0(r5)", "stw r11,0(r29)"]),
    (0x82A9FFC0, "AllocateInlineData", [r"lwz r\d+,13516\(r\d+\)", "bl 0x82a9fd90"]),
    (0x82AA1168, "SetRingBufferParameters", ["lwz r11,14904(r31)", "bl 0x82aa1058"]),
    (0x82AA5238, "D3DDevice_Swap", [r"stw r\d+,16560\(r\d+\)", r"stw r\d+,10928\(r\d+\)", "bl 0x82aa4d00"]),
    (0x82AA0CA8, "BlockOnFence", ["lwz r11,10896(r3)", "lwz r10,10908(r3)", "lwz r11,13232(r3)", "bl 0x82a8f9c8"]),
    (0x82A8F9C8, "CBlocker_Check", ["lwz r11,10896(r29)", "cmplwi cr6,r11,5000"]),
    (0x82AA10D8, "IsFencePending", ["lwz r11,10908(r31)", "bl 0x82aa0ba8", "lwz r11,2048(r11)"]),
    (0x82AA4D00, "SynchronizeToPresentationInterval", ["ori r11,r11,16", "stb r11,10941(r31)", "lwz r11,13596(r31)"]),
    (0x82AA0DA0, "RingCallback", [r",21504$"]),
    (0x82A9EFD0, "D3DDevice_DrawVertices", ["addi r6,r3,1920", "bl 0x82aa7408"]),
    (0x82A9F3C0, "D3DDevice_DrawIndexedVertices", [r"lwz r\d+,12444\(r31\)", "bl 0x82aa7408"]),
    (0x82A9E5D8, "D3DDevice_BeginVertices", ["mullw r9,r5,r6", "stb r10,12536(r3)"]),
    (0x82A9EAB8, "D3DDevice_BeginIndexedVertices", ["mullw r11,r6,r9", "stb r10,12536(r31)", "lwz r9,308(r1)",
                                                   "stw r11,13444(r31)"]),
    (0x82A9B2A8, "WindowScissor", ["mr r25,r4", "mr r22,r7", "lwz r11,12448(r31)"]),
    (0x82AA2B30, "D3DDevice_ClearF", ["lwz r8,12448(r3)", "lwz r11,12464(r3)", "bl 0x82aa2490"]),
    (0x82AA31F8, "D3DDevice_Resolve", [r",8984$", "stw r6,412(r1)"]),
    (0x82AA4398, "D3DDevice_BeginTiling", ["stw r5,12756(r3)", "stw r6,13188(r31)", "bl 0x82aa2b30"]),
    (0x82AA46F0, "EndTiling", ["lwz r11,12756(r31)", "bl 0x82aa0da0"]),
    (0x82A93298, "XGRegisterVertexShader", ["li r5,872", "stw r30,32(r31)", "lwz r11,872(r31)"]),
    (0x82A92FB0, "XGRegisterPixelShader", ["li r10,7", "stw r4,24(r3)"]),
    (0x82A93650, "VertexDeclaration_Init", ["cmplwi cr6,r11,255", "mulli r11,r30,12"]),
    (0x82AA7D78, "D3DVertexShader_Bind", ["lwz r10,28(r4)", "bl 0x82aa6df8", "bl 0x82aa6508"]),
    (0x82A8E7A0, "D3DResource_Destroy", ["clrlwi r10,r11,28", "bl 0x82aa0ca8", "bl 0x82c4b2d8"]),
    # Called from Me2CreateDevice.
    (0x82A8FAA0, "HardwarePM4Snippets", ["stw r3,23988(r31)", "stw r3,23996(r31)"]),
    (0x82AAAD90, "DeferredWorkQueue", ["stw r3,11808(r30)", "bl 0x82c49438"]),
    (0x82A92AC0, "ShaderGprBuffers", ["stw r31,13780(r30)", "bl 0x82a927d0"]),
    (0x82AAB0E0, "XenonGpu_Initialize", ["std r9,11832(r3)", "stw r8,10428(r31)"]),
    (0x82AA85C8, "InitializePresentationParameters", [r"stw r\d+,14824\(r\d+\)", r"stw r\d+,14832\(r\d+\)"]),
    (0x82AA82E8, "InitializeApiState", ["stw r29,11824(r31)", "stw r9,12716(r31)", "stw r24,13596(r31)"]),
    (0x82A9B0F0, "DefaultViewport", ["bl 0x82a9b2a8", "bl 0x82aa0ba8"]),
]


def listings(folder: Path) -> dict[int, list[str]]:
    found = {}
    for path in folder.glob("*_recomp.*.cpp"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\) \{(.*?)(?=^DEFINE_REX_FUNC|\Z)", text, re.M | re.S):
            va = int(match[1], 16)
            if 0x82A8D9A8 <= va < 0x82AB3138:
                found[va] = re.findall(r"^\s*// ([a-z][a-z0-9.]* ?.*?)\s*$", match[2], re.M)
    return found


def fnv32(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gen", type=Path, default=ROOT / "app/generated/default")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    bodies = listings(args.gen)
    image = args.image.read_bytes()
    rows = []
    failed = False
    for va, role, witnesses in MAP:
        body = bodies.get(va)
        if body is None:
            print(f"FAIL {role} {va:08X}: no such function in the listings")
            failed = True
            continue
        def found(w: str) -> bool:
            if "\\" in w or w.endswith("$"):  # a regular expression
                return any(re.search(w, i) for i in body)
            return w in body  # an exact instruction
        missing = [w for w in witnesses if not found(w)]
        if missing:
            print(f"FAIL {role} {va:08X}: instruction witnesses missing: {missing}")
            failed = True
            continue
        size = min(64, len(body) * 4)
        code = image[va - BASE:va - BASE + size]
        rows.append((va, size, fnv32(code), role))
    if failed:
        raise SystemExit(1)
    if args.write:
        header = ROOT / "app/src/renut_engine/gpu/d3d/me2_hook_map.h"
        lines = ["// Generated by tools/me2_renut_hooks.py from Mass Effect 2's loaded image (base default.xex, no title update).",
                 "#pragma once", "#include <cstdint>", "namespace renut::gpu::me2 {",
                 "struct GuestCodeGuard { uint32_t address, bytes, fnv32; const char* name; };",
                 "inline constexpr GuestCodeGuard kGuestCodeGuards[] = {"]
        lines += [f'  {{0x{va:08X}u, {size}u, 0x{h:08X}u, "{role}"}},' for va, size, h, role in rows]
        lines += ["};", "}  // namespace renut::gpu::me2", ""]
        header.write_text("\n".join(lines), newline="\n")
    print(f"PASS: {len(rows)} ME2 interfaces have their instruction witnesses")


if __name__ == "__main__":
    main()
