# 📈 Profile-guided builds (PGO)

The release NRO is compiled with GCC's profile-guided optimisation: the compiler learns which of the ~28,000
translated game functions and branches are hot from real play sessions. Three steps:

1. **Record.** `PGO=generate bash tools/nx/build_switch.sh sync build` makes an instrumented NRO (no LTO, slower).
   Play it on the console or in Ryujinx; each session writes `pgo/sa2_pgo_<N>.stream` next to the NRO. Cover
   different places: the prologue, the Normandy, a hub (Omega, the Citadel), combat. Six sessions were enough to
   saturate the profile (later ones added 74 functions of 28,217).
2. **Merge.** Copy the `pgo` folder to the PC, then
   `bash tools/nx/pgo_merge.sh <folder>` (profile in `~/lazarus-pgo/profile`, an older one is moved aside).
3. **Build.** `PGO=use bash tools/nx/build_switch.sh build`.

A profile fits only the code it was recorded with: after a codegen change (or another game version) record again.
`tools/pgo_fit.py` and `tools/pgo_summary.py` report how well a profile still matches.
