# Contributing to Lazarus

Thanks for helping. A few rules keep the project healthy and legal.

## Never commit game data

Nothing made from the game may enter the repository: game files, `app/generated/`, `out/default_image.bin`, shader
containers, SPIR-V / GLSL / DKSH made from the game's shaders, NROs, ELFs, saves, logs with save contents, and keys.
`.gitignore` covers the usual names; check `git status` before every commit and never `git add -f` such a file.

## Bug reports

Use the issue template. Attach `me2.log`, the sha256 of your `me2_opt.nro`, the profile you used, and for a crash the
Ryujinx log or the console's crash report ([docs/debugging.md](docs/debugging.md)).

## Changes

- One topic per pull request, with what it fixes and how you checked it (PC build, Ryujinx, console; clocks and place
  for performance claims).
- Hooks use the TU2 addresses. A new hook of a function the optimised codegen calls directly needs
  `python tools/direct_calls.py --redo` (or a new codegen) so the calls go through the hook.
- A new setting: a `REXCVAR_DEFINE_*` with a clear description, off by default unless it is safe everywhere, and a
  line in [docs/configuration.md](docs/configuration.md) if users should know it.
- Keep code comments short and factual; the history belongs in the commit message.
- Code in `sdk/` is BSD-3-Clause, `shaders/` MIT, the rest GPL-3.0 (see `THIRD_PARTY_NOTICES.md`).
