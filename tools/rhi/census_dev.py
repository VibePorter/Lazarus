#!/usr/bin/env python3
"""ME2 (2026-11-04): the device census -> app/watch_writers.txt (tools/watch_marks.py).

Reads the "[census-dev] <file>:<line> stores N chunks +off..." lines of a census build's log (me2_census_device; the
last report of each log counts), maps each generated source line to its guest function (out/callgraph.json: the
functions' first lines per file), and writes the list of functions to instrument: the census' writers plus the static
candidates (--static: the XDK D3D range and the functions that load the device pointer, tools/rhi/devrefs.json).

  python tools/callgraph.py build      # after the codegen whose lines the census build had
  python tools/rhi/census_dev.py --log <log> [--log <log> ...] [--static <devrefs.json>] [--write app/watch_writers.txt]
"""
import argparse
import bisect
import collections
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LINE = re.compile(r'\[census-dev\] (\S+\.cpp):(\d+) stores (\d+) chunks((?: \+[0-9A-F]+)*)')
HEAD = re.compile(r'\[census-dev\] device ([0-9A-F]{8}) ')
XDK_LO, XDK_HI = 0x82A8E000, 0x82AAC000  # the XDK Direct3D library in TU2 (tools/rhi/rhi_callers.py)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', action='append', required=True)
    ap.add_argument('--callgraph', default=os.path.join(ROOT, 'out', 'callgraph.json'))
    ap.add_argument('--static', help='devrefs.json of tools/rhi/devrefs.py (functions that load the device pointer)')
    ap.add_argument('--write', help='the list file to write')
    a = ap.parse_args()
    g = json.load(open(a.callgraph))
    starts = collections.defaultdict(list)
    for name, info in g.items():
        starts[info['file']].append((info['line'], name))
    for v in starts.values():
        v.sort()
    found = {}  # function -> [stores, chunk offsets]
    unmapped = 0
    for path in a.log:
        report = {}
        for text in open(path, encoding='utf-8', errors='replace'):
            if HEAD.search(text):
                report = {}  # a new report: the counts are cumulative, the last one has everything
                continue
            m = LINE.search(text)
            if m:
                report[(m.group(1), int(m.group(2)))] = (int(m.group(3)), m.group(4).split())
        for (f, line), (stores, chunks) in report.items():
            lst = starts.get(f)
            i = bisect.bisect_right(lst, (line, '￿')) - 1 if lst else -1
            if i < 0:
                unmapped += 1
                continue
            name = lst[i][1]
            e = found.setdefault(name, [0, set()])
            e[0] += stores
            e[1].update(chunks)
    print('%d writers from the census (%d lines unmapped)' % (len(found), unmapped))
    for name, (stores, chunks) in sorted(found.items(), key=lambda kv: -kv[1][0]):
        a_ = int(name[4:], 16) if name.startswith('sub_') else 0
        where = 'XDK' if XDK_LO <= a_ < XDK_HI else 'RHI' if 0x827D8000 <= a_ < 0x827E5000 else 'UE3'
        print('  %s %-4s stores %9d chunks %s' % (name, where, stores, ' '.join(sorted(chunks, key=lambda c: int(c, 16)))))
    names = set(found)
    static = set()
    if a.static:
        static |= {'sub_' + x for x in json.load(open(a.static))}
    static |= {n for n in g if n.startswith('sub_') and XDK_LO <= int(n[4:], 16) < XDK_HI}
    print('%d static candidates; %d census writers outside them' % (len(static), len(names - static)))
    if a.write:
        with open(a.write, 'w', encoding='utf-8', newline='\n') as out:
            out.write('# ME2 (2026-11-04): guest functions whose stores mark the watched D3D device chunks '
                      '(tools/watch_marks.py).\n# Written by tools/rhi/census_dev.py: census writers, then static '
                      'candidates (XDK D3D range, device-pointer loaders).\n')
            out.write('# census\n')
            for n in sorted(names):
                out.write(n + '\n')
            out.write('# static\n')
            for n in sorted(static - names):
                out.write(n + '\n')
        print('wrote %s (%d functions)' % (a.write, len(names | static)))


if __name__ == '__main__':
    main()
