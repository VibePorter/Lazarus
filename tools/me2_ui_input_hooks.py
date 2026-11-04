"""Keep the GUI script entry fix and UE3-to-GFx diagnostic hooks reachable after codegen."""
from pathlib import Path
import argparse


def route(directory):
    changed = []
    for path in sorted(Path(directory).glob('sa2_recomp.*.cpp')):
        before = path.read_bytes()
        after = before
        for address in [b'8273BE00', b'82781F70', b'825E9DA8']:
            after = after.replace(b'__imp__sub_' + address + b'(ctx, base);', b'sub_' + address + b'(ctx, base);')
        if after != before:
            path.write_bytes(after)
            changed.append(path)
    return changed


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--gen', type=Path, required=True)
    args = ap.parse_args()
    print('ME2 UI input hooks: rerouted', len(route(args.gen)), 'files')
