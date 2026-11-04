# Title Update 2 address port (2026-11-03)

Base default.xex (0.0.0.1) -> TU2 (0.0.2.1). TU2 relinks the whole image, so every guest address in the sources was
translated by these scripts (run from this folder, Python 3; tu2diff.py needs capstone).

1. tu2map.py <dir-with-img.py> <base image> <tu2 image> map.json   items = .pdata functions + gaps, normalized-hash alignment, data anchors
2. tu2refine.py <dir> <base> <tu2> map.json map2.json               call-site + data-pointer votes (fixes wrong twins)
3. tu2verify.py / tu2same.py                                        call-target consistency; marks code-identical items ('same')
4. TU2MAP=map2.json tu2_rewrite.py / tu2_rewrite_0x.py map2.json <root> <log.json> [--apply]
5. tu2local.py: 8-instruction local signature near the nearest mapped neighbour (local.json); tu2find.py / tu2diff.py for manual cases.

Images: base = me2-nx-deko/out/default_image.bin, TU2 = this project's out/default_image.bin (tools/codegen.ps1 writes it).
Logs of what was rewritten: rewrite_applied.json, rewrite_0x_applied.json. Manual mappings: MANUAL in tu2xlate.py.
