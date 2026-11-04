"""Reproduce the occlusion startup crash's TLS alias on saved final ELFs.

Models the actual KThread.GetThreadName offsets against libnx's TLS image,
then starts an occlusion query by setting its real t_carry_begin symbol to 1.
No game or emulator is launched.
"""
from pathlib import Path
import argparse
import struct
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
from check_switch_tls import layout, name_fields, check

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--before', type=Path, required=True)
ap.add_argument('--after', type=Path, required=True)
args = ap.parse_args()


def query_name(path):
    sections, symbols, tcb, image = layout(path)
    begin = next(s for s in symbols if s['kind'] == 6 and '13t_carry_beginE' in s['name'])
    assert begin['size'] == 8
    before = name_fields(image, tcb)
    struct.pack_into('<Q', image, begin['value'], 1)
    return before, name_fields(image, tcb), begin['value']


before, active, offset = query_name(args.before)
assert before == dict(version=0, name_pointer=0), before
assert active == dict(version=0, name_pointer=1), active
print(f'Captured failing ELF: t_carry_begin at TLS+{offset:#x}; active query becomes name pointer 1')
before, active, offset = query_name(args.after)
assert before == active == dict(version=0, name_pointer=0), (before, active)
print(f'Fixed ELF: t_carry_begin at TLS+{offset:#x}; name pointer stays null')
print(check(args.after))
print('PASS: query remains active without aliasing Ryujinx thread-name fields')
