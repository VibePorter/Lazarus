# 🐞 Debugging

## Logs

| File | |
| :--- | :--- |
| `me2.log` (next to the NRO) | the runtime log: `[me1-import]`, `[save]`, `[io]`, `[renut]`, `[occlusion]`, `[cull]`, ... |
| `logs/rex/rex_profile.log` | the periodic performance report ([measuring.md](measuring.md)) |
| `rex_crash.log` | written when the process catches a crash |

The log is flushed every `log_flush_interval` seconds: the last seconds before a crash or a HOME-button exit can be
missing. For a crash hunt set `log_flush_interval = 1`.

In **Ryujinx** the NRO has no path, so logs land in `sdmc:/switch/me2-deko/` of Ryujinx's SD folder, and Ryujinx's own
log (`Logs/Ryujinx_*.log`) has the guest stack of a crash: "`Guest stack trace`" with `sa2:0x...` offsets and
"`Invalid memory access at virtual address ...`".

## Turning a crash stack into function names

Every build keeps its unstripped ELF in `out/deployments/<sha256 of the NRO>/me2_opt.elf`. Find the right one with
`sha256sum me2_opt.nro`, then:

```sh
aarch64-none-elf-addr2line -e out/deployments/<sha>/me2_opt.elf -f -C -i -p 0x2496650 0x2709158 ...
```

The `sa2:0x...` offsets of the Ryujinx stack (or the console's crash report) are the addresses to pass. Names come out
as `__imp__sub_82XXXXXX`: the guest function at that Xbox 360 address, whose C++ is in `app/generated/default`
(search for `DEFINE_REX_FUNC(sub_82XXXXXX)`; every instruction is there as a comment). LTO folds functions, so a
frame now and then names a neighbour: trust the chain, check the odd ones.

## Inside the game's code

- `out/default_image.bin` (written by codegen) is the patched guest image; `tools/me2_package.py decompress` unpacks a
  game package, and `tools/me2_ue3_disasm.py <package> Class.Function` lists UnrealScript bytecode, which is where
  most menu and story logic lives.
- `tools/deko/d3d_index.py`, `d3d_find.py`, `d3d_offsets.py`: an index of the Direct3D functions and the device
  fields they touch.
- The PC build runs the same code under a debugger and RenderDoc (`tools/win/rdc_*.py` read captures).

## Before reporting an issue

Attach `me2.log`, the NRO's sha256 and, for a crash, the Ryujinx log or the console's crash report. Say which profile
(`me2.toml`) you used and which save / place it happened in.
