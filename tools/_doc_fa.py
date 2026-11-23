p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'FA-Read-Pfad' not in s:
    s += (
        '\n**FA-Read-Pfad im Menue nicht aktiv:** Hook auf den '
        'FA-Stream-Leser `0x280744` (Build-Patch fuer `hle_fa_read`) '
        'zeigt im Menue-Lauf **0 Treffer**; nur `0x28F730` (Sync-Init) '
        'wird 2x gerufen (`out/logs/fa_log.txt`). Es werden also gar '
        'keine `.fa`-Streams gelesen. Zudem injiziert der Port bereits '
        'HID-Eingaben (START/A alternierend alle 30 Frames, '
        '`runtime/hle/hle.c` Zeile ~3386) — Eingaben sind also nicht '
        'der Blocker.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
