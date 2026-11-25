/* D3D11-Rasterizer fuer die PICA-Fragment-Pipeline.
 *
 * Nur Rasterisierung + Fragment-Pipeline liegen auf der Host-GPU; Vertex- und
 * Geometry-Shader bleiben der CPU-Interpreter in runtime/gpu/gpu.c. Das Modul
 * ist eine schmale Schnittstelle (hw_d3d11.h) und wird ausschliesslich ueber
 * `--renderer hw` aktiviert. Ohne diesen Schalter wird kein Device erzeugt.
 *
 * Umfang:
 *   - Clip-Space-Dreiecke mit POSITION + PrimaryColor + TEXCOORD0..2
 *   - Viewport, Scissor (Include), Tiefentest/-schreiben, Alpha-Test
 *   - Farbziele RGBA8/RGB8/RGB5A1/RGB565/RGBA4 in PICA-Kachelung
 *   - Tiefenziele D16/D24/D24S8 in PICA-Kachelung
 *   - Texturen (Einheit 0..2, Typ 2D/Disabled): alle PICA-Formate werden mit
 *     dem Software-Dekoder (gpu_pica_decode_texel, inkl. ETC1/ETC1A4) nach
 *     RGBA8 dekodiert und hochgeladen. Cache je (Adresse, Format, Groesse);
 *     bei neuer Kommandoliste bzw. Kopie (gpu_pica_tex_generation) wird der
 *     Quellbereich konservativ per FNV-1a-Hash verglichen.
 *   - Wrap/Border wie p_wrap_coord, Nearest und Linear (PICA-Filterbits).
 *   - TEV: ein Uber-Pixel-Shader interpretiert die 6 Stufen aus einem
 *     Constant Buffer (Quellen, Modifier, Kombinierer inkl. Dot3, Skalierung,
 *     Combiner-Buffer) mit derselben Integer-Semantik wie p_tev.
 * Noch nicht enthalten: Fragment-Lighting, Fog, Blending/LogicOps, W-Buffer,
 * Stencil, prozedurale Textur, Cube-/Shadow-/Projection-Texturen. gpu.c nutzt
 * den Hardware-Pfad nur, wenn der Draw-Zustand dazu passt, sonst bleibt der
 * Software-Pfad aktiv.
 *
 * Tiefeninitialisierung: Das D3D-Tiefenziel wird beim Binden auf 1.0 (fern)
 * gesetzt; der Software-Pfad erhaelt dagegen vorhandene Gastbytes. Fuer
 * Vergleiche muss der Gast-Tiefenpuffer daher mit 0xFF vorbelegt sein (wie
 * im Selbsttest). Das ist eine dokumentierte Grenze des Grundgeruests.
 */
#define COBJMACROS
#include "hw_d3d11.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11InputLayout *g_layout;
static ID3D11Buffer *g_cb;
static ID3D11RasterizerState *g_rs;
static ID3D11DepthStencilState *g_dss;
static ID3D11BlendState *g_bs;

static ID3D11Texture2D *g_color_tex;
static ID3D11RenderTargetView *g_color_rtv;
static ID3D11Texture2D *g_color_stage;
static ID3D11Texture2D *g_depth_dsv_tex;
static ID3D11DepthStencilView *g_dsv;
static ID3D11Texture2D *g_depth_rt;
static ID3D11RenderTargetView *g_depth_rtv;
static ID3D11Texture2D *g_depth_stage;

static int g_ready;
static int g_warp;
static int g_target_w, g_target_h;
static HwDrawState g_state;
static int g_have_state;
static int g_guest_dirty = 1;
static unsigned long long g_draws, g_flushes;
static unsigned long long g_tex_uploads;
static uint32_t g_dss_key = 0xFFFFFFFFu;

/* ---- Texturcache ------------------------------------------------------- */
typedef struct {
    uint32_t offset, bytes, fmt, w, h;
    uint64_t hash;
    int valid;                  /* hash passt zum hochgeladenen Bild */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} HwTexSlot;

static HwTexSlot g_texslot[3];
static ID3D11ShaderResourceView *g_tex_dummy;
static uint32_t g_tex_gen_seen = 0xFFFFFFFFu;
static int g_tex_force = 1;

const char *hw_driver_name(void) { return g_warp ? "warp" : "hardware"; }
void hw_stats(unsigned long long *draws, unsigned long long *flushes) {
    if (draws) *draws = g_draws;
    if (flushes) *flushes = g_flushes;
}
unsigned long long hw_texture_uploads(void) { return g_tex_uploads; }
int hw_available(void) { return g_ready; }
void hw_invalidate_guest(void) { g_guest_dirty = 1; }
void hw_invalidate_textures(void) { g_tex_force = 1; g_tex_gen_seen = 0xFFFFFFFFu; }

/* ---- HLSL --------------------------------------------------------------
 * pica_cmp/PSOut wie im Grundgeruest; Textur- und TEV-Semantik folgt
 * runtime/gpu/gpu.c (p_wrap_coord, p_sample_tex, p_tev). */
static const char *k_hlsl =
    "cbuffer PicaCB : register(b0) {\n"
    "  float4 k_const;\n"
    "  float4 k_param;\n"
    "  float4 k_param2;\n"
    "  uint4  k_src[6];\n"
    "  uint4  k_mod[6];\n"
    "  uint4  k_ops[6];\n"
    "  uint4  k_cst[6];\n"
    "  uint4  k_scl[6];\n"
    "  uint4  k_buf;\n"
    "  uint4  k_tex0[3];\n"
    "  uint4  k_tex1[3];\n"
    "  uint4  k_tex2[3];\n"
    "  uint4  k_uint;\n"
    "};\n"
    "Texture2D tex0 : register(t0);\n"
    "Texture2D tex1 : register(t1);\n"
    "Texture2D tex2 : register(t2);\n"
    "struct VSIn { float4 pos : POSITION; float4 col : COLOR0;\n"
    "              float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float2 uv2 : TEXCOORD2; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "               float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float2 uv2 : TEXCOORD2; };\n"
    "struct PSOut { float4 col : SV_Target0; float4 dep : SV_Target1; float d : SV_Depth; };\n"
    "VSOut VSMain(VSIn i) {\n"
    "  VSOut o;\n"
    "  o.pos = float4(i.pos.x, -i.pos.y, -i.pos.z, i.pos.w);\n"
    "  o.col = i.col;\n"
    "  o.uv0 = i.uv0; o.uv1 = i.uv1; o.uv2 = i.uv2;\n"
    "  return o;\n"
    "}\n"
    "bool pica_cmp(uint fn, uint a, uint b) {\n"
    "  if (fn == 0) return false;\n"
    "  if (fn == 1) return true;\n"
    "  if (fn == 2) return a == b;\n"
    "  if (fn == 3) return a != b;\n"
    "  if (fn == 4) return a < b;\n"
    "  if (fn == 5) return a <= b;\n"
    "  if (fn == 6) return a > b;\n"
    "  return a >= b;\n"
    "}\n"
    "int wrap_coord(uint mode, int v, int size) {\n"
    "  if (mode == 4) {\n"
    "    if (v < 0) v = (int)((uint)v % (uint)size);\n"
    "    if (v < 0) v = 0;\n"
    "    if (v > size - 1) v = size - 1;\n"
    "    return v;\n"
    "  }\n"
    "  if (mode == 0) {\n"
    "    if (v < 0) v = 0;\n"
    "    if (v > size - 1) v = size - 1;\n"
    "    return v;\n"
    "  }\n"
    "  if (mode == 1) return v;\n"
    "  if (mode == 3) {\n"
    "    uint c = (uint)v % (uint)(2 * size);\n"
    "    if (c >= (uint)size) c = (uint)(2 * size) - 1u - c;\n"
    "    return (int)c;\n"
    "  }\n"
    "  return (int)((uint)v % (uint)size);\n"
    "}\n"
    "uint4 fetch_tex(uint unit, int x, int y) {\n"
    "  float4 c;\n"
    "  int3 p = int3(x, y, 0);\n"
    "  if (unit == 0) c = tex0.Load(p);\n"
    "  else if (unit == 1) c = tex1.Load(p);\n"
    "  else c = tex2.Load(p);\n"
    "  return (uint4)round(c * 255.0);\n"
    "}\n"
    "uint4 sample_unit(uint unit, float2 uv) {\n"
    "  uint4 c0 = k_tex0[unit];\n"
    "  uint4 c1 = k_tex1[unit];\n"
    "  uint4 c2 = k_tex2[unit];\n"
    "  if (c1.y == 0 || c1.w == 5) return uint4(0, 0, 0, 0);\n"
    "  if (c2.x == 0) return uint4(0, 0, 0, 255);\n"
    "  int w = (int)c0.x, h = (int)c0.y;\n"
    "  if (w <= 0 || h <= 0) return uint4(0, 0, 0, 0);\n"
    "  uint border = c1.x;\n"
    "  uint ws = c0.z, wt = c0.w;\n"
    "  int s = (int)trunc(uv.x * (float)w);\n"
    "  int t = (int)trunc(uv.y * (float)h);\n"
    "  bool bs = (ws == 1) ? (s < 0 || s >= w) : (ws == 5 && s >= w);\n"
    "  bool bt = (wt == 1) ? (t < 0 || t >= h) : (wt == 5 && t >= h);\n"
    "  if (bs || bt)\n"
    "    return uint4(border & 0xFFu, (border >> 8) & 0xFFu,\n"
    "                 (border >> 16) & 0xFFu, (border >> 24) & 0xFFu);\n"
    "  s = wrap_coord(ws, s, w);\n"
    "  t = wrap_coord(wt, t, h);\n"
    "  if (s < 0 || s >= w || t < 0 || t >= h) return uint4(0, 0, 0, 0);\n"
    "  if (c1.z != 0) {\n"
    "    float fx = uv.x * (float)w - 0.5;\n"
    "    float fy = uv.y * (float)h - 0.5;\n"
    "    int x0 = (int)floor(fx), y0 = (int)floor(fy);\n"
    "    float ax = fx - floor(fx), ay = fy - floor(fy);\n"
    "    float4 acc = 0.0;\n"
    "    [unroll] for (int j = 0; j < 2; j++) {\n"
    "      [unroll] for (int i = 0; i < 2; i++) {\n"
    "        float wx = i ? ax : 1.0 - ax;\n"
    "        float wy = j ? ay : 1.0 - ay;\n"
    "        int sx = wrap_coord(ws, x0 + i, w);\n"
    "        int ty = wrap_coord(wt, y0 + j, h);\n"
    "        acc += (float4)fetch_tex(unit, sx, h - 1 - ty) * (wx * wy);\n"
    "      }\n"
    "    }\n"
    "    return (uint4)round(acc);\n"
    "  }\n"
    "  return fetch_tex(unit, s, h - 1 - t);\n"
    "}\n"
    "uint3 cmod(uint m, uint4 s) {\n"
    "  if (m == 0) return s.rgb;\n"
    "  if (m == 1) return 255u - s.rgb;\n"
    "  if (m == 2) return s.aaa;\n"
    "  if (m == 3) return 255u - s.aaa;\n"
    "  if (m == 4) return s.rrr;\n"
    "  if (m == 5) return 255u - s.rrr;\n"
    "  if (m == 8) return s.ggg;\n"
    "  if (m == 9) return 255u - s.ggg;\n"
    "  if (m == 12) return s.bbb;\n"
    "  if (m == 13) return 255u - s.bbb;\n"
    "  return uint3(0, 0, 0);\n"
    "}\n"
    "uint amod(uint m, uint4 s) {\n"
    "  if (m == 0) return s.a;\n"
    "  if (m == 1) return 255u - s.a;\n"
    "  if (m == 2) return s.r;\n"
    "  if (m == 3) return 255u - s.r;\n"
    "  if (m == 4) return s.g;\n"
    "  if (m == 5) return 255u - s.g;\n"
    "  if (m == 6) return s.b;\n"
    "  if (m == 7) return 255u - s.b;\n"
    "  return 0u;\n"
    "}\n"
    "uint3 ccomb(uint op, uint3 a, uint3 b, uint3 c) {\n"
    "  if (op == 0) return a;\n"
    "  if (op == 1) return a * b / 255u;\n"
    "  if (op == 2) return min(a + b, 255u);\n"
    "  if (op == 3) { int3 v = (int3)(a + b) - 128; return (uint3)clamp(v, 0, 255); }\n"
    "  if (op == 4) return (a * c + b * (255u - c)) / 255u;\n"
    "  if (op == 5) return (a > b) ? (a - b) : uint3(0, 0, 0);\n"
    "  if (op == 6 || op == 7) {\n"
    "    int3 d = ((int3)a * 2 - 255) * ((int3)b * 2 - 255);\n"
    "    int r = (d.x + 128) / 256 + (d.y + 128) / 256 + (d.z + 128) / 256;\n"
    "    r = clamp(r, 0, 255);\n"
    "    return uint3((uint)r, (uint)r, (uint)r);\n"
    "  }\n"
    "  if (op == 8) return min((a * b + 255u * c) / 255u, 255u);\n"
    "  if (op == 9) return min(a + b, 255u) * c / 255u;\n"
    "  return uint3(0, 0, 0);\n"
    "}\n"
    "uint acomb(uint op, uint a, uint b, uint c) {\n"
    "  if (op == 0) return a;\n"
    "  if (op == 1) return a * b / 255u;\n"
    "  if (op == 2) return min(a + b, 255u);\n"
    "  if (op == 3) { int v = (int)a + (int)b - 128; return (uint)clamp(v, 0, 255); }\n"
    "  if (op == 4) return (a * c + b * (255u - c)) / 255u;\n"
    "  if (op == 5) return (a > b) ? (a - b) : 0u;\n"
    "  if (op == 8) return min((a * b + 255u * c) / 255u, 255u);\n"
    "  if (op == 9) return min(a + b, 255u) * c / 255u;\n"
    "  return 0u;\n"
    "}\n"
    "uint4 tev_src(uint src, uint4 prim, uint4 pfc, uint4 sfc,\n"
    "              uint4 t0, uint4 t1, uint4 t2, uint4 prevbuf,\n"
    "              uint4 cst, uint4 prev) {\n"
    "  if (src == 0) return prim;\n"
    "  if (src == 1) return pfc;\n"
    "  if (src == 2) return sfc;\n"
    "  if (src == 3) return t0;\n"
    "  if (src == 4) return t1;\n"
    "  if (src == 5) return t2;\n"
    "  if (src == 13) return prevbuf;\n"
    "  if (src == 14) return cst;\n"
    "  if (src == 15) return prev;\n"
    "  return uint4(0, 0, 0, 0);\n"
    "}\n"
    "PSOut PSMain(VSOut i) {\n"
    "  PSOut o;\n"
    "  uint4 prim = (uint4)round(saturate(i.col) * 255.0);\n"
    "  uint4 zero = uint4(0, 0, 0, 0);\n"
    "  uint4 pfc = zero, sfc = zero;\n"
    "  uint4 t0 = sample_unit(0, i.uv0);\n"
    "  uint4 t1 = sample_unit(1, i.uv1);\n"
    "  uint4 t2 = sample_unit(2, i.uv2);\n"
    "  uint4 res = zero;\n"
    "  uint bcol = k_buf.y;\n"
    "  uint4 nxt = uint4(bcol & 0xFFu, (bcol >> 8) & 0xFFu, (bcol >> 16) & 0xFFu, (bcol >> 24) & 0xFFu);\n"
    "  uint4 buffer = zero;\n"
    "  uint buf_in = k_buf.x;\n"
    "  [unroll] for (uint s = 0; s < 6; s++) {\n"
    "    uint4 srcw = k_src[s];\n"
    "    uint4 modw = k_mod[s];\n"
    "    uint4 opsw = k_ops[s];\n"
    "    uint cst = k_cst[s].x;\n"
    "    uint4 cstc = uint4(cst & 0xFFu, (cst >> 8) & 0xFFu, (cst >> 16) & 0xFFu, (cst >> 24) & 0xFFu);\n"
    "    uint scl = k_scl[s].x;\n"
    "    uint cs1 = srcw.x & 15u, cs2 = (srcw.x >> 4) & 15u, cs3 = (srcw.x >> 8) & 15u;\n"
    "    uint as1 = (srcw.x >> 16) & 15u, as2 = (srcw.x >> 20) & 15u, as3 = (srcw.x >> 24) & 15u;\n"
    "    uint cm1 = modw.x & 15u, cm2 = (modw.x >> 4) & 15u, cm3 = (modw.x >> 8) & 15u;\n"
    "    uint am1 = (modw.x >> 12) & 7u, am2 = (modw.x >> 16) & 7u, am3 = (modw.x >> 20) & 7u;\n"
    "    uint cop = opsw.x & 15u, aop = (opsw.x >> 16) & 15u;\n"
    "    uint cshift = scl & 3u, ashift = (scl >> 16) & 3u;\n"
    "    uint cmul = (cshift < 3u) ? (1u << cshift) : 1u;\n"
    "    uint amul = (ashift < 3u) ? (1u << ashift) : 1u;\n"
    "    if (s != 0u && cop == 0u && aop == 0u && cm1 == 0u && am1 == 0u &&\n"
    "        cs1 == 15u && as1 == 15u && cshift == 0u && ashift == 0u &&\n"
    "        ((buf_in & ((1u << (8u + s)) | (1u << (12u + s)))) == 0u))\n"
    "      continue;\n"
    "    if (s == 0u) {\n"
    "      if (cs1 == 15u) cs1 = cs3;\n"
    "      if (cs2 == 15u) cs2 = cs3;\n"
    "    }\n"
    "    uint4 s1 = tev_src(cs1, prim, pfc, sfc, t0, t1, t2, buffer, cstc, res);\n"
    "    uint4 s2 = tev_src(cs2, prim, pfc, sfc, t0, t1, t2, buffer, cstc, res);\n"
    "    uint4 s3 = tev_src(cs3, prim, pfc, sfc, t0, t1, t2, buffer, cstc, res);\n"
    "    uint3 ci = ccomb(cop, cmod(cm1, s1), cmod(cm2, s2), cmod(cm3, s3));\n"
    "    uint ao;\n"
    "    if (cop == 7u) ao = ci.x;\n"
    "    else {\n"
    "      uint4 a1 = tev_src(as1, prim, pfc, sfc, t0, t1, t2, buffer, cstc, res);\n"
    "      uint4 a2 = tev_src(as2, prim, pfc, sfc, t0, t1, t2, buffer, cstc, res);\n"
    "      uint4 a3 = tev_src(as3, prim, pfc, sfc, t0, t1, t2, buffer, cstc, res);\n"
    "      ao = acomb(aop, amod(am1, a1), amod(am2, a2), amod(am3, a3));\n"
    "    }\n"
    "    res = uint4(min(ci * cmul, 255u), min(ao * amul, 255u));\n"
    "    buffer = nxt;\n"
    "    if (s < 4u) {\n"
    "      if (buf_in & (1u << (8u + s))) nxt.rgb = res.rgb;\n"
    "      if (buf_in & (1u << (12u + s))) nxt.a = res.a;\n"
    "    }\n"
    "  }\n"
    "  if (k_param.w > 0.5) {\n"
    "    uint r = (uint)floor(saturate(k_param2.y) * 255.0 + 0.5);\n"
    "    if (!pica_cmp((uint)k_param2.x, res.a, r)) discard;\n"
    "  }\n"
    "  float d = (1.0 - 2.0 * i.pos.z) * k_param.x + k_param.y;\n"
    "  d = saturate(d);\n"
    "  o.col = float4(res.rgb, res.a) / 255.0;\n"
    "  o.dep = float4(d, d, d, 1.0);\n"
    "  o.d = d;\n"
    "  return o;\n"
    "}\n";



static uint32_t p_morton_offset(uint32_t x, uint32_t y, uint32_t bpp) {
    static const uint32_t xlut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t ylut[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
    uint32_t i = xlut[x & 7u] + ylut[y & 7u];
    return (i + (x & ~7u) * 8u) * bpp;
}

static uint32_t fmt_bpp(uint32_t fmt) {
    switch (fmt) {
    case 1: return 3;
    case 2: case 3: case 4: return 2;
    default: return 4;
    }
}

static uint32_t depth_bpp(uint32_t fmt) {
    switch (fmt) {
    case 0: return 2;
    case 2: return 3;
    default: return 4;
    }
}

static uint32_t depth_bits(uint32_t fmt) { return (fmt == 0) ? 16u : 24u; }

static void color_decode(const unsigned char *m, uint32_t fmt,
                         unsigned char out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    switch (fmt) {
    case 1:
        out[2] = m[0]; out[1] = m[1]; out[0] = m[2]; out[3] = 255;
        break;
    case 2: {
        uint32_t v = m[0] | ((uint32_t)m[1] << 8);
        out[0] = (unsigned char)(((v >> 11) & 0x1Fu) * 255u / 31u);
        out[1] = (unsigned char)(((v >> 6) & 0x1Fu) * 255u / 31u);
        out[2] = (unsigned char)(((v >> 1) & 0x1Fu) * 255u / 31u);
        out[3] = (unsigned char)((v & 1u) ? 255u : 0u);
        break;
    }
    case 3: {
        uint32_t v = m[0] | ((uint32_t)m[1] << 8);
        out[0] = (unsigned char)(((v >> 11) & 0x1Fu) * 255u / 31u);
        out[1] = (unsigned char)(((v >> 5) & 0x3Fu) * 255u / 63u);
        out[2] = (unsigned char)((v & 0x1Fu) * 255u / 31u);
        out[3] = 255;
        break;
    }
    case 4: {
        uint32_t v = m[0] | ((uint32_t)m[1] << 8);
        out[0] = (unsigned char)(((v >> 12) & 0xFu) * 17u);
        out[1] = (unsigned char)(((v >> 8) & 0xFu) * 17u);
        out[2] = (unsigned char)(((v >> 4) & 0xFu) * 17u);
        out[3] = (unsigned char)((v & 0xFu) * 17u);
        break;
    }
    default:
        out[3] = m[0]; out[2] = m[1]; out[1] = m[2]; out[0] = m[3];
        break;
    }
}

static void color_encode(unsigned char *m, uint32_t fmt,
                         const unsigned char in[4]) {
    unsigned r = in[0], g = in[1], b = in[2], a = in[3];
    switch (fmt) {
    case 1:
        m[0] = (unsigned char)b; m[1] = (unsigned char)g; m[2] = (unsigned char)r;
        break;
    case 2: {
        uint32_t v = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7);
        m[0] = (unsigned char)(v & 0xFFu); m[1] = (unsigned char)(v >> 8);
        break;
    }
    case 3: {
        uint32_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        m[0] = (unsigned char)(v & 0xFFu); m[1] = (unsigned char)(v >> 8);
        break;
    }
    case 4: {
        uint32_t v = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
        m[0] = (unsigned char)(v & 0xFFu); m[1] = (unsigned char)(v >> 8);
        break;
    }
    default:
        m[0] = (unsigned char)a; m[1] = (unsigned char)b;
        m[2] = (unsigned char)g; m[3] = (unsigned char)r;
        break;
    }
}

static int same_target(const HwDrawState *a, const HwDrawState *b) {
    return a->color_mem == b->color_mem && a->color_size == b->color_size &&
           a->color_width == b->color_width && a->color_height == b->color_height &&
           a->color_format == b->color_format && a->depth_mem == b->depth_mem &&
           a->depth_size == b->depth_size && a->depth_format == b->depth_format;
}

static void release_targets(void) {
    if (g_color_stage) { ID3D11Texture2D_Release(g_color_stage); g_color_stage = NULL; }
    if (g_color_rtv) { ID3D11RenderTargetView_Release(g_color_rtv); g_color_rtv = NULL; }
    if (g_color_tex) { ID3D11Texture2D_Release(g_color_tex); g_color_tex = NULL; }
    if (g_depth_stage) { ID3D11Texture2D_Release(g_depth_stage); g_depth_stage = NULL; }
    if (g_depth_rtv) { ID3D11RenderTargetView_Release(g_depth_rtv); g_depth_rtv = NULL; }
    if (g_depth_rt) { ID3D11Texture2D_Release(g_depth_rt); g_depth_rt = NULL; }
    if (g_dsv) { ID3D11DepthStencilView_Release(g_dsv); g_dsv = NULL; }
    if (g_depth_dsv_tex) { ID3D11Texture2D_Release(g_depth_dsv_tex); g_depth_dsv_tex = NULL; }
    g_target_w = g_target_h = 0;
}

static int create_targets(const HwDrawState *st) {
    D3D11_TEXTURE2D_DESC td;
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd;
    int w = (int)st->color_width, h = (int)st->color_height;
    unsigned char *stage;
    uint32_t bpp = fmt_bpp(st->color_format);
    int x, y;

    release_targets();
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;

    memset(&td, 0, sizeof(td));
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_color_tex))) return 0;
    memset(&rd, 0, sizeof(rd));
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_color_tex,
                                                   &rd, &g_color_rtv))) return 0;

    /* Gastinhalt in eine lineare RGBA8-Flaeche dekodieren (PICA-Kachelung). */
    stage = (unsigned char *)calloc((size_t)w * h * 4u, 1);
    if (!stage) return 0;
    if (st->color_mem != NULL) {
        for (y = 0; y < h; y++) {
            uint32_t yp = (uint32_t)h - 1u - (uint32_t)y;
            unsigned char *row = stage + (size_t)y * (size_t)w * 4u;
            for (x = 0; x < w; x++) {
                uint32_t off = p_morton_offset((uint32_t)x, yp, bpp) +
                               (yp & ~7u) * (uint32_t)w * bpp;
                unsigned char px[4];
                if ((size_t)off + bpp > st->color_size) continue;
                color_decode(st->color_mem + off, st->color_format, px);
                memcpy(row + x * 4, px, 4);
            }
        }
    }
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_color_tex, 0, NULL,
                                          stage, (UINT)w * 4u, 0);
    free(stage);

    /* Tiefenziel D32_FLOAT (nur Test, nicht Rueckgabepfad) + Schatten-R32. */
    memset(&td, 0, sizeof(td));
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_depth_dsv_tex))) return 0;
    memset(&dd, 0, sizeof(dd));
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)g_depth_dsv_tex,
                                                    &dd, &g_dsv))) return 0;
    ID3D11DeviceContext_ClearDepthStencilView(g_ctx, g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_depth_rt))) return 0;
    memset(&rd, 0, sizeof(rd));
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_depth_rt,
                                                   &rd, &g_depth_rtv))) return 0;
    {
        float fval = 1.0f;
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_depth_rtv, &fval);
    }

    /* Staging-Texturen fuer den Rueckweg. */
    memset(&td, 0, sizeof(td));
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_color_stage))) return 0;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_depth_stage))) return 0;

    g_target_w = w;
    g_target_h = h;
    g_guest_dirty = 0;
    return 1;
}

static int init_pipeline(void) {
    D3D11_INPUT_ELEMENT_DESC elems[5];
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_BLEND_DESC bl;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    unsigned char white[4] = {255, 255, 255, 255};
    ID3D11Texture2D *dummy = NULL;
    HRESULT hr;

    hr = D3DCompile(k_hlsl, strlen(k_hlsl), "pica_hw", NULL, NULL, "VSMain",
                    "vs_5_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) {
        if (err) { fprintf(stderr, "[hw] VS-Fehler: %s\n", (char *)ID3D10Blob_GetBufferPointer(err)); }
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    hr = D3DCompile(k_hlsl, strlen(k_hlsl), "pica_hw", NULL, NULL, "PSMain",
                    "ps_5_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        if (err) { fprintf(stderr, "[hw] PS-Fehler: %s\n", (char *)ID3D10Blob_GetBufferPointer(err)); }
        if (err) ID3D10Blob_Release(err);
        ID3D10Blob_Release(vsb);
        return 0;
    }
    memset(elems, 0, sizeof(elems));
    elems[0].SemanticName = "POSITION"; elems[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    elems[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elems[1].SemanticName = "COLOR"; elems[1].SemanticIndex = 0;
    elems[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    elems[1].AlignedByteOffset = 16;
    elems[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elems[2].SemanticName = "TEXCOORD"; elems[2].SemanticIndex = 0;
    elems[2].Format = DXGI_FORMAT_R32G32_FLOAT;
    elems[2].AlignedByteOffset = 32;
    elems[3].SemanticName = "TEXCOORD"; elems[3].SemanticIndex = 1;
    elems[3].Format = DXGI_FORMAT_R32G32_FLOAT;
    elems[3].AlignedByteOffset = 40;
    elems[4].SemanticName = "TEXCOORD"; elems[4].SemanticIndex = 2;
    elems[4].Format = DXGI_FORMAT_R32G32_FLOAT;
    elems[4].AlignedByteOffset = 48;
    elems[2].InputSlotClass = elems[3].InputSlotClass = elems[4].InputSlotClass =
        D3D11_INPUT_PER_VERTEX_DATA;
    if (FAILED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb),
                                               ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs))) {
        ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb); return 0;
    }
    if (FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb),
                                              ID3D10Blob_GetBufferSize(psb), NULL, &g_ps))) {
        ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb); return 0;
    }
    if (FAILED(ID3D11Device_CreateInputLayout(g_dev, elems, 5,
                                              ID3D10Blob_GetBufferPointer(vsb),
                                              ID3D10Blob_GetBufferSize(vsb), &g_layout))) {
        ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb); return 0;
    }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    /* TEV-Constant-Buffer: 3 float4 + 5*6 uint4 + 1 + 3*3 + 1 = 176 Woerter. */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = 176u * 4u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb))) return 0;

    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs))) return 0;

    memset(&ds, 0, sizeof(ds));
    ds.DepthEnable = FALSE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &ds, &g_dss))) return 0;

    memset(&bl, 0, sizeof(bl));
    bl.RenderTarget[0].BlendEnable = FALSE;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bl.RenderTarget[1].BlendEnable = FALSE;
    bl.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bl, &g_bs))) return 0;

    /* 1x1-Platzhaltertextur, damit Load() auf unbenutzten Einheiten gueltig ist. */
    memset(&td, 0, sizeof(td));
    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = white;
    sd.SysMemPitch = 4;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &sd, &dummy))) return 0;
    memset(&sv, 0, sizeof(sv));
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)dummy,
                                                     &sv, &g_tex_dummy))) {
        ID3D11Texture2D_Release(dummy);
        return 0;
    }
    ID3D11Texture2D_Release(dummy);
    return 1;
}

int hw_init(const char *driver) {
    D3D11_FEATURE_DATA_THREADING dummy;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr;
    (void)dummy;
    if (g_ready) return 1;
    g_warp = (driver != NULL && strcmp(driver, "warp") == 0);
    hr = D3D11CreateDevice(NULL, g_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                           NULL, 0, NULL, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr) && !g_warp) {
        printf("[hw] Hardware-Treiber nicht verfuegbar (hr=%08lX), Fallback WARP\n",
               (unsigned long)hr);
        g_warp = 1;
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[hw] D3D11-Device fehlgeschlagen (hr=%08lX)\n", (unsigned long)hr);
        return 0;
    }
    if (!init_pipeline()) {
        hw_shutdown();
        return 0;
    }
    g_ready = 1;
    printf("[hw] D3D11 aktiv (%s, feature level 0x%X)\n", hw_driver_name(), (unsigned)fl);
    return 1;
}

void hw_shutdown(void) {
    int i;
    for (i = 0; i < 3; i++) {
        if (g_texslot[i].srv) { ID3D11ShaderResourceView_Release(g_texslot[i].srv); g_texslot[i].srv = NULL; }
        if (g_texslot[i].tex) { ID3D11Texture2D_Release(g_texslot[i].tex); g_texslot[i].tex = NULL; }
        memset(&g_texslot[i], 0, sizeof(g_texslot[i]));
    }
    if (g_tex_dummy) { ID3D11ShaderResourceView_Release(g_tex_dummy); g_tex_dummy = NULL; }
    release_targets();
    if (g_bs) { ID3D11BlendState_Release(g_bs); g_bs = NULL; }
    if (g_dss) { ID3D11DepthStencilState_Release(g_dss); g_dss = NULL; }
    if (g_rs) { ID3D11RasterizerState_Release(g_rs); g_rs = NULL; }
    if (g_cb) { ID3D11Buffer_Release(g_cb); g_cb = NULL; }
    if (g_layout) { ID3D11InputLayout_Release(g_layout); g_layout = NULL; }
    if (g_ps) { ID3D11PixelShader_Release(g_ps); g_ps = NULL; }
    if (g_vs) { ID3D11VertexShader_Release(g_vs); g_vs = NULL; }
    if (g_ctx) { ID3D11DeviceContext_Release(g_ctx); g_ctx = NULL; }
    if (g_dev) { ID3D11Device_Release(g_dev); g_dev = NULL; }
    g_ready = 0;
    g_have_state = 0;
    g_tex_gen_seen = 0xFFFFFFFFu;
    g_tex_force = 1;
}

/* ---- Constant Buffer ---------------------------------------------------
 * Layout exakt wie cbuffer PicaCB in k_hlsl (176 Woerter / 704 Byte). */
typedef union {
    float f[176];
    uint32_t u[176];
} CbData;

static void set_constants(const HwDrawState *st, const ID3D11ShaderResourceView *srv[3]) {
    D3D11_MAPPED_SUBRESOURCE map;
    CbData cb;
    int i;
    (void)srv;

    memset(&cb, 0, sizeof(cb));
    cb.f[4] = st->depth_range;
    cb.f[5] = st->depth_near;
    cb.f[6] = 0.0f;
    cb.f[7] = st->alpha_test ? 1.0f : 0.0f;
    cb.f[8] = (float)st->alpha_func;
    cb.f[9] = (float)st->alpha_ref / 255.0f;
    for (i = 0; i < 6; i++) {
        cb.u[12 + i * 4 + 0] = st->tev_src[i];
        cb.u[36 + i * 4 + 0] = st->tev_mod[i];
        cb.u[60 + i * 4 + 0] = st->tev_ops[i];
        cb.u[84 + i * 4 + 0] = st->tev_cst[i];
        cb.u[108 + i * 4 + 0] = st->tev_scl[i];
    }
    cb.u[132] = st->tev_buf_in;
    cb.u[133] = st->tev_buf_color;
    for (i = 0; i < 3; i++) {
        cb.u[136 + i * 4 + 0] = st->tex[i].width;
        cb.u[136 + i * 4 + 1] = st->tex[i].height;
        cb.u[136 + i * 4 + 2] = st->tex[i].wrap_s;
        cb.u[136 + i * 4 + 3] = st->tex[i].wrap_t;
        cb.u[148 + i * 4 + 0] = st->tex[i].border_rgba;
        cb.u[148 + i * 4 + 1] = st->tex[i].enabled ? 1u : 0u;
        cb.u[148 + i * 4 + 2] = st->tex[i].filter;
        cb.u[148 + i * 4 + 3] = st->tex[i].type;
        cb.u[160 + i * 4 + 0] = g_texslot[i].valid ? 1u : 0u;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return;
    memcpy(map.pData, &cb, sizeof(cb));
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
}

/* ---- Textur-Upload / Cache -------------------------------------------- */
static uint64_t fnv1a(const unsigned char *p, uint32_t n)
{
    uint64_t h = 1469598103934665603ULL;
    uint32_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Dekodiert das ganze Texturbild einmal nach RGBA8 und laedt es hoch.
 * Kachelreihenfolge wie p_lookup_texel: ti = (y/8)*(w/8) + x/8, innerhalb
 * der Kachel dekodiert gpu_pica_decode_texel (alle Formate inkl. ETC1). */
static int texslot_upload(int unit, const HwDrawState *st)
{
    HwTexSlot *s = &g_texslot[unit];
    const HwTexture *t = &st->tex[unit];
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    unsigned char *stage;
    uint32_t ts, tiles, tx, ty, k;

    ts = gpu_pica_tex_tile_size(t->format);
    tiles = (t->width / 8u) * (t->height / 8u);
    if (ts == 0u || tiles == 0u) return 0;
    if (t->width == 0u || t->height == 0u || (uint64_t)tiles * ts > t->bytes)
        return 0;

    if (!s->tex || s->w != t->width || s->h != t->height) {
        if (s->srv) { ID3D11ShaderResourceView_Release(s->srv); s->srv = NULL; }
        if (s->tex) { ID3D11Texture2D_Release(s->tex); s->tex = NULL; }
        memset(&td, 0, sizeof(td));
        td.Width = t->width;
        td.Height = t->height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &s->tex)))
            return 0;
        memset(&sv, 0, sizeof(sv));
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(
                g_dev, (ID3D11Resource *)s->tex, &sv, &s->srv)))
            return 0;
        s->w = t->width;
        s->h = t->height;
    }

    stage = (unsigned char *)malloc((size_t)t->width * t->height * 4u);
    if (!stage) return 0;
    for (ty = 0; ty < t->height / 8u; ty++) {
        for (tx = 0; tx < t->width / 8u; tx++) {
            uint32_t ti = ty * (t->width / 8u) + tx;
            const unsigned char *tile = st->guest_mem + t->offset + ti * ts;
            for (k = 0; k < 64u; k++) {
                uint32_t px_x = k & 7u, px_y = k >> 3;
                unsigned char px[4];
                gpu_pica_decode_texel(tile, t->format, px_x, px_y, px);
                memcpy(stage + (((ty * 8u + px_y) * t->width +
                                 tx * 8u + px_x) * 4u), px, 4);
            }
        }
    }
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)s->tex, 0,
                                          NULL, stage, t->width * 4u, 0);
    free(stage);
    s->offset = t->offset;
    s->fmt = t->format;
    s->bytes = t->bytes;
    g_tex_uploads++;
    return 1;
}

static void bind_textures(const HwDrawState *st)
{
    ID3D11ShaderResourceView *srv[3];
    int check = g_tex_force || st->tex_generation != g_tex_gen_seen;
    int i;

    for (i = 0; i < 3; i++) {
        HwTexSlot *s = &g_texslot[i];
        const HwTexture *t = &st->tex[i];
        if (t->type != 0u || !t->enabled || t->bytes == 0u ||
            st->guest_mem == NULL ||
            (uint64_t)t->offset + t->bytes > st->guest_size) {
            s->valid = 0;
        } else if (check) {
            uint64_t h = fnv1a(st->guest_mem + t->offset, t->bytes);
            if (!s->valid || s->hash != h || s->offset != t->offset ||
                s->fmt != t->format || s->w != t->width || s->h != t->height) {
                if (texslot_upload(i, st)) {
                    s->hash = h;
                    s->valid = 1;
                } else {
                    s->valid = 0;
                }
            }
        }
        srv[i] = s->valid ? s->srv : g_tex_dummy;
    }
    if (check) {
        g_tex_gen_seen = st->tex_generation;
        g_tex_force = 0;
    }
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 3, srv);
}

int hw_begin_draw(const HwDrawState *st) {
    D3D11_VIEWPORT vp;
    D3D11_RECT rc;
    D3D11_DEPTH_STENCIL_DESC ds;
    float w = 2.0f * st->viewport_half_w;
    float h = 2.0f * st->viewport_half_h;
    uint32_t dkey;

    if (!g_ready) return 0;
    if (!g_have_state || !same_target(&g_state, st) || g_guest_dirty) {
        /* Bei dirty hat der Software-Pfad (oder der Test) den Gastpuffer
           geaendert; dann darf der alte GPU-Stand nicht zurueckgeschrieben
           werden, sondern wird verworfen und neu aus dem Gast geladen. */
        if (g_have_state && !g_guest_dirty) hw_flush_to_guest();
        if (!create_targets(st)) return 0;
    }
    g_state = *st;
    g_have_state = 1;

    bind_textures(st);

    if (w <= 0.0f) w = 1.0f;
    if (h <= 0.0f) h = 1.0f;
    memset(&vp, 0, sizeof(vp));
    vp.TopLeftX = (float)st->viewport_corner_x;
    vp.TopLeftY = (float)st->viewport_corner_y;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);

    if (st->scissor_mode == 3u) {
        rc.left = st->scissor_x1; rc.top = st->scissor_y1;
        rc.right = st->scissor_x2; rc.bottom = st->scissor_y2;
    } else {
        rc.left = 0; rc.top = 0; rc.right = g_target_w; rc.bottom = g_target_h;
    }
    ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &rc);

    dkey = (st->depth_test ? 1u : 0u) | (st->depth_write ? 2u : 0u) |
           ((st->depth_func & 7u) << 4);
    if (dkey != g_dss_key) {
        memset(&ds, 0, sizeof(ds));
        ds.DepthEnable = st->depth_test ? TRUE : FALSE;
        ds.DepthWriteMask = st->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                            : D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = (D3D11_COMPARISON_FUNC)((int[]){1, 8, 3, 6, 2, 4, 5, 7}[st->depth_func & 7u]);
        ds.StencilEnable = FALSE;
        if (g_dss) { ID3D11DepthStencilState_Release(g_dss); g_dss = NULL; }
        if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &ds, &g_dss))) {
            g_dss_key = 0xFFFFFFFFu;
            return 0;
        }
        g_dss_key = dkey;
    }

    {
        ID3D11RenderTargetView *rtv[2] = { g_color_rtv, g_depth_rtv };
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 2, rtv, g_dsv);
    }
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_dss, 0);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_layout);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    set_constants(st, NULL);
    return 1;
}

int hw_triangles(const HwVertex *v, int n) {
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer *vb = NULL;
    UINT stride = sizeof(HwVertex), offset = 0;
    int i;

    if (!g_ready || !g_have_state || v == NULL || n < 3) return 0;
    if (n % 3) n -= n % 3;
    for (i = 0; i < n; i++) {
        if (v[i].pos[3] == 0.0f) return 0;
    }
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = (UINT)(sizeof(HwVertex) * (size_t)n);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = v;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, &sd, &vb))) return 0;
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_Draw(g_ctx, (UINT)n, 0);
    ID3D11Buffer_Release(vb);
    g_draws++;
    return 1;
}

int hw_flush_to_guest(void) {
    D3D11_MAPPED_SUBRESOURCE cm, dm;
    unsigned char *cbase, *dbase;
    uint32_t cbpp, dbpp, i, h, w;
    int x, y;

    (void)i;
    if (!g_ready || !g_have_state) return 0;
    w = (uint32_t)g_target_w;
    h = (uint32_t)g_target_h;
    cbpp = fmt_bpp(g_state.color_format);
    dbpp = depth_bpp(g_state.depth_format);

    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_color_stage,
                                     (ID3D11Resource *)g_color_tex);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_color_stage, 0,
                                       D3D11_MAP_READ, 0, &cm)))
        return 0;
    cbase = (unsigned char *)cm.pData;

    /* Farbpuffer: PICA kachelt 8x8-Morton und legt die Zeilen von unten
       nach oben (y' = height-1-y), wie p_fb_write im Software-Pfad. */
    if (g_state.color_mem != NULL) {
        for (y = 0; y < (int)h; y++) {
            uint32_t yp = h - 1u - (uint32_t)y;
            const unsigned char *row = cbase + (size_t)y * cm.RowPitch;
            for (x = 0; x < (int)w; x++) {
                uint32_t off = p_morton_offset((uint32_t)x, yp, cbpp) +
                               (yp & ~7u) * w * cbpp;
                unsigned char px[4];
                if ((size_t)off + cbpp > g_state.color_size) continue;
                px[0] = row[x * 4 + 0]; px[1] = row[x * 4 + 1];
                px[2] = row[x * 4 + 2]; px[3] = row[x * 4 + 3];
                color_encode(g_state.color_mem + off, g_state.color_format, px);
            }
        }
    }

    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_depth_stage,
                                     (ID3D11Resource *)g_depth_rt);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_depth_stage, 0,
                                       D3D11_MAP_READ, 0, &dm))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_color_stage, 0);
        return 0;
    }
    dbase = (unsigned char *)dm.pData;
    if (g_state.depth_mem != NULL) {
        uint32_t maxv = (1u << depth_bits(g_state.depth_format)) - 1u;
        for (y = 0; y < (int)h; y++) {
            uint32_t yp = h - 1u - (uint32_t)y;
            const float *row = (const float *)(dbase + (size_t)y * dm.RowPitch);
            for (x = 0; x < (int)w; x++) {
                uint32_t off = p_morton_offset((uint32_t)x, yp, dbpp) +
                               (yp & ~7u) * w * dbpp;
                uint32_t z = (uint32_t)(row[x] * (float)maxv);
                if (z > maxv) z = maxv;
                if ((size_t)off + dbpp > g_state.depth_size) continue;
                g_state.depth_mem[off] = (unsigned char)(z & 0xFFu);
                if (g_state.depth_format != 0u) {
                    g_state.depth_mem[off + 1] = (unsigned char)((z >> 8) & 0xFFu);
                    g_state.depth_mem[off + 2] = (unsigned char)((z >> 16) & 0xFFu);
                }
            }
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_depth_stage, 0);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_color_stage, 0);
    g_flushes++;
    g_guest_dirty = 0;
    return 1;
}
