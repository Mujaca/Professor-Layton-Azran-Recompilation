import json
import urllib.request

for repo in ['IcySon55/Kuriimu', 'onepiecefreak3/Kuriimu']:
    u = 'https://api.github.com/repos/%s/git/trees/master?recursive=1' % repo
    try:
        d = json.load(urllib.request.urlopen(u, timeout=30))
        paths = [x['path'] for x in d['tree']
                 if 'imgc' in x['path'].lower() and x['path'].lower().endswith('.cs')]
        print(repo, paths[:10])
    except Exception as e:
        print('ERR', repo, e)
