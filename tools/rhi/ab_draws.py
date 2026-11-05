#!/usr/bin/env python3
"""ME2 (2026-11-04): what each draw cut saves, from the draw-count rotation's "[ab]" lines (app/me2_src/me2_ab_cycle.cpp).

Each slot with a cut off is compared with the all-on slots right before and after it (same spot if the player stands
still); the first second of every slot is left out (caches, Umbra's latent answers and LODs settle). Pairs whose two
all-on neighbours differ by more than --max-drift (the view changed) are left out.

  python tools/rhi/ab_draws.py out/pc/me2_1080p.log [--since "2026-11-04 22:00"] [--until "2026-11-04 22:05"] [--max-drift 0.08]
"""
import argparse
import collections
import re
import statistics

LINE = re.compile(r'^\[(\S+ \S+)\].*\[ab\] t (\d+) slot (\d+) \((\d+) s in\) off (\S+) frames (\d+) draws (\d+)'
                  r'(?: \([0-9.]+ a frame\) begin (\d+))?')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--since', default='')
    ap.add_argument('--until', default='9999')
    ap.add_argument('--max-drift', type=float, default=0.08)
    a = ap.parse_args()
    slots = collections.OrderedDict()  # (run, slot) -> [off, frames, draws]
    run = 0
    last_t = -1
    for text in open(a.log, encoding='utf-8', errors='replace'):
        m = LINE.search(text)
        if not m or m.group(1) < a.since or m.group(1) > a.until:
            continue
        t, slot, sec_in, off, frames, draws = int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5), \
            int(m.group(6)), int(m.group(7))
        if t < last_t:
            run += 1  # a new game session in the same log
        last_t = t
        if sec_in == 0 or frames == 0:
            continue
        e = slots.setdefault((run, slot), [off, 0, 0, 0])
        e[1] += frames
        e[2] += draws
        e[3] += int(m.group(8) or 0)  # Begin*Vertices draws (Umbra test boxes)
    keys = list(slots)
    per_cut = collections.defaultdict(list)
    skipped = collections.Counter()
    for i, k in enumerate(keys):
        off, frames, draws, _ = slots[k]
        if off == '-':
            continue
        prev = slots.get((k[0], k[1] - 1))
        nxt = slots.get((k[0], k[1] + 1))
        if not prev or not nxt or prev[0] != '-' or nxt[0] != '-':
            skipped[off] += 1
            continue
        b0, b1 = prev[2] / prev[1], nxt[2] / nxt[1]
        base = (b0 + b1) / 2
        if base <= 0 or abs(b0 - b1) / base > a.max_drift:
            skipped[off] += 1
            continue
        boxes = slots[k][3] / frames - (prev[3] / prev[1] + nxt[3] / nxt[1]) / 2
        per_cut[off].append((base, draws / frames, boxes))
    print('cut              pairs  all-on draws/f  cut-off draws/f  saved by the cut (draws/f, %)   test boxes saved/f   [pairs left out]')
    for cut in sorted(per_cut, key=lambda c: -statistics.mean(o - b for b, o, _ in per_cut[c])):
        pairs = per_cut[cut]
        base = statistics.mean(b for b, _, _ in pairs)
        offv = statistics.mean(o for _, o, _ in pairs)
        saved = [o - b for b, o, _ in pairs]
        boxes = statistics.mean(x for _, _, x in pairs)
        sd = statistics.stdev(saved) if len(saved) > 1 else 0.0
        print('%-16s %5d  %14.1f  %15.1f  %8.1f +- %5.1f  (%5.1f %%)   %10.1f          [%d]' %
              (cut, len(pairs), base, offv, statistics.mean(saved), sd, 100 * statistics.mean(saved) / base, boxes,
               skipped[cut]))
    for cut in sorted(set(skipped) - set(per_cut)):
        print('%-16s     0  (all %d pairs left out: view changed)' % (cut, skipped[cut]))


if __name__ == '__main__':
    main()
