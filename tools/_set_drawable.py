import json
p = r'tools\profiles\chain.json'
d = json.load(open(p, encoding='utf-8'))
e = d['env']
e['HLE_POKE'] = e['HLE_POKE'].replace('0D7F2014=002812EC', '0D7F2014=0011DB84')
if '0D7F2000=0D7F2000' not in e['HLE_POKE']:
    e['HLE_POKE'] += ',0D7F2000=0D7F2000'
e['HLE_POSTCALL'] = ('297618,3ECB68,42B860,0D300100,0;@0D600004=00000006;'
                     '@0D600010=0D7F2000;2669C0,42B7AC,0D600000,0,0;'
                     '13A4B0,42BB10,0,0,0')
json.dump(d, open(p, 'w', encoding='utf-8'), indent=2, ensure_ascii=False)
open(p, 'a', encoding='utf-8').write('\n')
print('ok')
