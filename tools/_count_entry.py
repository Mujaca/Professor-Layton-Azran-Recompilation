import re

txt = open(r'build\gen_boot.c', encoding='utf-8', errors='replace').read()
bad = 0
tot = 0
pat = (r'void fn_([0-9A-F]{8})\(ArmCpu \*c\) \{\n'
       r'(?:  if \(arm_retforce[^\n]*\n)?  goto L_([0-9A-F]{8})_([0-9A-F]{8});')
for m in re.finditer(pat, txt):
    tot += 1
    if m.group(2) != m.group(3):
        bad += 1
print('Funktionen mit goto: %d, davon falscher Einstieg: %d' % (tot, bad))
