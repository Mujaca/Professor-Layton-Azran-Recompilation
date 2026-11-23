"""Vergleich echte Referenz vs. Kontrollreferenz (Kachel-SAD)."""
import json

import numpy as np

for name in ('atest_tiles_sad.json', 'atest_tiles_sad_control.json'):
    rows = json.load(open('tools/out/' + name))
    s = np.array([r['sad'] for r in rows])
    print('%-28s min %6.2f median %6.2f mean %6.2f  <=10: %3d  <=15: %3d'
          % (name, s.min(), np.median(s), s.mean(),
             int((s <= 10).sum()), int((s <= 15).sum())))
