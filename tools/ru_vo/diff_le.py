"""Diff original vs Russian LE packages export by export; tally changed classes."""
import collections, json, os, sys
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import le_package as L

BASE = os.environ.get('ME2_LE_DIR', '')
RU = os.path.join(os.environ.get('RU_VO_WORK') or os.path.join(os.getcwd(), 'ru_vo_work'), 'le_ru')


def job(rel):
    a = L.Package(open(os.path.join(BASE, rel), 'rb').read())
    b = L.Package(open(os.path.join(RU, rel), 'rb').read())
    res = {'rel': rel, 'n': (len(a.exports), len(b.exports)), 'names': (len(a.names), len(b.names)),
           'imports': (len(a.imports), len(b.imports)), 'changed': collections.Counter(), 'same': collections.Counter()}
    ia = {a.full_name(i + 1): e for i, e in enumerate(a.exports)}
    for i, e in enumerate(b.exports):
        fn = b.full_name(i + 1)
        c = b.class_name(e)
        ea = ia.get(fn)
        if ea is None:
            res['changed']['NEW:' + c] += 1
        elif a.data(ea) != b.data(e):
            res['changed'][c] += 1
        else:
            res['same'][c] += 1
    return res


if __name__ == '__main__':
    rows = [json.loads(l) for l in open('manifest.jsonl', encoding='utf-8')]
    rels = [r['path'][len('Game/ME2/'):] for r in rows if r['path'].lower().endswith('.pcc')]
    tot_c, tot_s = collections.Counter(), collections.Counter()
    odd = []
    with ProcessPoolExecutor(max_workers=12) as ex:
        for res in ex.map(job, rels):
            tot_c.update(res['changed']); tot_s.update(res['same'])
            if res['n'][0] != res['n'][1] or res['names'][0] != res['names'][1] or res['imports'][0] != res['imports'][1]:
                odd.append((res['rel'], res['n'], res['names'], res['imports']))
    print('CHANGED', tot_c.most_common())
    print('UNCHANGED (top)', tot_s.most_common(15))
    print('table size changes', len(odd))
    for o in odd[:15]: print(o)
