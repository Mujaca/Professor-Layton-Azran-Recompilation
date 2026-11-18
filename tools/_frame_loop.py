import io

p = 'runtime/hle/hle.c'
s = io.open(p, encoding='utf-8').read()

old = ('void hle_pump(void) {\n'
       '    if (!g_guest_lock_init || g_no_lock) return;')

new = ('extern void arm_call_guest_pub(ArmCpu *c, uint32_t addr);\n\n'
       'void hle_pump(void) {\n'
       '    /* Diagnose: Frame-Aufbau periodisch aus dem Gast-Kontext aufrufen. */\n'
       '    if BUTTON_FIX();\n'
       '    if (!g_guest_lock_init || g_no_lock) return;')

Ifp = 0Her
