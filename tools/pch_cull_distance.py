#!/usr/bin/env python3
"""Size-aware draw distance of small static meshes in ME2 (run after every codegen; tools/post_codegen.py does).

Ported from masseffect-cinnamon's tools/pch_cull_distance.py (ME1). ME2's UE3 InitViews keeps the static mesh element
loop in its own function, sub_8269E460(SceneRenderer r3, ViewIndex r4, FPrimitiveSceneInfoCompact* r5 -> r28,
f1 = distance squared x LOD factor squared -> f31), called from sub_8269B040 after the primitive's own distance and
frustum tests. For each element (FStaticMesh* from PrimitiveSceneInfo->StaticMeshes, +20/+24) it marks
View.StaticMeshVisibilityMap (+608) bit Id (+224) when MinDrawDistanceSquared (+208) <= f31 < MaxDrawDistanceSquared
(+212). The loop is unrolled: five `lfs f0,212(r11)` sites. After each load of MaxDrawDistanceSquared this inserts

    <f0> = me2_cull_max_distance_sq(<f0>, r28.u32, base, <f31>);

which lowers it for small primitives (app/me2_src/me2_cull_distance.cpp, me2_cull_small_*; the bounds sphere radius
is FPrimitiveSceneInfoCompact +40; the distance only feeds its statistics). With the feature off the function returns
its first argument. Only static mesh elements are touched (dynamic primitives: characters, particles, effects, and
movable meshes take another path).

The function is found by searching the generated tree, and the register names are read from the generated text
(the optimised flavour renames them: ll_f0, ll_r11). Idempotent; a tree patched with the first, three-argument form is
upgraded. Exit 1 if the sites are not found as expected.
  python tools/pch_cull_distance.py --gen app/generated/default [--check]
"""
import argparse
import os
import re
import sys

FUNC = 'sub_8269E460'
MARK = 'me2_cull_max_distance_sq'
SITES = 5
DECL = 'extern "C" double %s(double max_sq, uint32_t compact, uint8_t* base, double dist_sq);' % MARK
OLD_DECL = 'extern "C" double %s(double max_sq, uint32_t compact, uint8_t* base);' % MARK
TAG = '  // tools/pch_cull_distance.py'

LOAD = re.compile(r'(\t// lfs f0,212\(r11\)\n\t(\w*f0)\.f64 = double\(REX_LOAD_D_F32\((\w*r11)\.u32, 212\)\);\n)'
                  r'(\t' + MARK + r'_PLACEHOLDER\n)?'
                  r'(?=\t// fcmpu cr6,f31,f0\n\tcr6\.compare\((\w*f31)\.f64, \2\.f64\);)')
OLD_CALL = re.compile(r'\t(\w*f0)\.f64 = ' + MARK + r'\(\1\.f64, (\w*r28)\.u32, base\);' + re.escape(TAG) + r'\n'
                      r'(?=\t// fcmpu cr6,f31,f0\n\tcr6\.compare\((\w*f31)\.f64, \1\.f64\);)')


def fail(msg):
    sys.exit('ERROR (post-codegen patch): pch_cull_distance: ' + msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app',
                                                  'generated', 'default'))
    ap.add_argument('--check', action='store_true', help='only report; exit 1 if the patch is missing')
    a = ap.parse_args()
    path = None
    for f in sorted(os.listdir(a.gen)):
        if re.match(r'\w+_recomp\.\d+\.cpp$', f):
            p = os.path.join(a.gen, f)
            if ('DEFINE_REX_FUNC(%s) {' % FUNC) in open(p, encoding='utf-8').read():
                path = p
                break
    if not path:
        fail('no %s definition in %s' % (FUNC, a.gen))
    s = open(path, encoding='utf-8').read()
    start = s.index('DEFINE_REX_FUNC(%s) {' % FUNC)
    end = s.find('\n}\n', start)
    if end < 0:
        fail('end of %s not found' % FUNC)
    body = s[start:end]
    r28 = re.search(r'\n\t((?:ctx\.)?\w*r28)\.u64 = ctx\.r5\.u64;', body)  # ctx.r28 in the classic flavour
    if not r28 or '// lwz r11,0(r28)' not in body:
        fail('r28 is not the primitive (r5) in %s' % FUNC)
    r28_name = r28.group(1)

    new_calls = body.count(MARK + '(') - len(OLD_CALL.findall(body))
    old_calls = len(OLD_CALL.findall(body))
    if old_calls:
        if old_calls != SITES:
            fail('%d first-form calls in %s (expected %d)' % (old_calls, FUNC, SITES))
        body = OLD_CALL.sub(lambda m: '\t%s.f64 = %s(%s.f64, %s.u32, base, %s.f64);%s\n' % (
            m.group(1), MARK, m.group(1), m.group(2), m.group(3), TAG), body)
        s = s[:start].replace(OLD_DECL, DECL) + body + s[end:]
        if a.check:
            sys.exit(1)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(s)
        print('pch_cull_distance: upgraded %d sites of %s to pass the distance' % (SITES, FUNC))
        return
    if new_calls == SITES:
        print('pch_cull_distance: %s already patched (%d sites) in %s' % (FUNC, new_calls, os.path.basename(path)))
        return
    if new_calls:
        fail('%s partly patched (%d of %d sites)' % (FUNC, new_calls, SITES))
    found = LOAD.findall(body)
    if not found and 'ctx.r28' in body:
        # The classic flavour (ctx.* registers, loads through temp) is only built for diagnostics (stack chains):
        # the patch matches the optimised flavour's text, and the shipped builds are optimised.
        print('pch_cull_distance: WARNING: classic-flavour code, %s not patched (diagnostic build only)' % FUNC)
        return
    if len(found) != SITES:
        fail('%d MaxDrawDistanceSquared loads found in %s (expected %d)' % (len(found), FUNC, SITES))
    if a.check:
        print('pch_cull_distance: NOT patched (%d sites) in %s' % (len(found), os.path.basename(path)))
        sys.exit(1)

    def patch(m):
        f0, f31 = m.group(2), m.group(5)
        return m.group(1) + '\t%s.f64 = %s(%s.f64, %s.u32, base, %s.f64);%s\n' % (f0, MARK, f0, r28_name, f31, TAG)

    body2 = LOAD.sub(patch, body)
    s2 = s[:start] + DECL + '\n\n' + body2 + s[end:]
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(s2)
    print('pch_cull_distance: patched %d sites of %s in %s' % (SITES, FUNC, os.path.basename(path)))


if __name__ == '__main__':
    main()
