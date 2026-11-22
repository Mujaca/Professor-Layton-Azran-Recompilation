import os
import re

roots = ['README.md']
for d in ['docs', 'reports', '.opencode/skills']:
    for dirpath, _, files in os.walk(d):
        for f in files:
            if f.lower().endswith('.md'):
                roots.append(os.path.join(dirpath, f))

pat = re.compile(r'out[\\/]([A-Za-z0-9_./\\-]+\.(?:txt|bmp|png|bin|exe|json|ctrt))')
missing = {}
for f in roots:
    try:
        text = open(f, encoding='utf-8').read()
    except Exception:
        continue
    for m in pat.finditer(text):
        rel = m.group(1).replace('\\', '/')
        p = os.path.join('out', rel)
        if os.path.exists(p):
            continue
        base = os.path.basename(rel)
        where = None
        for sub in ('logs', 'shots', 'artifacts', 'probes'):
            if os.path.exists(os.path.join('out', sub, base)):
                where = sub
                break
        missing.setdefault((f, rel), where)
for (f, rel), where in sorted(missing.items()):
    print(f'{f}: out/{rel} -> {where or "???"}')
