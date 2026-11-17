import io

p = 'runtime/hle/hle.c'
s = io.open(p, encoding='utf-8').read()

old = '''        case 0x24: /* WaitSynchronization1 */
            guest_yield();
            return 0;
        case 0x25: /* WaitSynchronizationN(out, handles, count, wait_all, timeout) */
            guest_yield();
            if (c->r[0]) write_u32(c, c->r[0], 0);
            return 0;'''
new = '''        case 0x24: /* WaitSynchronization1 */
            for (int i = 0; i < 4; i++) guest_yield();
            return 0;
        case 0x25: /* WaitSynchronizationN(out, handles, count, wait_all, timeout) */
            for (int i = 0; i < 4; i++) guest_yield();
            if (c->r[0]) write_u32(c, c->r[0], 0);
            return 0;'''
if old not in s:
    raise SystemExit('anchor fehlt')
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
