# 🏠 The NSP forwarder

An installable HOME-menu entry that starts `me2_opt.nro`. Worth it for three reasons:

- **The 4th CPU core.** Its NPDM allows cores 0-3; a homebrew launch only gets three. The port moves audio (Wwise, XMA
  decoding, the audio output) to core 3 when the process has it (`switch_audio_core_mask`, `switch_xma_core_mask`).
- **Switch user saves.** The HOME menu asks for a user, and the game saves into that user's save data
  (`app/me2_src/me2_switch_save.cpp`, 64 MB + 32 MB journal) instead of the SD card.
- **Full memory** without holding R.

## Building it

```sh
KEYS=/path/to/your/prod.keys bash tools/nx/build_forwarder.sh [sdmc:/switch/me2-deko/me2_opt.nro]
```

- `KEYS` is **your own console's** `prod.keys`. Only `header_key` is read, to encrypt the NCA headers; the file is
  never copied. **Never commit or share keys.**
- The loader is [Forwarder-Mod](https://github.com/Skywalker25/Forwarder-Mod) (nx-hbloader) at a pinned commit,
  built with your devkitA64; the NSP packing is [prototype-switch](https://github.com/fungalspores/prototype-switch)'s
  `build_forwarder.py` / `nsp_container.py` ([`forwarder/`](../forwarder)).
- `TITLE`, `AUTHOR`, `TITLE_ID` (default `014D453244000000`), `ICON` and `FILE_NAME` change the entry. The title ID
  must match `me2_switch_save_title_id` in `me2.toml`, or the saves go to another title.
- Output: `out/nsp/<FILE_NAME>.nsp`. Install it with any NSP installer; like every homebrew NSP it needs sigpatches.

The NSP only points at the NRO's path: updating the game means replacing `me2_opt.nro`, not reinstalling the NSP.
