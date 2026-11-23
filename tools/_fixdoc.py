p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
s = s.replace('r' + chr(0x9AD8) + chr(0x6821) + '1', 'r1')
open(p, 'w', encoding='utf-8', newline='\n').write(s)
bad = [c for c in s if 0x3000 <= ord(c) <= 0x9FFF or 0xAC00 <= ord(c) <= 0xD7AF]
print('cjk', len(bad), bad[:3])
