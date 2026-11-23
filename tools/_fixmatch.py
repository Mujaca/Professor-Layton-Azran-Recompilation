p = 'tools/_match_atlas.py'
s = open(p).read()
s = s.replace('import glob Bristol', 'import glob')
s = s.replace('bm[ Terrific1]', 'bm[1]')
open(p, 'w').write(s)
print('ok')
