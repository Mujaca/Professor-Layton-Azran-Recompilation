p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'GSP-Shared-Memory-Pointer fehlt' not in s:
    body = """

### GSP-Shared-Memory-Pointer fehlt im Engine-Zustand

Messungen im Komplettlauf (`HLE_ENGINE_MENU=1 HLE_MENU_OBJ0=1
HLE_DRAW_LIST=1 HLE_UNIT_XFER=1`):

1. `svc #0x1F` (MapMemoryBlock) wird **nie** ausgefuehrt:
   `ARM_WATCH` auf `0x11F338`, `0x12E4F0`, `0x11F350` = 0 Treffer.
2. Ein erzwungener Trigger ueber `HLE_FRAME_LOOP2=3418BC`
   (`0x3418BC` = cmd `0x000C`, normal=1) laeuft 42x, liefert aber
   weiterhin `qops=0`.
3. Beide Kandidaten-CommandBuffer sind komplett null
   (`out/logs/shmd_log.txt`):
   `[cmdq] shm10000800: 00000000 ...`
   `[cmdq] shm10002800: 00000000 ...`
4. Der GSP-Modulzustand `0x3D91E8` enthaelt nur Dienstnamen:
   ```
   gspstate: 00000000 003CBF4E 003CBF45 00000003 00000000 ...
   ```
   (`0x3CBF4E`/`0x3CBF45` zeigen auf `"gsp::Gpu"`-Strings).
   `0x3D7C40` ist komplett 0; der Handle-Slot `0x3DA6DC` = `2`.
5. `0x10000000` wird nur an `0x26C620` und `0x3B4804` referenziert.

Damit fehlt der Engine der Zeiger auf den GSP-Shared-Memory-Block;
ohne ihn kann sie keinen CommandBuffer schreiben und keinen Trigger
sinnvoll absetzen.

**Naechster Schritt:** Das Engine-Global suchen, das die gemappte
Shared-Memory-Adresse aufnehmen soll (der Schreiber des
`CommandBuffer` liest es), und es auf die vom HLE benutzte Basis
setzen. Danach schreibt `GX_SubmitGxCommand` seinen
`SubmitCmdList`-Eintrag, und `TriggerCmdReqQueue` liefert `qops>0`.
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
