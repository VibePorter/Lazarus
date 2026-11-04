# 📊 Measuring

With `switch_profile_report = true` (on in the profiles) the game writes a report every `switch_profile_report_s`
seconds to `logs/rex/rex_profile.log`, and a short line every 10 seconds to `me2.log`:

```text
[renut] 10.0 s: 294 game frames (29.30 fps), render thread busy 4.07 ms a frame, GPU ... ms a frame; 103227 draws, ...
```

- **game frames / fps:** frames the game presented in the window;
- **render thread busy:** the port's renderer thread (Renut) per frame;
- **GPU:** the GPU's own time per frame, when `renut_gpu_profile` is on (costs a little);
- **draws:** how much the game asked for. ~1,500-2,500 draws a frame is normal gameplay; hubs reach 3,500.

Rules of thumb from the console runs:

- **fps low, render thread low, GPU low:** the game's CPU threads are the limit (UE3's render thread, the game thread,
  Wwise). Nothing in the graphics settings helps much; draw-count features (occlusion, size culling) do.
- **GPU near 33 ms:** lower `sa2_render_height_*` or use the performance profile's upscale.
- Compare runs at the **same place and the same clocks**: one standing spot for 30 s is more useful than a long
  mixed session.

`switch_profile_stacks = true` adds sampled guest stacks of the hottest threads to the report (with a small cost):
the way the hot functions for natives and PGO were found.
