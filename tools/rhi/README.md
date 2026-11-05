RHI-boundary census tools (2026-11-04). Run from the project root after `python tools/callgraph.py build`.
  python tools/rhi/rhi_callers.py out/callgraph.json 82A8E000 82AAC000 <out>/rhi_callers.json   # UE3 functions calling XDK D3D
  python tools/rhi/devrefs.py <outdir>          # functions loading GDevice [0x833D074C] -> <outdir>/devrefs.json
  python tools/rhi/devwrites.py <outdir>        # their inline stores through the device pointer
  <venv python with capstone> tools/rhi/ppcdis.py sub_XXXXXXXX ...   # disassemble from out/default_image.bin (MAXI=n)
