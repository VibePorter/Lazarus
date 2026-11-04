"""dcbz clears the Xbox 360's whole 128-byte cache line; ReXGlue's build_dcbz clears 32 bytes (dcbzl / dcbz128 is
already 128). ME2 (2026-10-27, after silent-hill-downpour-switch 0.3.0 shd/tools/patch_generated.py and xenia-canary
72ce130): two of its three plain dcbz sites are clearing loops with a 128-byte step (sub_82C6DD78 and sub_82FCFBF8,
XDK library code: 256 or 512 bytes of a structure's buffer), so 3/4 of each line kept stale bytes; the third
(sub_82EB8468) is a copy loop that overwrites the whole line right after, unaffected either way.

Idempotent; tools/post_codegen.py runs it in both flavours. Only the generated files that change are written.

    python tools/dcbz_128.py --gen app/generated/default
"""
import glob
import os
import re
import sys

DCBZ = re.compile(r"(// dcbz [^\n]*\n\s*ea = [^\n;]*& )~31;(\n\s*memset\(\(void\*\)REX_RAW_ADDR\(ea\), 0, )32\);")


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else 'app/generated/default'
    total = 0
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        with open(path, encoding='utf-8', newline='') as f:
            text = f.read()
        new, n = DCBZ.subn(r'\1~127;\g<2>128);', text)
        if n:
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.write(new)
            total += n
    print(f'dcbz_128: {total} dcbz site(s) now clear 128 bytes')


if __name__ == '__main__':
    main()
