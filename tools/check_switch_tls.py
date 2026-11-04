"""Verify libnx native TLS cannot be mistaken for a thread-name pointer by Ryujinx.

Reads the final AArch64 ELF, including its TLS symbols and initial data image.
Does not launch a process or emulator. No external Python packages are needed.
"""
from pathlib import Path
import argparse
import json
import struct

PREFIX_BYTES = 0x200


def read_elf(path):
    data = Path(path).read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01', 'expected little-endian ELF64'
    assert struct.unpack_from('<H', data, 18)[0] == 183, 'expected AArch64'
    shoff = struct.unpack_from('<Q', data, 40)[0]
    shsize, count, names_index = struct.unpack_from('<HHH', data, 58)
    assert shsize == 64 and 0 < names_index < count
    raw = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize) for i in range(count)]
    names = data[raw[names_index][4]:raw[names_index][4] + raw[names_index][5]]

    def string(table, offset):
        return table[offset:table.index(0, offset)].decode('utf-8', errors='replace')

    sections = {}
    for i, s in enumerate(raw):
        sections[string(names, s[0])] = dict(index=i, kind=s[1], flags=s[2], addr=s[3],
            offset=s[4], size=s[5], alignment=s[8], data=data[s[4]:s[4]+s[5]] if s[1] != 8 else b'')
    symbols = []
    for s in raw:
        if s[1] != 2:
            continue
        strings = raw[s[6]]
        table = data[strings[4]:strings[4]+strings[5]]
        for at in range(s[4], s[4]+s[5], s[9]):
            name, info, other, section, value, size = struct.unpack_from('<IBBHQQ', data, at)
            symbols.append(dict(name=string(table, name), kind=info & 15,
                                section=section, value=value, size=size))
    return sections, symbols


def layout(path):
    sections, symbols = read_elf(path)
    td, tb = sections['.tdata'], sections['.tbss']
    alignment = struct.unpack('<Q', sections['.tls.align']['data'])[0]
    tcb = max(16, alignment)
    image = bytearray(sections['.main.tls']['size'])
    image[:td['size']] = td['data']
    return sections, symbols, tcb, image


def name_fields(image, tcb):
    # KThread.GetThreadName: TLS+0x1F8 is libnx's tls_tp, not ThreadType.
    version_at = 0x46 - tcb
    assert version_at >= 0
    version = struct.unpack_from('<H', image, version_at)[0]
    offset = {0: 0x1A8, 0xFFFF: 0x1A8, 1: 0x1A0}.get(version)
    pointer = struct.unpack_from('<Q', image, offset-tcb)[0] if offset else None
    return dict(version=version, name_pointer=pointer)


def check(path):
    sections, symbols, tcb, image = layout(path)
    td, tb = sections['.tdata'], sections['.tbss']
    assert 16 <= tcb <= 64, 'thread-name fields must follow the TCB'
    assert td['flags'] & 0x400 and tb['flags'] & 0x400, 'sections must carry SHF_TLS'
    assert len(td['data']) >= PREFIX_BYTES and not any(td['data'][:PREFIX_BYTES]), 'missing zero TLS prefix'
    objects = [s for s in symbols if s['kind'] == 6 and s['size']]
    assert objects and all(s['value'] >= PREFIX_BYTES for s in objects), 'native TLS object overlaps prefix'
    aligned_td = (td['size'] + tb['alignment'] - 1) & -tb['alignment']
    assert len(image) == aligned_td + tb['size'], 'main-thread TLS allocation has wrong size'
    values = {s['name']:s['value'] for s in symbols}
    assert values['__tls_end'] - values['__tls_start'] == len(image)
    assert values['__tdata_lma_end'] - values['__tdata_lma'] == td['size']
    assert name_fields(image, tcb) == dict(version=0, name_pointer=0)
    # Runtime writes to ANY native TLS object cannot change the probed fields.
    image[PREFIX_BYTES:] = b'\xff' * (len(image)-PREFIX_BYTES)
    assert name_fields(image, tcb) == dict(version=0, name_pointer=0)
    assert values['_start'] == 0, 'NRO entrypoint moved'
    return dict(elf=str(path), prefix_bytes=PREFIX_BYTES, tcb_bytes=tcb,
                tls_bytes=len(image), native_tls_objects=len(objects), thread_name_pointer=0,
                all_native_tls_objects_outside_prefix=True, start_address=0)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('elf', type=Path)
    args = ap.parse_args()
    print(json.dumps(check(args.elf), indent=2))
