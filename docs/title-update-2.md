# 🔄 Title Update 2

Lazarus is built for **Mass Effect 2 with Title Update 2** (`default.xex` 0.0.0.1 + `default.xexp` → 0.0.2.1;
Russian 0.0.0.6 → 0.0.2.6). The update brings shared-DLC support (Kasumi / Zaeed achievements, Overlord, the Arc
Projector, the Blackstorm), DLC planet names, script fixes (loadouts, ammo powers, planet scanning, QuickLoad,
*Character Record* cancel) and its own text files (`BioGame_Update_<LANG>.tlk`).

## What the port does with it

- **Codegen:** the SDK applies the `.xexp` next to `default.xex` before translating, so the C++ in `app/generated` is
  the patched program. The runtime applies it again at load (`XEX patch applied` in the log).
- **Update files:** `update_game_dir = "update"` makes `<game>/update/` the game's `UPDATE:` drive.
- **One binary for English and Russian:** the Russian TU2 image is byte-for-byte the English one; only its `.xexp`
  header and text files differ, and those come from `game_ru/`.

## The address port

TU2 is a full **relink**, not a small patch: `.text` moved by +0x10138, `.data` by +0x10000, and functions were
reordered. Every guest address in the port's sources (hooks, natives, data) was moved with the scripts in
[`tools/tu2`](../tools/tu2) (see its README):

1. `tu2map.py`: matches the two images function by function (normalised instruction hashes, sequence alignment,
   data anchors): ~65,000 of 68,500 code items;
2. `tu2refine.py`, `tu2verify.py`, `tu2same.py`: call-site and data-pointer votes, consistency checks;
3. `tu2_rewrite.py`, `tu2_rewrite_0x.py`: rewrite the addresses in the sources (logs: `rewrite_applied.json`);
4. `tu2local.py`, `tu2find.py`, `tu2diff.py`: the few functions TU2 changed, by hand (`MANUAL` in `tu2xlate.py`).

The hook "witnesses" (`tools/me2_renut_hooks.py --write`) and `app/overrides.toml` were regenerated for TU2, and a
PGO profile has to be recorded again (function names changed).

`docs/d3d-map.md` keeps the **base-game** addresses it was written with; the Direct3D functions are the same code at
TU2's addresses (`app/src/renut_engine/gpu/d3d/me2_hook_map.h`).
