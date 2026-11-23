p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if '0x284A38-Caller' not in s:
    s += (
        '\n**0x284A38-Caller:** Der Ring-/Slot-Allokator `0x284A38` '
        '(von `0x28D424`/`0x2800C8` genutzt) hat 143 Aufrufer, aber '
        '**keinen** im Menue-Code (`fn_0027…`/`fn_0028…`, geprueft mit '
        '`callgraph callers --verbose`). Der Node-Add der Menue-Listen '
        'laeuft also nicht ueber diesen Allokator.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
