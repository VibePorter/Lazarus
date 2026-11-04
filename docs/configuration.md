# ⚙️ Configuration

All settings live in **`me2.toml`** next to the NRO (`key = value`, TOML). Start from one of the profiles in
[`config/profiles`](../config/profiles) and change single lines; every key the build knows is also accepted on the PC
build's command line as `--key=value`.

## The two profiles

| | performance | fidelity |
| :--- | :--- | :--- |
| Meant for | stock clocks (handheld CPU 1020 / GPU 460.8 MHz, docked GPU 768) | overclocked consoles (tested at CPU 1785 / GPU 998) |
| Scene resolution | 854x480 handheld, 960x540 docked | 960x540 handheld, 1280x720 docked |
| Final upscale | bilinear (`me2_renut_ui_upscale = 1`) | FXAA + FSR 1 EASU (`= 0`) |
| Shadows, detail | `DetailMode=1`, no dynamic decals / distortion, 2x anisotropy, LOD bias 1 | `DetailMode=2`, decals, distortion, light-environment shadows, 8x anisotropy |
| Size culling | `me2_cull_small_factor = 60`, lights `15` | `80`, lights `20` |

The `ru/` profiles are the same plus `game_overlay_root` (the Russian files) and Russian text for the option rows.

## Settings worth knowing

### Paths

| Key | |
| :--- | :--- |
| `game_data_root` | the game folder (`/switch/me2-deko/game`) |
| `update_game_dir` | the folder inside it that becomes `UPDATE:` (TU2's `.tlk` files) |
| `game_overlay_root` | a folder laid over the game (the Russian files) |
| `user_data_root` | DLC and user data (`/switch/me2-deko/me2`) |
| `log_file` | the log (`/switch/me2-deko/me2.log`) |

### Picture

| Key | |
| :--- | :--- |
| `sa2_render_height_handheld` / `_docked` | 3D scene height: **360, 480, 540, 720** (others fall back to 720) |
| `me2_renut_ui_height_handheld` / `_docked` | the menus and HUD (720 / 1080) |
| `me2_renut_ui_upscale` | 0 = FXAA + FSR 1, 1 = bilinear (cheaper) |
| `me2_ini_override` | the game's own ini settings, `Section:Key=Value` separated by `\|` (shadows, LOD, VFX budget, ...) |
| `me2_renut_no_game_msaa` | keep `true`: the game's 2x MSAA path is not right on deko3d yet |

### Performance features

| Key | |
| :--- | :--- |
| `me2_occlusion_queries`, `me2_occlusion_umbra_latent` | real answers for the Umbra occlusion queries (the biggest draw-count saving) |
| `me2_cull_small_factor`, `me2_cull_dynamic_factor` | skip static / moving objects smaller than this on screen (higher = more culling) |
| `me2_light_small_factor`, `me2_light_whole_factor` | leave small objects out of dynamic lights / skip whole small lights |
| `me2_hot_*` | the native replacements of hot game functions (each self-checked at start) |
| `sa2_io_readahead` | reads the next blocks of a file while the game works on the current ones |
| `me2_gc_*` | when the engine's garbage collection may run (away from streaming and combat) |

### Input

| Key | |
| :--- | :--- |
| `input_xbox_layout` | X / Y by position (the game's left button is the Switch's Y) |
| `input_gyro` | 0 off, 1 while ZL is held, 2 always; also in *Options → Controller* |
| `input_gyro_invert_x` / `_y` | flip an axis |
| `input_rumble` | rumble |

### Game features

| Key | |
| :--- | :--- |
| `me2_menu_hide_import_me1`, `me2_me1_import_dir` | the ME1 import (see [me1-import.md](me1-import.md)) |
| `me2_skip_relaunch` | the Xbox's "restart the game" path is skipped (default `true`; the port cannot restart itself) |
| `me2_subtitle_size` | subtitle size in percent; also *Options → Gameplay* |

### Logs and diagnostics

`log_level`, `switch_profile_report` (the `logs/rex/rex_profile.log` report, see [measuring.md](measuring.md)) and the
many `*_diag_*` / `*_trace` / `*_verify` keys, which are all off in the profiles. Turn one on for a single run when a
guide or an issue asks for it: most cost frame rate.

## Gyro and subtitle rows

*Options → Controller* has four rows for gyro aiming (mode, sensitivity, invert X / Y) and *Options → Gameplay* one for
the subtitle size. They are saved in `me2/me2_gyro.cfg` and override the toml once set in the game.
