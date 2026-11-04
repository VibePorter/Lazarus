# 📦 Installing

What goes on the SD card, where it comes from, and how to start the game. Every file that comes from the game has to
be made from **your own copy**; this repository has none of them ([building.md](building.md) makes them).

## What you need

- A Switch that runs homebrew (Atmosphère), with about **14 GB** free on the SD card (game ~9.4 GB, DLC up to ~4 GB,
  shader files ~170 MB).
- Your Xbox 360 copy of *Mass Effect 2*: **both discs**.
- **Title Update 2** for your language (the Xbox LIVE update package `TU_...`; English and Spanish share one, Russian,
  French, German, Italian and Polish have their own). See [title-update-2.md](title-update-2.md).
- Optional: your DLC packages, and ME1 saves for the character import.
- The build output: `me2_opt.nro`, `deko3d_glsl.bin`, `deko3d_shaders.bin`, `deko3d_shaders.bin.idx`.

## SD card layout

The port expects the folder **`sdmc:/switch/me2-deko/`** (the profiles name it; change the paths in `me2.toml` if you
use another one).

```text
sdmc:/switch/me2-deko/
  ├── me2_opt.nro                      the game
  ├── me2.toml                         settings: one of config/profiles, renamed
  ├── deko3d_glsl.bin                  every game shader as GLSL (made from your game)
  ├── deko3d_shaders.bin               precompiled default shader variants (made from your game)
  ├── deko3d_shaders.bin.idx           its index (first start reads it instead of walking 100+ MB)
  ├── game/                            the game (see below)
  │     ├── default.xex                from disc 1
  │     ├── default.xexp               Title Update 2 (disc001 folder of the update package)
  │     ├── BioGame/ media/ ...        disc 1 + disc 2 merged
  │     └── update/
  │           └── BioGame_Update_INT.tlk (+ _ESN.tlk)   Title Update 2's text files
  ├── game_ru/                         (Russian only) the Russian files as an overlay, see below
  ├── me2/                             user data root
  │     └── 0000000000000000/454108CE/00000002/<package>/   DLC, one extracted folder per package
  └── ME1/                             (optional) ME1 saves for the import, see me1-import.md
```

The game writes some files itself on first start: `pipeline_list.bin` (shader pipelines seen so far, compiled early
on the next start), `logs/`, and its saves.

### The game folder

1. Extract both disc images (`python tools/extract_iso.py <disc.iso> -o <out folder>`, or any Xbox 360 ISO
   extractor such as extract-xiso).
2. Merge them into one folder, disc 1 first: `python tools/merge_discs.py game <disc1 folder> <disc2 folder>`
   (hard links on the same drive; files present on both discs are compared and reported if they differ).
3. Extract the Title Update 2 package: `python tools/stfs_extract.py <TU2 package> tu2`. Copy
   `tu2/.../disc001/default.xexp` next to `default.xex`, and the `BioGame_Update_<LANG>.tlk` files into `game/update/`.

The SDK applies the `.xexp` patch at load (the log says `XEX patch applied: 0.0.0.1 -> 0.0.2.1`) and mounts
`update/` as the game's `UPDATE:` drive.

> [!NOTE]
> The NRO must be built against the **same** TU2 `default.xex` + `default.xexp`: the code generator translates the
> patched program, and every hook in the port points at TU2 addresses.

### Russian

The Russian release (Russian text, English voices; media IDs `0A064D08` / `592BA3B4`) runs on the same NRO: its
program is byte for byte the English one, only five data files differ. It is often kept as two **Games on Demand**
packages (`454108CE/00007000/<header>` + `<header>.data/`) rather than ISOs; `tools/god_extract.py` reads those:

```sh
python tools/god_extract.py "<disc 1 header>" extract-all out/ru_disc1
python tools/god_extract.py "<disc 2 header>" extract-all out/ru_disc2
python tools/merge_discs.py out/game_ru_full out/ru_disc1 out/ru_disc2
python tools/stfs_extract.py "<Russian Title Update 2 package>" out/tu2_ru
```

(ISOs work too: `tools/extract_iso.py` as for English.) Then pick one of two layouts:

**A. Russian only.** Use the merged Russian game as `game/`, exactly like the English one: the Russian TU2
`default.xexp` next to `default.xex`, `BioGame_Update_RUS.tlk` in `game/update/`. Take a `ru/` profile and **delete
its `game_overlay_root` line**.

**B. English and Russian side by side.** Keep the English game in `game/` and make the overlay (7 files, ~30 MB):

```sh
python tools/make_overlay.py <English game> out/game_ru_full out/game_ru --tu2 out/tu2_ru/<...>/disc001
```

Copy `out/game_ru` to `sdmc:/switch/me2-deko/game_ru/`. The `ru/` profiles set
`game_overlay_root = "/switch/me2-deko/game_ru"`, which lays it over `game/`; the `en/` profiles leave it out and
play in English. The game takes its language from the overlay's `default.xex`.

### DLC

Extract each DLC package with `python tools/stfs_extract.py <package> <folder>` into
`me2/0000000000000000/454108CE/00000002/<package file name>/`. The game finds them like on the Xbox 360.

## Settings

Copy one of the profiles as `sdmc:/switch/me2-deko/me2.toml`:

| Profile | For |
| :--- | :--- |
| [`en/performance`](../config/profiles/en/performance/me2.toml) | English, stock clocks, the steadiest frame rate |
| [`en/fidelity`](../config/profiles/en/fidelity/me2.toml) | English, higher resolution and detail; meant for overclocked consoles |
| [`ru/performance`](../config/profiles/ru/performance/me2.toml), [`ru/fidelity`](../config/profiles/ru/fidelity/me2.toml) | the same in Russian (game_ru overlay, Russian option rows) |

What the settings do: [configuration.md](configuration.md).

## Starting the game

- **From the Homebrew Menu, with full memory:** hold **R** while starting an installed game (title takeover). The
  applet mode (album) does not have enough memory.
- **From the HOME menu:** build and install the [NSP forwarder](forwarder.md). It also gives the game the 4th CPU
  core and saves into the Switch user you pick.

The first start takes longer: the game indexes the game folder and reads the shader files. Later starts are quick.

## Ryujinx

Lazarus runs in Ryujinx for testing (not as the main way to play). An NRO there has no path of its own, so the game
always uses `sdmc:/switch/me2-deko` inside Ryujinx's SD folder. Add these to the `me2.toml` you use in Ryujinx:

```toml
switch_guest_views_eager = true    # Ryujinx reports no data aborts: map guest memory up front
log_flush_interval = 1             # the log tail is otherwise lost when Ryujinx closes
```

and turn off the XMemCpy natives (`me2_hot_xmemcpy`, `me2_hot_xmemcpy_large`, `me2_hot_xmemcpy_large_wc` = `false`). Never copy a Ryujinx `me2.toml` to the
console: eager views cost memory there.
