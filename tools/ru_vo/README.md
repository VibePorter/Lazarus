# Russian voice-over (SynthVoiceRu, 2026-11-19)

SynthVoiceRu is a Russian voice-over for Mass Effect 2 Legendary Edition. These scripts move its lines onto the Xbox
360 game data. Every 360 WwiseStream that has a Russian LE counterpart now points at a converted copy of the Russian
stream, appended to the same afc file. The English data stays where it was, and an InterpData's InterpLength takes
the longer of the two lengths. The runtime lays the result over the game data and the DLC (`me2_russian_voiceover`,
sdk/src/system/runtime.cpp and xam/content_manager.cpp).

Nothing made here belongs in the tree: you build it from your own LE2 install, your own 360 game data (TU2) and DLC,
and the SynthVoiceRu installer.

## What you need

- Python 3 (all scripts run with `python -I`; `tools/me2_package.py` is found through the parent folder)
- ffmpeg with libvorbis (`FFMPEG`, default `ffmpeg` on the PATH)
- `packed_codebooks_aoTuV_603.bin` from ww2ogg (`WW_CODEBOOKS`, default: next to wwvorbis.py)
- 7-Zip, to unpack `SynthVoiceRu_MELE2_VO.exe` (a 7z archive)

## Settings (environment)

| variable        | what                                                                        |
|-----------------|-----------------------------------------------------------------------------|
| `RU_VO_WORK`    | work folder (default `ru_vo_work` in the current folder; ignored by git)    |
| `ME2_LE_DIR`    | the original LE2 install's `Game/ME2` (Oodle is loaded from its `Binaries/Win64`, or `OODLE_DLL`) |
| `ME2_X360_GAME` | the 360 game data (TU2), with `BioGame/` inside                             |
| `ME2_X360_DLC`  | the 360 DLC store, with `<xuid>/<title>/00000002/<package>/` inside         |
| `FFMPEG`, `WW_CODEBOOKS`, `OODLE_DLL` | see above                                             |

## Steps

1. Unpack the installer into the work folder: `7z x SynthVoiceRu_MELE2_VO.exe -o%RU_VO_WORK%`. This gives
   `manifest.jsonl` and `patches/Game/ME2/...` (xdelta patches against the LE2 files).
2. Patch a copy of the LE2 files (the hashes are checked before and after):
   `python -I apply_all.py %RU_VO_WORK%\manifest.jsonl %RU_VO_WORK% %ME2_LE_DIR% %RU_VO_WORK%\le_ru`
3. Map the changed lines:
   - `python -I build_map.py le` writes `map/le_streams.json` and `map/le_interp.json`
   - `python -I build_map.py x360 %ME2_X360_GAME% %ME2_X360_DLC%` writes `map/x360_scan.json` and the coverage report
     (`build_map.py report` prints it again)
4. Build:
   - `python -I build_ru_vo.py plan` writes `map/plan.json`
   - `python -I build_ru_vo.py convert [N]` writes `work/conv/<sha1>.wem` (360 Wwise Vorbis, 24 kHz mono, q4).
     It is cached, and N converts only the first N lines, for a test.
   - `python -I build_ru_vo.py assemble` writes `out/game/...` and `out/dlc/<xuid>/<title>/00000002/<package>/...`
5. Check: `python -I verify_out.py`. Every WwiseStream must point at a valid RIFX.

Tests and tools for looking around: `test_roundtrip360.py` (360 Vorbis round trip), `diff_le.py` (what the mod changed,
class by class), `afc_scan.py`, `le_package.py` (LE package lister), `vcdiff.py` (one xdelta patch by hand).

## On the card

Copy the output beside the game data folder as `ru_vo/` (or set `me2_russian_voiceover_root`):

    ru_vo/game/...                 out/game/...
    ru_vo/dlc/<package>/...        out/dlc/<xuid>/<title>/00000002/<package>/...   (the package folder only)

Then set `me2_russian_voiceover = true`. Both halves belong together, because DLC lines also live in the game's afc
files. If `ru_vo/game` is missing, the runtime turns the flag off and plays the original voices.

Packed (fewer files for MTP and FAT32), both are optional and a pack wins over loose files:

    python tools/build_asset_pack.py ru_vo/game --inside --split-mb 3072      -> ru_vo/game/game.nxpack + .nxidx
    python tools/build_asset_pack.py ru_vo/dlc/<package> --inside             -> one pack per package
