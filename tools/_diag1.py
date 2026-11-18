import io

L = [l.rstrip() for l in io.open('out/boot_log.txt', encoding='utf-8', errors='replace')]
idx = [i for i, l in enumerate(L) if 'errctx' in l]
print('errctx at', idx)
for k in idx:
    for l in L[max(0, k - 8):k + 2]:
        print(' ', l)

print('=== thread dumps ===')
E = [l.rstrip() for l in io.open('out/boot_err.txt', encoding='utf-8', errors='replace')]
m = [i for i, l in enumerate(E) if 'tdump bei tick' in l]
print('dumps', m)
for k in m[-1:]:
    print(E[k])
    seg = []
    j = k + 1
    while j < len(E) and E[j].startswith('[tdump]'):
        seg.append(E[j])
        j += 1
    print('\n'.join(seg[:22]))
