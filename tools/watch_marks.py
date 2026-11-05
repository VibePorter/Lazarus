#!/usr/bin/env python3
"""ME2 (2026-11-04): the watch marks. Runs after tools/store_cards.py (post_codegen.py).

Renut's draw capture (queued.cpp CaptureDelta) compared 39 64-byte chunks of the D3D device with its copy at every
draw to find the 2-3 that changed (~7 % of UE3's render thread on the console, mostly cache misses). With the watch
marks, the stores that can write the device set a dirty bit for their chunk (the watched range: g_me2_watch, the app's
object; rex::memory::WatchedRange in the SDK) and the capture compares only the marked chunks.

Only the guest functions in the list file (--list, one sub_XXXXXXXX per line, '#' comments) are instrumented, and in
them only the stores store_cards.py already marks (no stack stores):
    REX_STORE_D_U32_C(r31.u32, 8, x);    ->  REX_STORE_D_U32_W(r31.u32, 8, x);
    REX_STORE_U32_C(ea, x);              ->  REX_STORE_U32_W(ea, x);
    REX_CARD(ea), simde_mm_store_si128   ->  REX_CARD(ea), RexWatchN((u32)(ea), 16), simde_mm_store_si128
    REX_CARD(ea), memset((void*)REX_RAW_ADDR(ea), 0, N)  ->  ... RexWatchN((u32)(ea), N), memset(...)
    REX_GUEST_CAS_C(                     ->  REX_GUEST_CAS_W(
    REX_MM_STORE_U32_C(                  ->  REX_MM_STORE_U32_W(
The list comes from the device census (app/me2_src/me2_write_census.cpp, me2_census_device) and the static census
(tools/rhi/devwrites.py); Renut's verify mode (me2_renut_watch_verify) reports a changed chunk nobody marked.

Idempotent; --restore puts the _C forms back and drops the macros; --check counts only.
  python tools/watch_marks.py --gen app/generated/default --list app/watch_writers.txt [--restore] [--check]
"""
import argparse
import glob
import os
import re
import sys

DEF = re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\((?:__imp__)?(\w+)\)')
STORE_C = re.compile(r'\b(REX_STORE_D_U(?:8|16|32|64)|REX_STORE_U(?:8|16|32|64)|REX_MM_STORE_U(?:8|16|32|64)|'
                     r'REX_GUEST_CAS)_C\(')
VEC = 'REX_CARD(ea), simde_mm_store_si128'
DCBZ = re.compile(r'REX_CARD\(ea\), memset\(\(void\*\)REX_RAW_ADDR\(ea\), 0, (32|128)\);')
SYNC_CAS = 'REX_CARD(ea), cr0.eq = __sync_bool_compare_and_swap'

UNDO = [
    (re.compile(r'\b(REX_STORE_D_U(?:8|16|32|64)|REX_STORE_U(?:8|16|32|64)|REX_MM_STORE_U(?:8|16|32|64)|'
                r'REX_GUEST_CAS)_W\('), r'\1_C('),
    (re.compile(r'REX_CARD\(ea\), RexWatch(?:N\(\(u32\)\(ea\), \d+\)|\(\(u32\)\(ea\)\)), '), 'REX_CARD(ea), '),
]

MARKER = '// ME2: the watch marks (tools/watch_marks.py)'
END = '// (end of the macros of tools/watch_marks.py)\n'
MACROS = MARKER + '''
// The stores of the guest functions that write the D3D device also set the dirty bit of the 64-byte chunk they write
// in the watched range (rex::memory::WatchedRange, the app's g_me2_watch): Renut's draw capture compares only those.
// Branch-free (an OR of a mask that is 0 outside the range): the instrumented functions keep their control flow, and
// the PGO profile made on it. A scalar store (aligned, 8 bytes at most) marks its first byte's chunk; a vector store
// or dcbz the chunks of its first and last byte. Nothing is watched while span is 0.
#ifndef REX_WATCHED_RANGE_DEFINED
#define REX_WATCHED_RANGE_DEFINED
struct RexWatchedRange {
  uint32_t lo;
  uint32_t span;
  uint64_t dirty[4];
};
#endif
extern "C" RexWatchedRange g_me2_watch;
static inline void RexWatch(uint32_t ea) {
  const uint32_t o = ea - g_me2_watch.lo;
  const uint64_t hit = uint64_t(0) - uint64_t(o < g_me2_watch.span);
  const uint32_t a = o >> 6;
  g_me2_watch.dirty[(a >> 6) & 3] |= hit & (uint64_t(1) << (a & 63));
}
static inline void RexWatchN(uint32_t ea, uint32_t n) {
  RexWatch(ea);
  RexWatch(ea + n - 1);
}
#define REX_STORE_D_U8_W(r, d, y) (REX_STORE_D_U8_C(r, d, y), RexWatch((u32)(r) + (u32)(d)))
#define REX_STORE_D_U16_W(r, d, y) (REX_STORE_D_U16_C(r, d, y), RexWatch((u32)(r) + (u32)(d)))
#define REX_STORE_D_U32_W(r, d, y) (REX_STORE_D_U32_C(r, d, y), RexWatch((u32)(r) + (u32)(d)))
#define REX_STORE_D_U64_W(r, d, y) (REX_STORE_D_U64_C(r, d, y), RexWatch((u32)(r) + (u32)(d)))
#define REX_STORE_U8_W(x, y) (REX_STORE_U8_C(x, y), RexWatch((u32)(x)))
#define REX_STORE_U16_W(x, y) (REX_STORE_U16_C(x, y), RexWatch((u32)(x)))
#define REX_STORE_U32_W(x, y) (REX_STORE_U32_C(x, y), RexWatch((u32)(x)))
#define REX_STORE_U64_W(x, y) (REX_STORE_U64_C(x, y), RexWatch((u32)(x)))
#define REX_MM_STORE_U8_W(a, v) do { REX_MM_STORE_U8_C(a, v); RexWatch((u32)(a)); } while (0)
#define REX_MM_STORE_U16_W(a, v) do { REX_MM_STORE_U16_C(a, v); RexWatch((u32)(a)); } while (0)
#define REX_MM_STORE_U32_W(a, v) do { REX_MM_STORE_U32_C(a, v); RexWatch((u32)(a)); } while (0)
#define REX_MM_STORE_U64_W(a, v) do { REX_MM_STORE_U64_C(a, v); RexWatch((u32)(a)); } while (0)
#define REX_GUEST_CAS_W(ea, p, e, d) (RexWatch((u32)(ea)), REX_GUEST_CAS_C(ea, p, e, d))
''' + END


def load_list(path):
    names = set()
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        token = line.split()[0]
        hexpart = token[4:] if token.startswith('sub_') else token
        if hexpart.lower().startswith('0x'):
            hexpart = hexpart[2:]
        names.add('sub_' + hexpart.upper())
    return names


def restore_text(text):
    for rx, rep in UNDO:
        text = rx.sub(rep, text)
    return text


def apply(text, names, counts, seen):
    out = []
    cur = None
    for line in text.split('\n'):
        m = DEF.match(line)
        if m:
            cur = m.group(1) if m.group(1) in names else None
            if cur:
                seen.add(cur)
        if cur and not line.lstrip().startswith('//'):
            line, n = STORE_C.subn(lambda mm: mm.group(1) + '_W(', line)
            counts['scalar'] += n
            if VEC in line:
                line = line.replace(VEC, 'REX_CARD(ea), RexWatchN((u32)(ea), 16), simde_mm_store_si128')
                counts['vector'] += 1
            dm = DCBZ.search(line)
            if dm:
                line = line.replace('REX_CARD(ea), memset', 'REX_CARD(ea), RexWatchN((u32)(ea), %s), memset' % dm.group(1), 1)
                counts['dcbz'] += 1
            if SYNC_CAS in line:
                line = line.replace(SYNC_CAS, 'REX_CARD(ea), RexWatch((u32)(ea)), cr0.eq = __sync_bool_compare_and_swap')
                counts['cas'] += 1
        out.append(line)
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--list', help='the functions to instrument (required unless --restore)')
    ap.add_argument('--restore', action='store_true')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    if not a.restore and not a.list:
        sys.exit('watch_marks: --list is required')
    names = set() if a.restore else load_list(a.list)
    counts = {'scalar': 0, 'vector': 0, 'dcbz': 0, 'cas': 0}
    seen = set()
    changed = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        new = restore_text(text)
        if not a.restore:
            new = apply(new, names, counts, seen)
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): watch_marks: no single *_pch.h in %s' % a.gen)
    text = open(pchs[0], encoding='utf-8').read()
    new = text
    if MARKER in new:
        start = new.index(MARKER)
        new = new[:start] + new[new.index(END, start) + len(END):]
    if not a.restore:
        anchor = '#define REX_LOAD_U8(x)'
        if anchor not in new:
            sys.exit('ERROR (post-codegen patch): watch_marks: no REX_LOAD_U8 in %s' % pchs[0])
        new = new.replace(anchor, MACROS + anchor, 1)
    if new != text and not a.check:
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(new)
    if a.restore:
        print('watch_marks: plain card stores restored (%d files)' % changed)
        return
    missing = sorted(names - seen)
    print('watch_marks%s: %d of %d listed functions found; marks on %d scalar, %d vector, %d dcbz, %d CAS stores '
          '(%d files)' % (' (check only)' if a.check else '', len(seen), len(names), counts['scalar'], counts['vector'],
                          counts['dcbz'], counts['cas'], changed))
    if missing:
        print('watch_marks: not in the generated code (inlined or folded? check): ' + ' '.join(missing[:40]) +
              (' ...' if len(missing) > 40 else ''))


if __name__ == '__main__':
    main()
