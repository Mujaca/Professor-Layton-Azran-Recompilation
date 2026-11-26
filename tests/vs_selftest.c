/* Vertex-Shading-Selbsttest: paralleles, deduplizierendes Vertex-Shading
 * (p_vs_shade_draw in runtime/gpu/gpu.c) gegen den seriellen Pfad
 * (p_load_attrs + p_vs_process je Draw-Position, wie vor dem Umbau).
 *
 * Fuer viele zufaellige Vertex-Programme (Arithmetik inkl. DP/MUL/MAD mit
 * 0*inf, RCP/RSQ/EX2/LG2, CMP, MOVA + relative Adressierung sowie
 * IF/LOOP/CALL/JMP/BREAKC), zufaellige Swizzles, Uniforms und Vertexdaten
 * (inkl. 0, +-inf, NaN, Denormale) und zufaellige Index-Puffer (u8/u16, mit
 * Wiederholungen) bzw. nicht indizierte Draws mit Offset muss jede
 * Position bitgleich (memcmp) zur seriellen Referenz sein -- mit
 * Worker-Pool (parallel) und ohne (seriell, nur Deduplizierung). Zusaetzlich
 * muss die Zahl der gerechneten Vertices der Zahl eindeutiger Indizes
 * entsprechen.
 *
 *   python tests/vs_test.py [Faelle]
 */
#include "../runtime/gpu/gpu.c"

/* Stubs: arm_cpu.c referenziert die Trace-Runtime. */
uint32_t arm_last_block = 0;
void arm_trace_svc(uint32_t imm) { (void)imm; }

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static float rnd_val(void)
{
    uint32_t k = rnd() % 32u;
    float f;
    switch (k) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return INFINITY;
    case 3: return -INFINITY;
    case 4: return NAN;
    case 5: { uint32_t b = rnd() & 0x807FFFFFu; memcpy(&f, &b, 4); return f; } /* denormal */
    case 6: return 1.0f;
    case 7: return (float)(rnd() % 7u) - 3.0f;
    default: break;
    }
    f = ((float)(rnd() & 0xFFFFFFu) / 16777216.0f - 0.5f) * 8.0f;
    if ((rnd() & 7u) == 0u) f *= 1e20f;
    return f;
}

#define VBUF   0x00010000u      /* Vertexdaten (64 Byte je Vertex) */
#define IBUF   0x00008000u      /* Indexpuffer (relativ zu VBUF-Basis) */
#define NVERT  300u

static const uint32_t arith_ops[] = {
    0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
    0x0E, 0x0F, 0x12, 0x13, 0x18, 0x1A, 0x1B, 0x2E
};

static uint32_t rnd_arith(void)
{
    uint32_t op = arith_ops[rnd() % (sizeof(arith_ops) / sizeof(arith_ops[0]))];
    int inv = (op >= 0x18u && op <= 0x1Bu);
    uint32_t desc = rnd() & 0x7Fu;
    uint32_t ari = rnd() & 3u;
    uint32_t dest = rnd() & 0x1Fu;
    uint32_t s1, s2;
    if (inv) { s1 = rnd() & 0x1Fu; s2 = rnd() & 0x7Fu; }
    else { s1 = rnd() & 0x7Fu; s2 = rnd() & 0x1Fu; }
    if (op == 0x2Eu)   /* CMP: Bits 21..26 sind die Vergleichsoperatoren */
        return (op << 26) | ((rnd() & 0x3Fu) << 21) | (ari << 19) |
               (s1 << 12) | (s2 << 7) | desc;
    if (inv)
        return (op << 26) | (dest << 21) | (ari << 19) | (s1 << 14) | (s2 << 7) | desc;
    return (op << 26) | (dest << 21) | (ari << 19) | (s1 << 12) | (s2 << 7) | desc;
}

static uint32_t rnd_mad(void)
{
    uint32_t op = 0x30u + (rnd() & 0xFu);
    return (op << 26) | (rnd() & 0x03FFFFFFu);
}

/* Flusskontrolle nur vorwaerts (Ziel > pc), damit das Programm endet. */
static uint32_t rnd_flow(uint32_t pc, uint32_t len)
{
    static const uint32_t ops[] = {0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2C, 0x2D, 0x21};
    uint32_t op = ops[rnd() % (sizeof(ops) / sizeof(ops[0]))];
    uint32_t room = len - 1u - pc;
    uint32_t dest = pc + 1u + (room > 1u ? rnd() % room : 0u);
    uint32_t num = rnd() % 4u;
    return (op << 26) | (rnd() & (0xFu << 22)) | (dest << 10) | num;
}

static void random_program(void)
{
    uint32_t len = 8u + rnd() % 40u, pc;
    g_vs.code_words = 0;
    for (pc = 0; pc < len - 1u; pc++) {
        uint32_t k = rnd() % 10u;
        uint32_t w;
        if (k < 6u) w = rnd_arith();
        else if (k < 8u) w = rnd_mad();
        else w = rnd_flow(pc, len);
        g_vs.code[pc] = w;
    }
    g_vs.code[len - 1u] = 0x22u << 26;                 /* END */
    g_vs.code_words = len;
    for (pc = 0; pc < 128u; pc++) {
        uint32_t s = rnd();
        if ((rnd() & 3u) == 0u) s = (s & ~0xFu) | 0xFu;  /* oft volle Maske */
        g_vs.swz[pc] = s;
    }
    for (pc = 0; pc < 96u; pc++) {
        int k;
        for (k = 0; k < 4; k++) g_vs.f[pc][k] = rnd_val();
    }
    for (pc = 0; pc < 16u; pc++) g_vs.b[pc] = (unsigned char)(rnd() & 1u);
    for (pc = 0; pc < 4u; pc++) {
        g_vs.i[pc][0] = (unsigned char)(rnd() % 5u);   /* Schleifenzahl */
        g_vs.i[pc][1] = (unsigned char)(rnd() & 0xFFu);
        g_vs.i[pc][2] = (unsigned char)(rnd() & 0xFFu);
        g_vs.i[pc][3] = 0;
    }
}

static void random_regs(void)
{
    uint32_t k;
    p_regs[0x200] = VBUF >> 3;                      /* Basis = VBUF */
    p_regs[0x201] = 0xFFFFu;                        /* attr0..3 float4 */
    /* 4 Attribute, attr3 mal als Default-Attribut */
    p_regs[0x202] = (3u << 28) | ((rnd() & 1u) ? (1u << 19) : 0u);
    p_regs[0x203] = 0u;
    p_regs[0x204] = 0x3210u;
    p_regs[0x205] = (4u << 28) | (64u << 16);
    for (k = 1; k < 12; k++) {
        p_regs[0x203 + k * 3] = 0; p_regs[0x204 + k * 3] = 0; p_regs[0x205 + k * 3] = 0;
    }
    p_regs[0x2B9] = 3u;
    /* Input-Map: zufaellige Permutation der Register 0..15 fuer attr0..3 */
    p_regs[0x2BB] = (rnd() & 0xFu) | ((rnd() & 0xFu) << 4) | ((rnd() & 0xFu) << 8) |
                    ((rnd() & 0xFu) << 12) | 0xFEDC0000u;
    p_regs[0x2BC] = 0u;
    p_regs[0x2BD] = rnd() & 0xFFFFu;
    p_regs[0x2BA] = 0u;
    for (k = 0; k < 16; k++) {
        int j;
        for (j = 0; j < 4; j++) g_vs_def_attr[k][j] = rnd_val();
    }
}

static int run_case(ArmCpu *c, unsigned char *mem, int indexed, uint32_t nv, int u16,
                    int par)
{
    static float ref[0x2000][16][4];
    static unsigned char seen[0x10000];
    uint32_t i, voff = 0, distinct = 0;
    unsigned long long run0;
    int bad = 0;

    if (nv > 0x2000u) nv = 0x2000u;
    if (indexed) {
        p_regs[0x227] = IBUF | (u16 ? 0x80000000u : 0u);
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < nv; i++) {
            uint32_t v = rnd() % (u16 ? NVERT : 256u);
            if ((rnd() & 3u) == 0u && i >= 3u) v = (v & 1u) ? i - 1u : i - 3u; /* Reuse */
            if (u16) {
                v %= NVERT;
                memcpy(mem + VBUF + IBUF + i * 2u, &(uint16_t){(uint16_t)v}, 2);
            } else {
                v %= 256u;
                mem[VBUF + IBUF + i] = (unsigned char)v;
            }
            if (!seen[v]) { seen[v] = 1; distinct++; }
        }
    } else {
        voff = rnd() % 32u;
        if (nv + voff > NVERT) nv = NVERT - voff;
        distinct = nv;
    }
    for (i = 0; i < nv; i++) {
        float attrs[16][4];
        uint32_t vert = indexed ? p_index_vertex(c, i) : (i + voff);
        p_load_attrs(c, vert, attrs);
        p_vs_process(c, (const float (*)[4])attrs, ref[i]);
    }
    g_vs_par_enabled = par;
    run0 = g_vd_run_n;
    if (!p_vs_shade_draw(c, indexed, nv, voff)) {
        printf("  kein Speicher\n");
        return 1;
    }
    if (g_vd_run_n - run0 != distinct) {
        printf("  FEHL: %llu Vertices gerechnet, %u eindeutig\n",
               g_vd_run_n - run0, distinct);
        bad++;
    }
    for (i = 0; i < nv; i++) {
        if (memcmp(ref[i], g_vd_outs[g_vd_slot[i]], sizeof(ref[i])) != 0) {
            if (bad < 3) {
                int r, k;
                printf("  FEHL Position %u (idx=%d par=%d nv=%u):", i, indexed, par, nv);
                for (r = 0; r < 16; r++)
                    for (k = 0; k < 4; k++)
                        if (memcmp(&ref[i][r][k], &g_vd_outs[g_vd_slot[i]][r][k], 4))
                            printf(" o%d.%d %g!=%g (%08X/%08X)", r, k, ref[i][r][k],
                                   g_vd_outs[g_vd_slot[i]][r][k],
                                   *(const uint32_t *)&ref[i][r][k],
                                   *(const uint32_t *)&g_vd_outs[g_vd_slot[i]][r][k]);
                printf("\n");
            }
            bad++;
        }
    }
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    static unsigned char mem[0x80000];
    static const uint32_t sizes[] = {3, 40, 95, 96, 97, 300, 1500, 6000};
    ArmCpu c;
    int cases = 300, n, fails = 0, total = 0;
    unsigned long long par0;
    if (argc > 1) cases = atoi(argv[1]);
    memset(&c, 0, sizeof(c));
    arm_cpu_init(&c, mem, sizeof(mem));
    gpu_pica_set_raster_threads(3);
    gpu_pica_reset();
    par0 = g_vd_par_n;

    for (n = 0; n < cases; n++) {
        uint32_t i;
        int indexed = (n % 3) != 2;
        uint32_t nv = sizes[rnd() % (sizeof(sizes) / sizeof(sizes[0]))];
        random_program();
        random_regs();
        for (i = 0; i < NVERT * 16u; i++) {
            float f = rnd_val();
            memcpy(mem + VBUF + i * 4u, &f, 4);
        }
        /* Rundungsmodus des aufrufenden Threads (Gast-VFP: oft
           round-toward-zero) muss auch in den Workern gelten. */
        fesetround((n & 2) ? FE_TOWARDZERO : FE_TONEAREST);
        fails += run_case(&c, mem, indexed, nv, (n & 1) != 0, 1);
        fails += run_case(&c, mem, indexed, nv, (n & 1) != 0, 0);
        if (fegetround() != ((n & 2) ? FE_TOWARDZERO : FE_TONEAREST)) {
            printf("FEHL: Rundungsmodus des Aufrufers veraendert\n");
            fails++;
        }
        total += 2;
    }
    if (g_vd_par_n == par0) {
        printf("FEHL: kein Draw lief parallel\n");
        fails++;
    }
    printf("%s Vertex-Shading parallel/dedupliziert gegen seriell: %d/%d Faelle "
           "abweichend, %llu Vertices parallel\n", fails ? "FEHL" : "OK  ", fails, total,
           g_vd_par_n - par0);
    printf(fails ? "FEHLGESCHLAGEN (%d)\n" : "ALLE OK\n", fails);
    return fails ? 1 : 0;
}
