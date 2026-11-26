/* Fragment-Lighting-Selbsttest: runtime/gpu/gpu.c gegen eine getreue
 * C-Uebertragung von Azahar.
 *
 * Teil A (Zufallsvergleich): p_compute_fragments (gpu.c) gegen
 *   ref_compute_fragments_colors = Uebertragung von
 *   tools/azahar/src/video_core/renderer_software/sw_lighting.cpp
 *   (SwRenderer::ComputeFragmentsColors) mit der Registerdekodierung aus
 *   tools/azahar/src/video_core/pica/regs_lighting.h, LutEntry aus
 *   pica/pica_core.h (value 0.0.12 / 4095, difference 0.0.11 / 2047,
 *   neg_difference Bit23) und f16/f20 aus pica_types.h (Float<M,E>::FromRaw).
 *   Die Quaternion-Normierung vor dem Aufruf entspricht sw_rasterizer.cpp
 *   (Quaternion::Normalized, common/quaternion.h). Fuer viele zufaellige,
 *   gueltige Registerzustaende (Lichtanzahl, Slots, LUT-Eingaenge/abs/
 *   Skalierung, Spot, Fresnel, config0/config1, Bump-Modi, LUT-Inhalte,
 *   Quaternion, View, Texturfarben) muessen Primary/Secondary auf 1 LSB
 *   uebereinstimmen.
 *
 * Teil B (Interpolation): echte Draws ueber gpu_run_pica_list mit je Vertex
 *   verschiedenem w, View und Quaternion. Azahar sw_rasterizer.cpp
 *   MakeScreenCoords multipliziert quat/view/tc0_w mit 1/w, und
 *   get_interpolated_attribute teilt durch die interpolierte 1/w-Summe
 *   (perspektivkorrekt). Erwartung je Pixel aus diesen Formeln.
 *
 *   python tests/lighting_test.py
 */
#include "../runtime/gpu/gpu.c"

/* Stubs: arm_cpu.c referenziert die Trace-Runtime. */
uint32_t arm_last_block = 0;
void arm_trace_svc(uint32_t imm) { (void)imm; }

static int fails = 0;

/* ---------------- Zufall (xorshift, reproduzierbar) ---------------- */
static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}
static float rndf(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rnd() & 0xFFFFFFu) / 16777215.0f;
}

/* ============ Referenz: Azahar (getreue Uebertragung) ============ */

/* pica_types.h Float<M,E>::FromRaw, unabhaengig per ldexp formuliert. */
static float ref_float_raw(uint32_t hex, int M, int E)
{
    uint32_t mant = hex & ((1u << M) - 1u);
    uint32_t ex = (hex >> M) & ((1u << E) - 1u);
    int sign = (hex >> (M + E)) & 1u;
    int bias = (1 << (E - 1)) - 1;
    float v;
    if (ex == 0u && mant == 0u) v = 0.0f;
    else if (ex == 0u) v = ldexpf((float)mant, 1 - bias - M);
    else if (ex == (1u << E) - 1u) v = mant ? NAN : INFINITY;
    else v = ldexpf(1.0f + (float)mant / (float)(1u << M), (int)ex - bias);
    return sign ? -v : v;
}

typedef struct { float x, y, z; } V3;
static V3 v3(float x, float y, float z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }
static V3 v3_add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 v3_sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 v3_mul(V3 a, float f) { return v3(a.x * f, a.y * f, a.z * f); }
static V3 v3_mulv(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static V3 v3_div(V3 a, float f) { return v3(a.x / f, a.y / f, a.z / f); }
static float v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float v3_len2(V3 a) { return a.x * a.x + a.y * a.y + a.z * a.z; }
static float v3_len(V3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static V3 v3_normalized(V3 a) { return v3_div(a, v3_len(a)); }
static V3 v3_cross(V3 a, V3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
/* common/quaternion.h QuaternionRotate */
static V3 quat_rotate(V3 qv, float qw, V3 v)
{
    return v3_add(v, v3_mul(v3_cross(qv, v3_add(v3_cross(qv, v), v3_mul(v, qw))), 2.0f));
}
/* regs_lighting.h LightColor::ToVec3f */
static V3 light_color(uint32_t v)
{
    return v3((float)((v >> 20) & 0x3FFu) / 255.f, (float)((v >> 10) & 0x3FFu) / 255.f,
              (float)(v & 0x3FFu) / 255.f);
}
static int sext13(uint32_t v) { return ((int32_t)(v << 19)) >> 19; }

static float lut_scale_of(uint32_t s)
{
    switch (s) {
    case 0: return 1.0f; case 1: return 2.0f; case 2: return 4.0f; case 3: return 8.0f;
    case 6: return 0.25f; case 7: return 0.5f;
    }
    return 0.0f;
}

/* regs_lighting.h IsLightingSamplerSupported; sampler 8 = Spotlight */
static int ref_supported(uint32_t config, int sampler)
{
    switch (sampler) {
    case 0: return config != 1;
    case 1: return config != 0 && config != 1 && config != 5;
    case 8: return config != 2 && config != 3;
    case 3: return config != 0 && config != 2 && config != 4;
    case 6: return config != 3;
    case 5: case 4: return config == 4 || config == 5 || config == 8;
    }
    return 0;
}

typedef struct {
    const uint32_t *R;               /* Registerdatei (Index = PICA-Register) */
    const uint32_t (*lut)[256];      /* 24 LUTs, Roh-LutEntry */
} RefState;

static float ref_lookup(const RefState *st, unsigned lut_index, uint8_t index, float delta)
{
    uint32_t e = st->lut[lut_index][index];
    float value = (float)(e & 0xFFFu) / 4095.f;
    float diff = (float)((e >> 12) & 0x7FFu) / 2047.f;
    if ((e >> 23) & 1u) diff = -diff;
    return value + diff * delta;
}

typedef struct {
    const RefState *st;
    V3 normal, tangent, half_vector, norm_view, light_vector;
    uint32_t lbase, config, light_cfg;
} RefCtx;

/* sw_lighting.cpp get_lut_value-Lambda */
static float ref_get_lut_value(const RefCtx *c, uint32_t input, int abs_on,
                               uint32_t scale_enum, unsigned sampler)
{
    const uint32_t *R = c->st->R;
    float result = 0.0f;
    uint8_t index;
    float delta;
    switch (input) {
    case 0: result = v3_dot(c->normal, v3_normalized(c->half_vector)); break;
    case 1: result = v3_dot(c->norm_view, v3_normalized(c->half_vector)); break;
    case 2: result = v3_dot(c->normal, c->norm_view); break;
    case 3: result = v3_dot(c->light_vector, c->normal); break;
    case 4: {
        V3 sd = v3((float)sext13(R[c->lbase + 6]), (float)sext13(R[c->lbase + 6] >> 16),
                   (float)sext13(R[c->lbase + 7]));
        result = v3_dot(c->light_vector, v3_div(sd, 2047.0f));
        break;
    }
    case 5:
        if (c->config == 8) {
            V3 nh = v3_normalized(c->half_vector);
            V3 proj = v3_sub(nh, v3_mul(c->normal, v3_dot(c->normal, nh)));
            result = v3_dot(proj, c->tangent);
        } else {
            result = 0.0f;
        }
        break;
    default: result = 0.0f;
    }
    if (abs_on) {
        float flr;
        if (c->light_cfg & 2u) result = fabsf(result);
        else result = result > 0.0f ? result : 0.0f;   /* std::max(result, 0) */
        flr = floorf(result * 256.0f);
        index = (uint8_t)(flr < 0.0f ? 0.0f : (flr > 255.0f ? 255.0f : flr));
        delta = result * 256 - index;
    } else {
        float flr = floorf(result * 128.0f);
        int8_t si = (int8_t)(flr < -128.0f ? -128.0f : (flr > 127.0f ? 127.0f : flr));
        delta = result * 128.0f - si;
        index = (uint8_t)si;
    }
    return lut_scale_of(scale_enum) * ref_lookup(c->st, sampler, index, delta);
}

static uint8_t clamp_u8(float v)
{
    float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (uint8_t)(c * 255);
}

/* sw_lighting.cpp ComputeFragmentsColors; normquat bereits normiert. */
static void ref_compute(const RefState *st, const float nq[4], const float viewa[3],
                        const uint8_t tex[4][4], uint8_t outp[4], uint8_t outs[4])
{
    const uint32_t *R = st->R;
    const uint32_t config0 = R[0x1C3], config1 = R[0x1C4];
    const uint32_t config = (config0 >> 4) & 0xFu;
    const uint32_t bump_mode = (config0 >> 28) & 3u;
    const uint32_t max_light = R[0x1C2] & 7u;
    const uint32_t lut_input = R[0x1D1], abs_in = R[0x1D0], lut_scale = R[0x1D2];
    float shadow[4];
    V3 surface_normal = v3(0, 0, 0), surface_tangent = v3(0, 0, 0);
    V3 qv = v3(nq[0], nq[1], nq[2]);
    V3 normal, tangent, view = v3(viewa[0], viewa[1], viewa[2]);
    float diffuse_sum[4] = {0, 0, 0, 1}, specular_sum[4] = {0, 0, 0, 1};
    uint32_t li;
    int i;

    if (config0 & 1u) {
        uint32_t sel = (config0 >> 24) & 3u;
        for (i = 0; i < 4; i++) shadow[i] = (float)tex[sel][i] / 255.0f;
        if (config0 & (1u << 18))
            for (i = 0; i < 4; i++) shadow[i] = 1.0f - shadow[i];
    } else {
        for (i = 0; i < 4; i++) shadow[i] = 1.0f;
    }

    if (bump_mode != 0u) {
        uint32_t sel = (config0 >> 22) & 3u;
        V3 p = v3((float)tex[sel][0] / 127.5f - 1.0f, (float)tex[sel][1] / 127.5f - 1.0f,
                  (float)tex[sel][2] / 127.5f - 1.0f);
        if (bump_mode == 1u) {
            if (!(config0 & (1u << 30))) {
                float z2 = 1 - (p.x * p.x + p.y * p.y);
                p.z = sqrtf(z2 > 0.0f ? z2 : 0.0f);
            }
            surface_normal = p;
            surface_tangent = v3(1, 0, 0);
        } else if (bump_mode == 2u) {
            surface_normal = v3(0, 0, 1);
            surface_tangent = p;
        }
    } else {
        surface_normal = v3(0, 0, 1);
        surface_tangent = v3(1, 0, 0);
    }
    normal = quat_rotate(qv, nq[3], surface_normal);
    tangent = quat_rotate(qv, nq[3], surface_tangent);

    for (li = 0; li <= max_light; li++) {
        uint32_t num = (R[0x1D9] >> (li * 4u)) & 7u;
        uint32_t lb = 0x140u + num * 0x10u;
        uint32_t lcfg = R[lb + 9];
        V3 position = v3(ref_float_raw(R[lb + 4] & 0xFFFFu, 10, 5),
                         ref_float_raw((R[lb + 4] >> 16) & 0xFFFFu, 10, 5),
                         ref_float_raw(R[lb + 5] & 0xFFFFu, 10, 5));
        V3 refl, light_vector, norm_view, half_vector, specular_0, specular_1;
        V3 sp, ss, diffuse, specular;
        float length, dist_atten = 1.0f, spot_atten = 1.0f, d0 = 1.0f, d1 = 1.0f;
        float dot_product, clamp_hl = 1.0f;
        RefCtx c;

        light_vector = (lcfg & 1u) ? position : v3_add(position, view);
        length = v3_len(light_vector);
        light_vector = v3_div(light_vector, length);
        norm_view = v3_normalized(view);
        half_vector = v3_add(norm_view, light_vector);

        if (!(config1 & (1u << (24u + num)))) {
            float scale = ref_float_raw(R[lb + 0xB] & 0xFFFFFu, 12, 7);
            float bias = ref_float_raw(R[lb + 0xA] & 0xFFFFFu, 12, 7);
            float s = scale * length + bias;
            float f;
            uint8_t idx;
            s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
            f = floorf(s * 256.0f);
            idx = (uint8_t)(f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
            dist_atten = ref_lookup(st, 16u + num, idx, s * 256 - idx);
        }

        c.st = st; c.normal = normal; c.tangent = tangent; c.half_vector = half_vector;
        c.norm_view = norm_view; c.light_vector = light_vector; c.lbase = lb;
        c.config = config; c.light_cfg = lcfg;

        if (!(config1 & (1u << (8u + num))) && ref_supported(config, 8))
            spot_atten = ref_get_lut_value(&c, (lut_input >> 8) & 7u,
                                           ((abs_in >> 9) & 1u) == 0, (lut_scale >> 8) & 7u,
                                           8u + num);
        if (!(config1 & (1u << 16)) && ref_supported(config, 0))
            d0 = ref_get_lut_value(&c, lut_input & 7u, ((abs_in >> 1) & 1u) == 0,
                                   lut_scale & 7u, 0);
        specular_0 = v3_mul(light_color(R[lb + 0]), d0);

        if (!(config1 & (1u << 20)) && ref_supported(config, 6))
            refl.x = ref_get_lut_value(&c, (lut_input >> 24) & 7u, ((abs_in >> 25) & 1u) == 0,
                                       (lut_scale >> 24) & 7u, 6);
        else
            refl.x = 1.0f;
        if (!(config1 & (1u << 21)) && ref_supported(config, 5))
            refl.y = ref_get_lut_value(&c, (lut_input >> 20) & 7u, ((abs_in >> 21) & 1u) == 0,
                                       (lut_scale >> 20) & 7u, 5);
        else
            refl.y = refl.x;
        if (!(config1 & (1u << 22)) && ref_supported(config, 4))
            refl.z = ref_get_lut_value(&c, (lut_input >> 16) & 7u, ((abs_in >> 17) & 1u) == 0,
                                       (lut_scale >> 16) & 7u, 4);
        else
            refl.z = refl.x;

        if (!(config1 & (1u << 17)) && ref_supported(config, 1))
            d1 = ref_get_lut_value(&c, (lut_input >> 4) & 7u, ((abs_in >> 5) & 1u) == 0,
                                   (lut_scale >> 4) & 7u, 1);
        /* d1_lut_value * refl_value * specular_1 (links nach rechts) */
        specular_1 = v3_mulv(v3_mul(refl, d1), light_color(R[lb + 1]));
        /* Hinweis: Azahar rechnet `d1 * refl` (Skalar*Vektor) und dann
           komponentenweise mit specular_1 - identisch zu oben. */

        if (li == max_light && !(config1 & (1u << 19)) && ref_supported(config, 3)) {
            float v = ref_get_lut_value(&c, (lut_input >> 12) & 7u, ((abs_in >> 13) & 1u) == 0,
                                        (lut_scale >> 12) & 7u, 3);
            if (config0 & (1u << 2)) diffuse_sum[3] = v;
            if (config0 & (1u << 3)) specular_sum[3] = v;
        }

        dot_product = v3_dot(light_vector, normal);
        if (lcfg & 2u) dot_product = fabsf(dot_product);
        else dot_product = dot_product > 0.0f ? dot_product : 0.0f;

        if (config0 & (1u << 27)) clamp_hl = dot_product == 0.0f ? 0.0f : 1.0f;

        if (lcfg & 0xCu) {
            float g = v3_len2(half_vector);
            g = g == 0.0f ? 0.0f : (dot_product / g < 1.0f ? dot_product / g : 1.0f);
            if (lcfg & 4u) specular_0 = v3_mul(specular_0, g);
            if (lcfg & 8u) specular_1 = v3_mul(specular_1, g);
        }

        sp = ((config0 >> 16) & 1u) && !(config1 & (1u << num))
                 ? v3(shadow[0], shadow[1], shadow[2]) : v3(1, 1, 1);
        ss = ((config0 >> 17) & 1u) && !(config1 & (1u << num))
                 ? v3(shadow[0], shadow[1], shadow[2]) : v3(1, 1, 1);

        diffuse = v3_mul(v3_mul(v3_add(v3_mulv(v3_mul(light_color(R[lb + 2]), dot_product), sp),
                                       light_color(R[lb + 3])),
                                dist_atten),
                         spot_atten);
        specular = v3_mulv(v3_mul(v3_mul(v3_mul(v3_add(specular_0, specular_1), clamp_hl),
                                         dist_atten),
                                  spot_atten),
                           ss);
        diffuse_sum[0] += diffuse.x; diffuse_sum[1] += diffuse.y; diffuse_sum[2] += diffuse.z;
        specular_sum[0] += specular.x; specular_sum[1] += specular.y;
        specular_sum[2] += specular.z;
    }

    if (config0 & (1u << 19)) {
        if (config0 & (1u << 2)) diffuse_sum[3] *= shadow[3];
        if (config0 & (1u << 3)) specular_sum[3] *= shadow[3];
    }
    {
        V3 g = light_color(R[0x1C0]);
        diffuse_sum[0] += g.x; diffuse_sum[1] += g.y; diffuse_sum[2] += g.z;
    }
    for (i = 0; i < 4; i++) {
        outp[i] = clamp_u8(diffuse_sum[i]);
        outs[i] = clamp_u8(specular_sum[i]);
    }
}

/* ============ Teil A: Zufallsvergleich ============ */

static uint32_t ref_regs[PICA_NREGS];
static uint32_t ref_lut[24][256];

static uint32_t rnd_f16(float lo, float hi)
{
    /* Zufaelliger, endlicher f16-Rohwert im Bereich (Exponent < 31). */
    float v = rndf(lo, hi);
    int e;
    float m;
    uint32_t sign = 0;
    if (v == 0.0f) return 0;
    if (v < 0.0f) { sign = 0x8000u; v = -v; }
    m = frexpf(v, &e);
    if (e - 1 + 15 <= 0) return sign;
    return sign | ((uint32_t)(e - 1 + 15) << 10) | ((uint32_t)((m * 2.0f - 1.0f) * 1024.0f) & 0x3FFu);
}

static uint32_t rnd_f20(float lo, float hi)
{
    float v = rndf(lo, hi);
    int e;
    float m;
    uint32_t sign = 0;
    if (v == 0.0f) return 0;
    if (v < 0.0f) { sign = 0x80000u; v = -v; }
    m = frexpf(v, &e);
    return sign | ((uint32_t)(e - 1 + 63) << 12) | ((uint32_t)((m * 2.0f - 1.0f) * 4096.0f) & 0xFFFu);
}

static void set_reg(uint32_t id, uint32_t v)
{
    p_regs[id] = v;
    ref_regs[id] = v;
}

static uint32_t rnd_color(void)
{
    /* 10-Bit-Felder, ueberwiegend 0..255, gelegentlich > 255 */
    uint32_t lim = (rnd() & 7u) == 0 ? 1023u : 160u;
    return ((rnd() % (lim + 1u)) << 20) | ((rnd() % (lim + 1u)) << 10) | (rnd() % (lim + 1u));
}

static uint32_t rnd_input(void) { return rnd() % 6u; }          /* NH..CP */
static uint32_t rnd_scale(void)
{
    static const uint32_t s[6] = {0, 1, 2, 3, 6, 7};
    return s[rnd() % 6u];
}

static void random_config(void)
{
    static const uint32_t cfgs[8] = {0, 1, 2, 3, 4, 5, 6, 8};
    uint32_t i, t, v;
    for (i = 0; i < 8; i++) {
        uint32_t lb = 0x140u + i * 0x10u;
        set_reg(lb + 0, rnd_color());
        set_reg(lb + 1, rnd_color());
        set_reg(lb + 2, rnd_color());
        set_reg(lb + 3, (rnd() & 1u) ? rnd_color() & 0x3FCFF3Fu : 0u);
        set_reg(lb + 4, rnd_f16(-8.0f, 8.0f) | (rnd_f16(-8.0f, 8.0f) << 16));
        set_reg(lb + 5, rnd_f16(-8.0f, 8.0f));
        set_reg(lb + 6, (rnd() & 0x1FFFu) | ((rnd() & 0x1FFFu) << 16));
        set_reg(lb + 7, rnd() & 0x1FFFu);
        set_reg(lb + 9, rnd() & 0xFu);
        set_reg(lb + 0xA, rnd_f20(-1.0f, 1.0f));
        set_reg(lb + 0xB, rnd_f20(-1.0f, 1.0f));
    }
    set_reg(0x1C0, rnd_color() & 0x3F0FC3Fu);
    set_reg(0x1C2, rnd() % 8u);
    v = 0;
    for (i = 0; i < 8; i++) v |= (rnd() & 7u) << (i * 4u);
    set_reg(0x1D9, v);
    {
        uint32_t bump = rnd() % 8u;                 /* 0..2 haeufig, 3 selten */
        bump = bump < 4u ? 0u : (bump < 6u ? 1u : (bump < 7u ? 2u : 3u));
        v = (rnd() & 1u) | (rnd() & 0xCu) | (cfgs[rnd() % 8u] << 4) |
            (rnd() & 0x000F0000u) | ((rnd() % 3u) << 22) | ((rnd() % 3u) << 24) |
            (rnd() & (1u << 27)) | (bump << 28) | (rnd() & (1u << 30));
    }
    set_reg(0x1C3, v);
    set_reg(0x1C4, rnd());
    v = 0;
    for (i = 0; i < 7; i++) v |= rnd_input() << (i * 4u);
    set_reg(0x1D1, v);
    set_reg(0x1D0, rnd() & 0x02222222u);
    v = 0;
    for (i = 0; i < 7; i++) v |= rnd_scale() << (i * 4u);
    set_reg(0x1D2, v);
    /* LUTs: ueber den Schreibpfad 0x1C5/0x1C8 von gpu.c */
    for (t = 0; t < 24; t++) {
        set_reg(0x1C5, t << 8);
        for (i = 0; i < 256; i++) {
            uint32_t e = rnd() & 0xFFFFFFu;
            ref_lut[t][i] = e;
            p_light_lut_write(e);
        }
    }
    p_light_draw_invalidate();
}

static int part_a(int cases)
{
    int n, bad = 0, pts = 0;
    RefState st;
    st.R = ref_regs;
    st.lut = (const uint32_t (*)[256])ref_lut;
    for (n = 0; n < cases; n++) {
        int k;
        random_config();
        for (k = 0; k < 16; k++) {
            float q[4], nq[4], view[3], len;
            uint8_t tex[4][4], rp[4], rs[4];
            PicaColor tc[4], op, os;
            int i, j, diff = 0;
            for (i = 0; i < 4; i++) q[i] = rndf(-1.0f, 1.0f);
            for (i = 0; i < 3; i++) view[i] = rndf(-6.0f, 6.0f);
            for (i = 0; i < 4; i++)
                for (j = 0; j < 4; j++) tc[i].c[j] = tex[i][j] = (uint8_t)rnd();
            /* sw_rasterizer.cpp: Quaternion<f32>{...}.Normalized() */
            len = sqrtf((q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) + q[3] * q[3]);
            for (i = 0; i < 4; i++) nq[i] = q[i] / len;
            ref_compute(&st, nq, view, tex, rp, rs);
            p_compute_fragments(tc, q, view, &op, &os);
            for (i = 0; i < 4; i++) {
                int d1 = abs((int)rp[i] - (int)op.c[i]);
                int d2 = abs((int)rs[i] - (int)os.c[i]);
                if (d1 > 1 || d2 > 1) diff = 1;
            }
            pts++;
            if (diff) {
                if (bad < 8)
                    printf("  DIFF Fall %d/%d: ref P(%d,%d,%d,%d) S(%d,%d,%d,%d) "
                           "gpu P(%d,%d,%d,%d) S(%d,%d,%d,%d) cfg0=%08X cfg1=%08X\n",
                           n, k, rp[0], rp[1], rp[2], rp[3], rs[0], rs[1], rs[2], rs[3],
                           op.c[0], op.c[1], op.c[2], op.c[3], os.c[0], os.c[1], os.c[2],
                           os.c[3], ref_regs[0x1C3], ref_regs[0x1C4]);
                bad++;
            }
        }
    }
    printf("%s Teil A: %d/%d Fragmente weichen um > 1 LSB ab (%d Konfigurationen)\n",
           bad ? "FEHL" : "OK  ", bad, pts, cases);
    return bad;
}

/* ============ Teil B: perspektivische Interpolation ============ */

#define LIST 0x00001000u
#define VTX  0x00021000u
#define FBA  0x00040000u
#define FBW  32
#define FBH  32
#define DEPA 0x00050000u

static uint32_t *emit(uint32_t *p, uint32_t id, uint32_t val)
{
    p[0] = val;
    p[1] = (id & 0xFFFFu) | (0xFu << 16);
    return p + 2;
}

static uint32_t f24raw(float v)
{
    int e;
    float m;
    uint32_t sign;
    if (v == 0.0f) return 0u;
    m = frexpf(v < 0.0f ? -v : v, &e);
    sign = (v < 0.0f) ? 0x800000u : 0u;
    return sign | ((uint32_t)(e - 1 + 63) << 16) |
           ((uint32_t)((m * 2.0f - 1.0f) * 65536.0f + 0.5f) & 0xFFFFu);
}

static uint32_t f16raw(float v)
{
    int e;
    float m;
    uint32_t sign = 0;
    if (v == 0.0f) return 0;
    if (v < 0.0f) { sign = 0x8000u; v = -v; }
    m = frexpf(v, &e);
    return sign | ((uint32_t)(e - 1 + 15) << 10) |
           ((uint32_t)((m * 2.0f - 1.0f) * 1024.0f + 0.5f) & 0x3FFu);
}

static uint32_t f20raw(float v)
{
    int e;
    float m;
    uint32_t sign = 0;
    if (v == 0.0f) return 0;
    if (v < 0.0f) { sign = 0x80000u; v = -v; }
    m = frexpf(v, &e);
    return sign | ((uint32_t)(e - 1 + 63) << 12) |
           ((uint32_t)((m * 2.0f - 1.0f) * 4096.0f + 0.5f) & 0xFFFu);
}

static uint32_t ins_mov(uint32_t src, uint32_t dst)
{
    return (0x13u << 26) | ((src & 0x7Fu) << 12) | ((dst & 0x1Fu) << 21);
}

/* Byte-Offset eines Pixels im gekachelten RGBA8-Farbziel. */
static uint32_t fb_off(int x, int y)
{
    static const uint32_t xl[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t yl[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
    int yp = (FBH - 1) - y;
    uint32_t i = xl[x % 8] + yl[yp % 8] + (uint32_t)(x & ~7) * 8u;
    return FBA + (i + (uint32_t)(yp & ~7) * (uint32_t)FBW) * 4u;
}

typedef struct { float pos[4], vw[4], quat[4]; } TVtx;

/* Ein Dreieck: attr0 float4 POSITION, attr1 float4 (tc0_w, view.xyz),
   attr2 float4 QUATERNION. TEV: PrimaryFragment. */
static uint32_t build_list(unsigned char *mem, const TVtx v[3], int mode)
{
    uint32_t *q = (uint32_t *)(mem + LIST);
    uint32_t i;
    for (i = 0; i < 3; i++) {
        float f[12];
        memcpy(f, v[i].pos, 16); memcpy(f + 4, v[i].vw, 16); memcpy(f + 8, v[i].quat, 16);
        memcpy(mem + VTX + i * 48u, f, sizeof(f));
    }
    q = emit(q, 0x200, VTX >> 3);
    q = emit(q, 0x201, 0xFFFu);                    /* attr0..2 float4 */
    q = emit(q, 0x202, 2u << 28);
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0x210u);
    q = emit(q, 0x205, (3u << 28) | (48u << 16));
    q = emit(q, 0x228, 3u);
    /* Vertexprogramm: o0=v0, o1=v1, o2=v2 */
    q = emit(q, 0x2B9, 2u);
    q = emit(q, 0x2BB, 0x210u);
    q = emit(q, 0x4F, 3u);
    q = emit(q, 0x50, 0x03020100u);                /* POSITION */
    q = emit(q, 0x51, 0x14131210u);                /* tc0_w, VIEW 18..20 */
    q = emit(q, 0x52, 0x07060504u);                /* QUATERNION */
    q = emit(q, 0x2BD, 7u);
    q = emit(q, 0x2CB, 0u);
    q = emit(q, 0x2CC, ins_mov(0, 0));
    q = emit(q, 0x2CC, ins_mov(1, 1));
    q = emit(q, 0x2CC, ins_mov(2, 2));
    q = emit(q, 0x2CC, 0x22u << 26);
    q = emit(q, 0x2D5, 0u);
    q = emit(q, 0x2D6, 0xFu | (1u << 9) | (2u << 7) | (3u << 5) |
                       (1u << 18) | (2u << 16) | (3u << 14));
    q = emit(q, 0x41, f24raw(FBW * 0.5f));
    q = emit(q, 0x43, f24raw(FBH * 0.5f));
    q = emit(q, 0x68, 0u);
    /* Farbziel RGBA8 */
    q = emit(q, 0x117, 0u);
    q = emit(q, 0x11D, FBA >> 3);
    q = emit(q, 0x11E, (uint32_t)FBW | ((uint32_t)(FBH - 1) << 12));
    q = emit(q, 0x107, 0x00000F00u);
    q = emit(q, 0x102, 3u);
    q = emit(q, 0x113, 1u);
    /* TEV 0: PrimaryFragment, Stufen 1..5 Previous */
    q = emit(q, 0xC0, 1u | (1u << 16));
    q = emit(q, 0xC1, 0u); q = emit(q, 0xC2, 0u); q = emit(q, 0xC3, 0u); q = emit(q, 0xC4, 0u);
    for (i = 1; i < 6; i++) {
        uint32_t b = (i < 4) ? (0xC0u + i * 8u) : (0xF0u + (i - 4u) * 8u);
        q = emit(q, b, 0x000F000Fu);
        q = emit(q, b + 1, 0u); q = emit(q, b + 2, 0u); q = emit(q, b + 3, 0u);
        q = emit(q, b + 4, 0u);
    }
    /* Lighting: 1 Licht (Slot 0 -> Licht 0), Config0, Spot/D0/D1/RR aus */
    q = emit(q, 0x1C6, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1C0, 0u);
    q = emit(q, 0x140, 0u);
    q = emit(q, 0x141, 0u);
    if (mode == 0) {
        /* Punktlicht im Ursprung, nur Ambient (weiss) * Distanz-LUT:
           Primary = LUT(|view| * 1/8) => misst die Laenge von view. */
        q = emit(q, 0x1C3, 0u);
        q = emit(q, 0x1C4, 0x00FF0000u | 0x0000FF00u);   /* LUTs aus, Spot aus, Dist an */
        q = emit(q, 0x142, 0u);
        q = emit(q, 0x143, (255u << 20) | (255u << 10) | 255u);
        q = emit(q, 0x144, 0u);
        q = emit(q, 0x145, 0u);
        q = emit(q, 0x149, 0u);
        q = emit(q, 0x14A, 0u);
        q = emit(q, 0x14B, f20raw(0.125f));
        q = emit(q, 0x1C5, 16u << 8);
        for (i = 0; i < 256; i++) {
            /* Wert i/255 mit Differenz zum Nachfolger (0.0.11) */
            uint32_t val = (i * 4095u + 127u) / 255u;
            q = emit(q, 0x1C8, val | (8u << 12));
        }
    } else { /* mode 1 und 2 */
        /* Direktionales Licht +x, Diffus weiss: Primary = max(N.x, 0),
           N = QuaternionRotate(normquat, (0,0,1)) => misst die Quaternion. */
        q = emit(q, 0x1C3, 0u);
        q = emit(q, 0x1C4, 0xFFFF0000u | 0x0000FF00u);   /* Dist + Spot + LUTs aus */
        q = emit(q, 0x142, (255u << 20) | (255u << 10) | 255u);
        q = emit(q, 0x143, 0u);
        q = emit(q, 0x144, f16raw(1.0f));
        q = emit(q, 0x145, 0u);
        q = emit(q, 0x149, 1u);
    }
    (void)f16raw;
    if (mode == 2) {
        /* Teil C: D16-Tiefenziel, LessThanOrEqual + Depth-Write,
           depth = -z/w (0x4D = -1, 0x4E = 0). */
        q = emit(q, 0x4D, f24raw(-1.0f));
        q = emit(q, 0x4E, 0u);
        q = emit(q, 0x6D, 1u);                      /* z/w, kein W-Buffer */
        q = emit(q, 0x116, 0u);                     /* D16 */
        q = emit(q, 0x11C, DEPA >> 3);
        q = emit(q, 0x114, 3u);
        q = emit(q, 0x115, 3u);
        q = emit(q, 0x107, 0x00001F51u);            /* Test an, LE, Farbe+Tiefe schreiben */
    }
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (mem + LIST));
}

/* Erwartung nach Azahar: Bildschirmposition, baryzentrische Gewichte in
   Bildschirmkoordinaten, perspektivisch korrigiert ueber 1/w. */
static int part_b_case(ArmCpu *c, unsigned char *mem, const TVtx v[3], int mode,
                       const char *name)
{
    float sx[3], sy[3], iw[3];
    int x, y, bad = 0, tested = 0, i;
    uint32_t size = build_list(mem, v, mode);
    memset(mem + FBA, 0, FBW * FBH * 4);
    gpu_pica_reset();
    gpu_run_pica_list(c, LIST, size);
    for (i = 0; i < 3; i++) {
        iw[i] = 1.0f / v[i].pos[3];
        sx[i] = (v[i].pos[0] * iw[i] + 1.0f) * FBW * 0.5f;
        sy[i] = (v[i].pos[1] * iw[i] + 1.0f) * FBH * 0.5f;
    }
    for (y = 0; y < FBH; y++) {
        for (x = 0; x < FBW; x++) {
            float px = x + 0.5f, py = y + 0.5f, b[3], area, s, view[3], qq[4], exp;
            uint8_t got;
            area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            b[0] = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
            b[1] = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
            b[2] = 1.0f - b[0] - b[1];
            /* nur klar innere Pixel (Kantenregeln sind nicht Gegenstand) */
            if (b[0] < 0.05f || b[1] < 0.05f || b[2] < 0.05f) continue;
            s = b[0] * iw[0] + b[1] * iw[1] + b[2] * iw[2];
            for (i = 0; i < 3; i++)
                view[i] = (b[0] * v[0].vw[1 + i] * iw[0] + b[1] * v[1].vw[1 + i] * iw[1] +
                           b[2] * v[2].vw[1 + i] * iw[2]) / s;
            for (i = 0; i < 4; i++)
                qq[i] = (b[0] * v[0].quat[i] * iw[0] + b[1] * v[1].quat[i] * iw[1] +
                         b[2] * v[2].quat[i] * iw[2]) / s;
            if (mode == 0) {
                float l = sqrtf(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
                float sl = l * 0.125f;
                if (sl > 1.0f) sl = 1.0f;
                exp = sl;                           /* LUT ~ linear */
            } else {
                float ql = sqrtf(qq[0] * qq[0] + qq[1] * qq[1] + qq[2] * qq[2] + qq[3] * qq[3]);
                V3 n;
                for (i = 0; i < 4; i++) qq[i] /= ql;
                n = quat_rotate(v3(qq[0], qq[1], qq[2]), qq[3], v3(0, 0, 1));
                exp = n.x > 0.0f ? n.x : 0.0f;
                if (exp > 1.0f) exp = 1.0f;
            }
            got = mem[fb_off(x, y) + 3];            /* R */
            tested++;
            if (abs((int)got - (int)(exp * 255.0f)) > 3) {
                if (bad < 3)
                    printf("  DIFF %s Pixel (%d,%d): erwartet %d, gpu %d\n", name, x, y,
                           (int)(exp * 255.0f), got);
                bad++;
            }
        }
    }
    printf("%s Teil B %s: %d/%d Pixel weichen ab\n", (bad || !tested) ? "FEHL" : "OK  ",
           name, bad, tested);
    return (bad || !tested) ? 1 : 0;
}

static int part_b(ArmCpu *c, unsigned char *mem)
{
    /* Bildschirmkoordinaten auf dem 1/16-Raster: x/w*16+16 ganzzahlig. */
    TVtx v[3];
    int r = 0;
    memset(v, 0, sizeof(v));
    /* w = 1, 4, 2; Position = ndc * w; z = -0.5*w (innerhalb [-w, 0]) */
    v[0].pos[0] = -0.875f * 1.0f; v[0].pos[1] = -0.875f * 1.0f; v[0].pos[3] = 1.0f;
    v[1].pos[0] = 0.875f * 4.0f; v[1].pos[1] = -0.75f * 4.0f; v[1].pos[3] = 4.0f;
    v[2].pos[0] = 0.0f * 2.0f; v[2].pos[1] = 0.875f * 2.0f; v[2].pos[3] = 2.0f;
    v[0].pos[2] = -0.5f; v[1].pos[2] = -2.0f; v[2].pos[2] = -1.0f;
    /* View: Laengen 1, 7, 3 */
    v[0].vw[0] = 1.0f; v[0].vw[3] = 1.0f;
    v[1].vw[0] = 1.0f; v[1].vw[3] = 7.0f;
    v[2].vw[0] = 1.0f; v[2].vw[1] = 3.0f;
    /* Quaternionen: Drehungen um y (N.x = sin(theta)) */
    {
        const float th[3] = {0.1f, 1.4f, 0.7f};
        int i;
        for (i = 0; i < 3; i++) {
            v[i].quat[1] = sinf(th[i] * 0.5f);
            v[i].quat[3] = cosf(th[i] * 0.5f);
        }
    }
    r += part_b_case(c, mem, v, 0, "View (Distanz-LUT)");
    r += part_b_case(c, mem, v, 1, "Quaternion (Diffus)");
    return r;
}

/* Teil C: D16-Tiefenpuffer (Azahar sw_framebuffer.cpp SetDepth/EncodeD16
   schreibt 2 Bytes). Dreieck 1 nah (depth 0.25, N.x = 1 => R 255), danach
   Dreieck 2 fern (depth 0.75, N.x = 0 => R 0) mit LessThanOrEqual: das
   ferne Dreieck muss ueberall verworfen werden. Der Tiefenpuffer ist
   vorher auf 0xFFFF geloescht. */
static int part_c(ArmCpu *c, unsigned char *mem)
{
    TVtx v[3];
    int k, x, y, bad = 0, tested = 0, pass;
    memset(mem + DEPA, 0xFF, FBW * FBH * 2);
    memset(mem + FBA, 0, FBW * FBH * 4);
    gpu_pica_reset();
    for (pass = 0; pass < 2; pass++) {
        float th = pass == 0 ? 1.5707964f : 0.0f;
        float zz = pass == 0 ? -0.25f : -0.75f;
        uint32_t size;
        memset(v, 0, sizeof(v));
        v[0].pos[0] = -1.0f; v[0].pos[1] = -1.0f;
        v[1].pos[0] = 1.0f;  v[1].pos[1] = -1.0f;
        v[2].pos[0] = -1.0f; v[2].pos[1] = 1.0f;
        for (k = 0; k < 3; k++) {
            v[k].pos[2] = zz; v[k].pos[3] = 1.0f;
            v[k].vw[3] = 1.0f;
            v[k].quat[1] = sinf(th * 0.5f);
            v[k].quat[3] = cosf(th * 0.5f);
        }
        size = build_list(mem, v, 2);
        gpu_run_pica_list(c, LIST, size);
    }
    for (y = 2; y < FBH / 2; y++)
        for (x = 2; x < FBW / 2; x++) {
            tested++;
            if (mem[fb_off(x, y) + 3] < 250) bad++;
        }
    printf("%s Teil C D16-Tiefe: %d/%d Pixel vom fernen Dreieck ueberschrieben\n",
           bad ? "FEHL" : "OK  ", bad, tested);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    static unsigned char mem[0x100000];
    ArmCpu c;
    int cases = 4000;
    if (argc > 1) cases = atoi(argv[1]);
    memset(&c, 0, sizeof(c));
    arm_cpu_init(&c, mem, sizeof(mem));
    gpu_pica_set_raster_threads(1);

    gpu_pica_reset();
    if (part_a(cases)) fails++;
    gpu_pica_reset();
    fails += part_b(&c, mem);
    fails += part_c(&c, mem);

    printf(fails ? "FEHLGESCHLAGEN (%d)\n" : "ALLE OK\n", fails);
    return fails ? 1 : 0;
}
