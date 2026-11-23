"""Translationen der Bottom-Treffer (Positivkontrolle)."""
import collections
import json

rows = json.load(open('tools/out/atest_tiles_lin_menu_official_bottom.bin.json'))
by = collections.defaultdict(list)
for r in rows:
    if r['atlas']:
        by[r['atlas']].append(r)
for atl, rs in sorted(by.items(), key=lambda kv: -len(kv[1]))[:3]:
    tr = collections.Counter((r['u'] - r['tile'][0], r['v'] - r['tile'][1])
                             for r in rs)
    print('%-30s %3d Kacheln meanSAD %.2f' % (atl, len(rs), sum(
        r['sad'] for r in rs) / len(rs)))
    for k, v in tr.most_common(3):
        print('   translation', k, '->', v, 'Kacheln')
