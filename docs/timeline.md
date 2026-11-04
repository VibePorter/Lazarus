# 📜 How it came back to life

The Lazarus Project took two years and a cell-by-cell rebuild. This one took about a fortnight, a lot of console
logs, and one dead end. Dates are 2026.

## Before the Switch: the PC recompilation

Mass Effect 2's Xbox 360 program was first recompiled for **Windows** with the upstream ReXGlue SDK (Xenia's GPU
emulation drawing it). By 24 October it booted to *Press START* at 1080p, saved games, mounted all 23 DLC packages,
and black triangles on characters were gone after porting xenia-canary's MSAA sample-position work. That PC build is
still the debugging companion of the Switch port.

## 24 October: first light, and the first wall

- The Switch effort starts from **masseffect-nx** (the ME1 port) in WSL: its SDK fork with the Horizon layer and a
  Vulkan (NVK) renderer. The first NRO is built the same day.
- **Eden** hangs right after the intro movie, waiting for a flip that never comes.
- On a **real Switch** the main menu runs stable for 7 minutes with the SDK's Xenos GPU emulation; in the evening a
  native renderer borrowed from Silent Hill: Downpour's port gets **in game at 13-15 fps**, held back by the CPU cost
  of drawing through Vulkan.
- 23:45: **mgr-nx-deko** is found: Metal Gear Rising on the Switch through a Direct3D-level renderer on deko3d,
  shaders converted offline, no Vulkan driver at all.

## 25 October: the switch to deko3d

- The decision: take mgr-nx-deko's route. Its tree becomes the base of the port; ME2's Direct3D is mapped field by
  field against the layout Renut was written for.
- Afternoon: the **white frame is fixed**: first real picture from Renut on the PC (title screen, logo, the 3D scene).
- Evening: the first deko3d NRO runs in **Ryujinx**, title screen correct, 30 fps.
- Late that night: it **runs on a real Switch**. Gameplay 12-15 fps: alive, but barely.

## 26-27 October: getting up off the table

- Optimised code generation (registers as locals, direct calls), the guest depth prepass skipped, ideas from
  **Cinnamon** (PhysX and light-environment trims, the self-checked native framework; the first native is the LZO
  decompressor): busy scenes **~14 → 21-23 fps**.
- The **NSP forwarder** unlocks the 4th CPU core; audio moves there: heavy scenes **13-16 → 26-30 fps** and the audio
  starvation disappears.
- Picture fixes: half-black faces (the XDK's MSAA viewport offset), black patches on characters, washed-out colours
  (texture gamma), stale constants around the eyes. The UI goes to 720p over a smaller scene, with **FXAA + FSR 1**.
- **PGO** wired in and six profiling sessions recorded; the **Switch button glyphs** replace the Xbox ones; the main
  menu's Cerberus terminal greets you with *"Welcome to Lazarus"*.
- Real **occlusion queries** for Umbra, ME2's visibility middleware.

## 28-31 October: Umbra, logs and the Russian edition

- Umbra gets answers it can use without waiting (latent results, a CPU reprojection test, guards against walls
  vanishing): the Omega hub's heaviest spot goes **16 → 27 fps**.
- Store cards from **mk9-nx**, the input and death-screen fixes, a native LZX decompressor.
- Many console log runs read line by line: audio, XMA, garbage collection, streaming hitches.
- The galaxy map's mass relays (and other particles) stop vanishing: the Xbox computes 0 × ∞ = 0, the Switch NaN, and
  the shader translator now follows the Xbox rule.
- The **Russian edition** is dumped and runs as an overlay on the English game.

## 1-2 November: the render thread, and ME1 comes home

- The hunt for the last frames starts on UE3's render thread: wait-site profilers, stack sampling, material natives.
- Ports from **naruto-rise-of-a-ninja-nx**, Umbra size culling, whole-light culling, the 540p mode.
- The **ME1 import** is taken apart: the Xbox starts a second program (`Unreap.xex`) and restarts the game. The port
  follows the PC version's route instead, converting PC ME1 saves to the Xbox format. That night a converted Shepard
  makes it into a new game for the first time; the remaining format differences are fixed in the days after.

## 3 November: Title Update 2

- Console logs of the release: **median 29.4 fps** in gameplay at 540p (overclocked), 48 % of the time at 30.
- An experiment to split UE3's render thread in two is measured and dropped (its reference counts are not atomic).
- **Title Update 2** turns out to be a full relink: every one of ~2,000 addresses in the port is moved with a
  function-by-function map of the two images, and the port switches to TU2.
- Gyro aiming with its own rows in *Options → Controller*; the start-up music no longer plays over the logos; the
  Kasumi-outfit texture mix-up is fixed.

## 4 November: Lazarus

- A subtitle size option, the gyro slider and start-up black-screen fixes, and the ME1 import crash fixed.
- The port gets its name and its own repository.

---

*Shepard died in the first minutes of Mass Effect 2 and came back. So did the game, on a console it was never made
for.*
