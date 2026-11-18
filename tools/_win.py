import io

L = [l.rstrip() for l in io.open('out/boot_log.txt', encoding='utf-8', errors='replace')]
k = [i for i, l in enumerate(L) if 'gspinit' in l]
print('idx', k)
if k:
    a = max(0, k[0] - 3)
    b = min(len(L), k[-1] + 14)
    for l in L[a:b]:
        print(l)
