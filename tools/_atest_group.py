"""Analyse der linearen SAD-Treffer: kohaerente Translationen + Komposition."""
import collections
import json
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
rows = json.load(open('tools/out/atest_tiles_lin.json'))
TS = 16
print('Kacheln', len(rows))
sads = np.array([r['sad'] for r in rows])
print('SAD median %.2f' % np.median(sads))

byatl = collections.defaultdict(list)
for r in rows:
    if r['atlas']:
        byatl[r['atlas']].append(r)

print('--- Top-Atlanten + Translationen ---')
for atl, rs in sorted(byatl.items(), key=lambda kv: -len(kv[1]))[:10]:
    tr = collections.Counter((r['u'] - r['tile'][0], r['v'] - r['tile'][1])
                             for r in rs)
    top = tr.most_common(4)
    print('%-30s %2d Kacheln meanSAD %5.2f  translationen %s'
          % (atl, len(rs), float(np.mean([r['sad'] for r in rs])),
             ', '.join('(%d,%d)x%d' % (k[0], k[1], v) for k, v in top)))
    # Gruppen mit >=3 Kacheln gleicher Translation = Rechteck-Kandidat
    for (dx, dy), cnt in tr.items():
        if cnt < 3:
            continue
        grp = [r for r in rs if (r['u'] - r['tile'][0], r['v'] - r['tile'][1])
               == (dx, dy)]
        xs = [r['tile'][0] for r in grp]
        ys = [r['tile'][1] for r in grp]
        print('   Translation (%4d,%4d): %d Kacheln  screen %d,%d..%d,%d '
              'atlas %d,%d  meanSAD %.1f'
              % (dx, dy, cnt, min(xs), min(ys), max(xs) + TS, max(ys) + TS,
                 min(r['u'] for r in grp), min(r['v'] for r in grp),
                 float(np.mean([r['sad'] for r in grp]))))
