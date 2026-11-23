p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Echte Aufrufargumente von fn_00108F70' not in s:
    body = """

### Echte Aufrufargumente von fn_00108F70 gemessen

An der echten Aufrufstelle (`0x101360`, zweiter `fn_001034F4`) wurden
die Argumente mitgeloggt (`build/gen_boot.c`):

```
[skip1034] 2. fn_001034F4 uebersprungen r0=003EC028 r1=0CF09A48
```

Also `r0=0x3EC028` (Welt), `r1=0x0CF09A48menue` (Zeiger auf einen
Init-Stack-Struct). Damit ist die Signatur bekannt:
`fn_001034F4(world, struct_ptr)` -> `fn_00108F70(world, struct_ptr)`.

Der Struct wird beim Init in einen stabilen Puffer kopiert
(`0x0CFE9800`, 0x100 Byte). Ein Aufruf
`HLE_FRAME_LOOP2=108F70, HLE_FRAME_ARG2=3EC028,0CFE9800,0,0` laesst
`fn_00284D24` laufen, aber der GX-Zweig bleibt aus:

```
[flowdbg] 1014EC reached r4=00000001
[initdbg] asset-block
[frameloop2] call 0x00108F70 r0=003EC028
ARM_WATCH=11E494 -> 0
```

`fn_00284D24` bricht also weiterhin vor `0x11E494` ab; der GX-Submit
braucht offenbar noch weiteren Zustand (geladene Szene/Ressourcen).
"""
    s += body.replace('0CF09A48menue', '0CF09A48')
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
