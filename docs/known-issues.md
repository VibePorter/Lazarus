# 🚧 Known issues

Lazarus is a work in progress. What is known not to work, or not yet verified:

## Performance

- The heaviest hubs and parties (Omega's Afterlife, Kasumi's loyalty party, busy Citadel wards) can drop into the high
  teens or low 20s fps: UE3's render thread is busy 90-100 % at 2,500-3,500 draws a frame there. Parallelising it was
  investigated and rejected (non-atomic reference counts in inlined code).
- Audio dropouts can happen in long heavy stretches when Wwise and XMA decoding share cores with the game.

## Rendering

- The game's 2x MSAA path makes models see-through on deko3d: the profiles keep `me2_renut_no_game_msaa = true`.
- Lens flares are off (`me2_renut_lens_flares = false`).
- Distant lights may pop in with size culling (`me2_light_whole_factor`); lower the value if it bothers you.
- Only scene heights 360 / 480 / 540 / 720 exist on deko3d; other values fall back to 720.

## Game

- ME1 import: tested with two PC saves; Xbox 360 ME1 saves and unusual PC saves are less tested
  ([me1-import.md](me1-import.md)). The 2026-11-04 fix for "open the import list, back out, start a new game"
  awaits a console test.
- The game's "restart" path (sign-out, some import failures) is skipped (`me2_skip_relaunch`): you stay where you
  are instead of returning to the title screen.
- Online (Cerberus Network, store) does not exist; the main menu shows an offline Cerberus greeting.

## Build

- The build chain is documented from the maintainer's runs; a full run from a fresh clone has not been repeated yet
  ([building.md](building.md)).
- `tools/` still holds analysis scripts from the projects this port came from (SA2, MGR). They are kept because later
  steps reuse them; not all of them apply to Mass Effect 2.
