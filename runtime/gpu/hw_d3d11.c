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
 *   - Output-Merger 0x100..0x107: Blending (alle Faktoren/Gleichungen),
 *     LogicOp, Alpha-Test, Schreibmasken. Blending laeuft native ueber
 *     D3D11-BlendStates; Mischfaelle (Konstantfarbe+Konstantalpha in einem
 *     Draw, Min/Max mit Faktoren) und LogicOp werden im Pixelshader gegen
 *     eine Kopie des Ziels gerechnet (exakt wie sw_framebuffer.cpp).
 *   - Stencil 0x105/0x106 ueber D3D11-DepthStencilState (D24S8).
 *   - Fog 0xE0/0xE1/0xE8..0xEF wie WriteFog im Software-Pfad.
 * Tiefen-/Stencilziel wird aus dem Gastpuffer geladen und beim Flush
 * zurueckgeschrieben (D16/D24/D24S8).
 * Noch nicht enthalten: Fragment-Lighting, W-Buffer, prozedurale Textur,
 * Cube-/Shadow-/Projection-Texturen. gpu.c nutzt den Hardware-Pfad nur, wenn
 * der Draw-Zustand dazu passt, sonst bleibt der Software-Pfad aktiv.
 */
#define COBJMACROS
#include "hw_d3d11.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

/* d3d11.dll/d3dcompiler_47.dll werden erst bei --renderer hw geladen, damit
   der Software-Pfad ohne diese DLLs startet. */
typedef HRESULT (WINAPI *HwD3D11CreateDeviceFn)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *HwD3DCompileFn)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static HMODULE g_d3d11_dll, g_d3dcompiler_dll;
static HwD3D11CreateDeviceFn g_pfn_d3d11_create;
static HwD3DCompileFn g_pfn_d3d_compile;

static void hw_release_dlls(void)
{
    if (g_d3dcompiler_dll) { FreeLibrary(g_d3dcompiler_dll); g_d3dcompiler_dll = NULL; }
    if (g_d3d11_dll) { FreeLibrary(g_d3d11_dll); g_d3d11_dll = NULL; }
    g_pfn_d3d11_create = NULL;
    g_pfn_d3d_compile = NULL;
}

static int hw_load_dlls(void)
{
    if (g_pfn_d3d11_create && g_pfn_d3d_compile) return 1;
    g_d3d11_dll = LoadLibraryA("d3d11.dll");
    g_d3dcompiler_dll = LoadLibraryA("d3dcompiler_47.dll");
    if (g_d3d11_dll)
        g_pfn_d3d11_create = (HwD3D11CreateDeviceFn)(void *)
            GetProcAddress(g_d3d11_dll, "D3D11CreateDevice");
    if (g_d3dcompiler_dll)
        g_pfn_d3d_compile = (HwD3DCompileFn)(void *)
            GetProcAddress(g_d3dcompiler_dll, "D3DCompile");
    if (!g_pfn_d3d11_create || !g_pfn_d3d_compile) {
        fprintf(stderr, "[hw] d3d11.dll/d3dcompiler_47.dll nicht verfuegbar\n");
        hw_release_dlls();
        return 0;
    }
    return 1;
}

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
static ID3D11Texture2D *g_color_shadow;
static ID3D11ShaderResourceView *g_color_shadow_srv;
static ID3D11Texture2D *g_depth_dsv_tex;
static ID3D11DepthStencilView *g_dsv;
static ID3D11Texture2D *g_depth_stage;

static int g_ready;
static int g_warp;
static int g_target_w, g_target_h;
static HwDrawState g_state;
static int g_have_state;
static int g_guest_dirty = 1;
static int g_rendered;          /* GPU-Ergebnisse stehen noch nicht im Gast */
static int g_depth_dirty;       /* seit dem letzten Flush wurde Tiefe/Stencil geschrieben */
static unsigned long long g_draws, g_flushes;
static unsigned long long g_depth_writes;
static unsigned long long g_tex_uploads;
static uint32_t g_dss_key = 0xFFFFFFFFu;
static uint32_t g_bs_key = 0xFFFFFFFFu;

/* Schattenkopie des zuletzt synchronisierten Gast-Ziels. hw_list_begin()
 * vergleicht sie mit dem Gastpuffer: nur wenn die CPU zwischen zwei
 * Kommandolisten hineingeschrieben hat, wird das D3D-Ziel verworfen und neu
 * geladen. Kosten je Liste: ein memcmp ueber Farb- und Tiefenziel (bei
 * 400x240 RGBA8+D24 rund 0,67 MB); ein Neuladen kostet nur bei echter
 * Gast-Aenderung. */
static unsigned char *g_sh_color, *g_sh_depth;
static uint32_t g_sh_color_cap, g_sh_depth_cap;
static uint32_t g_sh_color_len, g_sh_depth_len;
static int g_sh_valid;
static unsigned long long g_sync_kept, g_sync_reloads;
static unsigned long long g_target_loads;   /* volle Gast->D3D-Ladungen */

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
void hw_target_sync_stats(unsigned long long *kept, unsigned long long *reloads) {
    if (kept) *kept = g_sync_kept;
    if (reloads) *reloads = g_sync_reloads;
}
unsigned long long hw_texture_uploads(void) { return g_tex_uploads; }
unsigned long long hw_depth_writes(void) { return g_depth_writes; }
unsigned long long hw_target_loads(void) { return g_target_loads; }
int hw_available(void) { return g_ready; }
void hw_invalidate_guest(void) { g_guest_dirty = 1; g_rendered = 0; }
void hw_invalidate_textures(void) { g_tex_force = 1; g_tex_gen_seen = 0xFFFFFFFFu; }
void hw_sync_guest(void)
{
    if (g_have_state && g_rendered && !g_guest_dirty) hw_flush_to_guest();
    /* Zielzustand verwerfen: der naechste HW-Draw laedt neu aus dem Gast. */
    g_have_state = 0;
    g_rendered = 0;
    g_guest_dirty = 0;
}

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
    "  uint4  k_om;\n"
    "  uint4  k_blend;\n"
    "  uint4  k_fog;\n"
    "  uint4  k_fog_lut[128];\n"
    "};\n"
    "Texture2D tex0 : register(t0);\n"
    "Texture2D tex1 : register(t1);\n"
    "Texture2D tex2 : register(t2);\n"
    "Texture2D tshadow : register(t3);\n"
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
    "uint logic_op(uint s, uint d, uint op) {\n"
    "  if (op == 0u) return 0u;\n"
    "  if (op == 1u) return s & d;\n"
    "  if (op == 2u) return s & (~d & 0xFFu);\n"
    "  if (op == 3u) return s;\n"
    "  if (op == 4u) return 255u;\n"
    "  if (op == 5u) return (~s) & 0xFFu;\n"
    "  if (op == 6u) return d;\n"
    "  if (op == 7u) return (~d) & 0xFFu;\n"
    "  if (op == 8u) return (~(s & d)) & 0xFFu;\n"
    "  if (op == 9u) return (s | d) & 0xFFu;\n"
    "  if (op == 10u) return (~(s | d)) & 0xFFu;\n"
    "  if (op == 11u) return (s ^ d) & 0xFFu;\n"
    "  if (op == 12u) return (~(s ^ d)) & 0xFFu;\n"
    "  if (op == 13u) return ((~s) & d) & 0xFFu;\n"
    "  if (op == 14u) return (s | (~d)) & 0xFFu;\n"
    "  return ((~s) | d) & 0xFFu;\n"
    "}\n"
    "uint blend_factor(uint f, uint ch, uint4 src, uint4 dst, uint4 cst) {\n"
    "  uint cc = (cst >> (8u * ch)) & 0xFFu;\n"
    "  uint ca = (cst >> 24) & 0xFFu;\n"
    "  if (f == 0u) return 0u;\n"
    "  if (f == 1u) return 255u;\n"
    "  if (f == 2u) return src[ch];\n"
    "  if (f == 3u) return 255u - src[ch];\n"
    "  if (f == 4u) return dst[ch];\n"
    "  if (f == 5u) return 255u - dst[ch];\n"
    "  if (f == 6u) return src.a;\n"
    "  if (f == 7u) return 255u - src.a;\n"
    "  if (f == 8u) return dst.a;\n"
    "  if (f == 9u) return 255u - dst.a;\n"
    "  if (f == 10u) return cc;\n"
    "  if (f == 11u) return 255u - cc;\n"
    "  if (f == 12u) return ca;\n"
    "  if (f == 13u) return 255u - ca;\n"
    "  if (f == 14u) return (ch == 3u) ? 255u : min(src.a, 255u - dst.a);\n"
    "  return src[ch];\n"
    "}\n"
    "int blend_eq(uint eq, int s, int d) {\n"
    "  if (eq == 1u) return (s - d) / 255;\n"
    "  if (eq == 2u) return (d - s) / 255;\n"
    "  if (eq == 3u) return (min(s, d)) / 255;\n"
    "  if (eq == 4u) return (max(s, d)) / 255;\n"
    "  return (s + d) / 255;\n"
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
    "  float d = (0.0 - i.pos.z) * k_param.x + k_param.y;\n"
    "  d = saturate(d);\n"
    "  if (k_fog.x == 5u) {\n"
    "    float fog_index = ((k_fog.y != 0u) ? (1.0 - d) : d) * 128.0;\n"
    "    float fi = clamp(floor(fog_index), 0.0, 127.0);\n"
    "    float ff = fog_index - fi;\n"
    "    uint e = k_fog_lut[(uint)fi].x;\n"
    "    int rawdiff = (int)(e & 0x1FFFu);\n"
    "    if ((rawdiff & 0x1000) != 0) rawdiff -= 0x2000;\n"
    "    float fact = saturate((float)((e >> 13) & 0x7FFu) / 2047.0 +\n"
    "                          (float)rawdiff / 2047.0 * ff);\n"
    "    uint3 fc = uint3(k_fog.z & 0xFFu, (k_fog.z >> 8) & 0xFFu, (k_fog.z >> 16) & 0xFFu);\n"
    "    res.rgb = (uint3)floor((float3)res.rgb * fact +\n"
    "                           (1.0 - fact) * (float3)fc);\n"
    "  }\n"
    "  if (k_om.x != 0u || k_om.y != 0u) {\n"
    "    uint4 dst = (uint4)round(tshadow.Load(int3((int2)i.pos.xy, 0)) * 255.0);\n"
    "    if (k_om.x != 0u) {\n"
    "      uint srgb = k_blend.z & 15u, drgb = (k_blend.z >> 8) & 15u;\n"
    "      uint sa = k_blend.w & 15u, da = (k_blend.w >> 8) & 15u;\n"
    "      uint4 cst = uint4(k_om.z & 0xFFu, (k_om.z >> 8) & 0xFFu,\n"
    "                        (k_om.z >> 16) & 0xFFu, (k_om.z >> 24) & 0xFFu);\n"
    "      uint4 outb = res;\n"
    "      [unroll] for (uint k = 0; k < 4u; k++) {\n"
    "        uint sf = (k == 3u) ? sa : srgb;\n"
    "        uint df = (k == 3u) ? da : drgb;\n"
    "        uint eq = (k == 3u) ? k_blend.y : k_blend.x;\n"
    "        int sv = (int)res[k] * (int)blend_factor(sf, k, res, dst, cst);\n"
    "        int dv = (int)dst[k] * (int)blend_factor(df, k, res, dst, cst);\n"
    "        outb[k] = (uint)clamp(blend_eq(eq, sv, dv), 0, 255);\n"
    "      }\n"
    "      res = outb;\n"
    "    }\n"
    "    if (k_om.y != 0u) {\n"
    "      res = uint4(logic_op(res.r, dst.r, k_om.y),\n"
    "                      logic_op(res.g, dst.g, k_om.y),\n"
    "                      logic_op(res.b, dst.b, k_om.y),\n"
    "                      logic_op(res.a, dst.a, k_om.y));\n"
    "    }\n"
    "  }\n"
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

static DXGI_FORMAT depth_dxgi(uint32_t fmt) {
    if (fmt == 3) return DXGI_FORMAT_D24_UNORM_S8_UINT;
    if (fmt == 0) return DXGI_FORMAT_D16_UNORM;
    return DXGI_FORMAT_D32_FLOAT;
}

/* Gast-Tiefen-/Stencilpuffer (PICA-Kachelung) in die Staging-Flaeche des
 * D3D-Tiefenziels dekodieren (bzw. zurueck). Byteordnung wie p_depth_get/
 * p_depth_set: D16=2 Byte, D24=3 Byte, D24S8=4 Byte (Stencil im 4. Byte). */
static void depth_from_guest(const HwDrawState *st, unsigned char *base,
                             uint32_t pitch, int w, int h) {
    uint32_t bpp = depth_bpp(st->depth_format);
    uint32_t maxv = (1u << depth_bits(st->depth_format)) - 1u;
    int x, y;
    for (y = 0; y < h; y++) {
        uint32_t yp = (uint32_t)h - 1u - (uint32_t)y;
        unsigned char *row = base + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            uint32_t off = p_morton_offset((uint32_t)x, yp, bpp) +
                           (yp & ~7u) * (uint32_t)w * bpp;
            uint32_t z = maxv, s = 0;
            if (st->depth_mem != NULL && (size_t)off + bpp <= st->depth_size) {
                const unsigned char *p = st->depth_mem + off;
                z = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
                if (st->depth_format != 0u)
                    z |= (uint32_t)p[2] << 16;
                if (st->depth_format == 3u) s = p[3];
            }
            if (st->depth_format == 0u) {
                ((uint16_t *)row)[x] = (uint16_t)z;
            } else if (st->depth_format == 3u) {
                ((uint32_t *)row)[x] = (z & 0xFFFFFFu) | (s << 24);
            } else {
                /* D24 liegt in D3D als D32_FLOAT. Skaliert wird mit 2^24
                 * (Zweierpotenz = in float exakt): z/2^24 ist fuer jedes
                 * z < 2^24 exakt darstellbar, und die Rueckrechnung
                 * f*2^24 in depth_to_guest gewinnt z konstruktiv bitgleich
                 * zurueck (kein 1-LSB-Drift bei unberuehrten Pixeln). */
                ((float *)row)[x] = (float)z / 16777216.0f;
            }
        }
    }
}

static void depth_to_guest(const HwDrawState *st, const unsigned char *base,
                           uint32_t pitch, int w, int h) {
    uint32_t bpp = depth_bpp(st->depth_format);
    int x, y;
    if (st->depth_mem == NULL) return;
    for (y = 0; y < h; y++) {
        uint32_t yp = (uint32_t)h - 1u - (uint32_t)y;
        const unsigned char *row = base + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            uint32_t off = p_morton_offset((uint32_t)x, yp, bpp) +
                           (yp & ~7u) * (uint32_t)w * bpp;
            uint32_t z, s = 0;
            if ((size_t)off + bpp > st->depth_size) continue;
            if (st->depth_format == 0u) {
                z = ((const uint16_t *)row)[x];
            } else if (st->depth_format == 3u) {
                uint32_t v = ((const uint32_t *)row)[x];
                z = v & 0xFFFFFFu;
                s = (v >> 24) & 0xFFu;
            } else {
                float f = ((const float *)row)[x];
                /* Exakte Umkehrung von depth_from_guest (Skala 2^24). */
                if (!(f == f)) f = 0.0f;          /* NaN */
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                z = (uint32_t)(f * 16777216.0f);
                if (z > 0xFFFFFFu) z = 0xFFFFFFu;
            }
            st->depth_mem[off] = (unsigned char)(z & 0xFFu);
            if (st->depth_format != 0u) {
                st->depth_mem[off + 1u] = (unsigned char)((z >> 8) & 0xFFu);
                st->depth_mem[off + 2u] = (unsigned char)((z >> 16) & 0xFFu);
            }
            if (st->depth_format == 3u)
                st->depth_mem[off + 3u] = (unsigned char)(s & 0xFFu);
        }
    }
}

/* ---- Schattenkopie des Gast-Ziels (hw_list_begin) ---------------------- */

static void shadow_invalidate(void) {
    g_sh_valid = 0;
    g_sh_color_len = 0;
    g_sh_depth_len = 0;
}

static int shadow_reserve(unsigned char **buf, uint32_t *cap, uint32_t need) {
    unsigned char *p;
    if (*cap >= need) return 1;
    p = (unsigned char *)realloc(*buf, need);
    if (!p) return 0;
    *buf = p;
    *cap = need;
    return 1;
}

/* Merkt sich die Gastbytes des aktuellen Ziels. Aufruf nach jedem Flush und
 * nach jedem Neuladen: damit ist die Kopie genau der Stand, den D3D kennt. */
static void shadow_capture(void) {
    uint32_t clen = 0, dlen = 0;
    if (!g_have_state || g_target_w <= 0 || g_target_h <= 0) {
        shadow_invalidate();
        return;
    }
    if (g_state.color_mem != NULL) {
        uint64_t n = (uint64_t)g_target_w * (uint64_t)g_target_h *
                     fmt_bpp(g_state.color_format);
        if (n > g_state.color_size) { shadow_invalidate(); return; }
        clen = (uint32_t)n;
    }
    if (g_state.depth_mem != NULL) {
        uint64_t n = (uint64_t)g_target_w * (uint64_t)g_target_h *
                     depth_bpp(g_state.depth_format);
        if (n > g_state.depth_size) { shadow_invalidate(); return; }
        dlen = (uint32_t)n;
    }
    if (!shadow_reserve(&g_sh_color, &g_sh_color_cap, clen) ||
        !shadow_reserve(&g_sh_depth, &g_sh_depth_cap, dlen)) {
        shadow_invalidate();
        return;
    }
    if (clen) memcpy(g_sh_color, g_state.color_mem, clen);
    if (dlen) memcpy(g_sh_depth, g_state.depth_mem, dlen);
    g_sh_color_len = clen;
    g_sh_depth_len = dlen;
    g_sh_valid = 1;
}

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
    if (g_color_shadow_srv) { ID3D11ShaderResourceView_Release(g_color_shadow_srv); g_color_shadow_srv = NULL; }
    if (g_color_shadow) { ID3D11Texture2D_Release(g_color_shadow); g_color_shadow = NULL; }
    if (g_color_rtv) { ID3D11RenderTargetView_Release(g_color_rtv); g_color_rtv = NULL; }
    if (g_color_tex) { ID3D11Texture2D_Release(g_color_tex); g_color_tex = NULL; }
    if (g_depth_stage) { ID3D11Texture2D_Release(g_depth_stage); g_depth_stage = NULL; }
    if (g_dsv) { ID3D11DepthStencilView_Release(g_dsv); g_dsv = NULL; }
    if (g_depth_dsv_tex) { ID3D11Texture2D_Release(g_depth_dsv_tex); g_depth_dsv_tex = NULL; }
    g_target_w = g_target_h = 0;
}

static int create_targets(const HwDrawState *st) {
    D3D11_TEXTURE2D_DESC td;
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd;
    D3D11_MAPPED_SUBRESOURCE map;
    int w = (int)st->color_width, h = (int)st->color_height;
    unsigned char *stage;
    uint32_t bpp = fmt_bpp(st->color_format);
    int x, y;
    DXGI_FORMAT dfmt = depth_dxgi(st->depth_format);

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

    /* Schattenkopie des Farbziels fuer LogicOp und Shader-Blending. */
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_color_shadow))) return 0;
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv;
        memset(&sv, 0, sizeof(sv));
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(
                g_dev, (ID3D11Resource *)g_color_shadow, &sv, &g_color_shadow_srv)))
            return 0;
    }

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

    /* Tiefen-/Stencilziel im Gastformat (D16/D24/D24S8) plus Staging fuer
       Laden und Rueckschreiben. */
    memset(&td, 0, sizeof(td));
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = dfmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_depth_dsv_tex))) return 0;
    memset(&dd, 0, sizeof(dd));
    dd.Format = dfmt;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)g_depth_dsv_tex,
                                                    &dd, &g_dsv))) return 0;

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_depth_stage))) return 0;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_depth_stage, 0,
                                       D3D11_MAP_WRITE, 0, &map))) return 0;
    depth_from_guest(st, (unsigned char *)map.pData, map.RowPitch, w, h);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_depth_stage, 0);
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_depth_dsv_tex,
                                     (ID3D11Resource *)g_depth_stage);

    /* Staging-Textur fuer den Farb-Rueckweg. */
    memset(&td, 0, sizeof(td));
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_color_stage))) return 0;

    g_target_w = w;
    g_target_h = h;
    g_target_loads++;
    g_guest_dirty = 0;
    g_rendered = 0;
    g_depth_dirty = 0;
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

    hr = g_pfn_d3d_compile(k_hlsl, strlen(k_hlsl), "pica_hw", NULL, NULL, "VSMain",
                    "vs_5_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) {
        if (err) { fprintf(stderr, "[hw] VS-Fehler: %s\n", (char *)ID3D10Blob_GetBufferPointer(err)); }
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    hr = g_pfn_d3d_compile(k_hlsl, strlen(k_hlsl), "pica_hw", NULL, NULL, "PSMain",
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

    /* TEV-Constant-Buffer: 176 Woerter + k_om/k_blend/k_fog + Fog-LUT
       (128 x uint4) = 700 Woerter. */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = 700u * 4u;
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
    if (!hw_load_dlls()) {
        fprintf(stderr, "[hw] D3D11 nicht ladbar, Software-Pfad bleibt aktiv\n");
        return 0;
    }
    g_warp = (driver != NULL && strcmp(driver, "warp") == 0);
    hr = g_pfn_d3d11_create(NULL, g_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                           NULL, 0, NULL, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr) && !g_warp) {
        printf("[hw] Hardware-Treiber nicht verfuegbar (hr=%08lX), Fallback WARP\n",
               (unsigned long)hr);
        g_warp = 1;
        hr = g_pfn_d3d11_create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
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
    for (i = 0; i < 3; i++) {        if (g_texslot[i].srv) { ID3D11ShaderResourceView_Release(g_texslot[i].srv); g_texslot[i].srv = NULL; }
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
    g_guest_dirty = 1;
    g_rendered = 0;
    g_depth_dirty = 0;
    g_tex_gen_seen = 0xFFFFFFFFu;
    g_tex_force = 1;
    free(g_sh_color); g_sh_color = NULL;
    free(g_sh_depth); g_sh_depth = NULL;
    g_sh_color_cap = g_sh_depth_cap = 0;
    shadow_invalidate();
    g_sync_kept = g_sync_reloads = 0;
    g_target_loads = 0;
    hw_release_dlls();
}

/* ---- Constant Buffer ---------------------------------------------------
 * Layout exakt wie cbuffer PicaCB in k_hlsl (700 Woerter / 2800 Byte). */
typedef union {
    float f[700];
    uint32_t u[700];
} CbData;

/* Shader-Blending ist noetig, wenn D3D die PICA-Semantik nicht nativ
 * abbildet: Min/Max werten die Faktoren nicht aus, und Konstantfarbe +
 * Konstantalpha in einem Draw brauchen verschiedene Blend-Konstanten. */
static int pica_shader_blend(const HwDrawState *st) {
    uint32_t f[4];
    int has_cc = 0, has_ca = 0, i;
    if (!st->blend_enable) return 0;
    if (st->blend_eq_rgb >= 3u || st->blend_eq_a >= 3u) return 1;
    f[0] = st->blend_src_rgb; f[1] = st->blend_dst_rgb;
    f[2] = st->blend_src_a; f[3] = st->blend_dst_a;
    for (i = 0; i < 4; i++) {
        if ((f[i] & 0xFu) == 10u || (f[i] & 0xFu) == 11u) has_cc = 1;
        if ((f[i] & 0xFu) == 12u || (f[i] & 0xFu) == 13u) has_ca = 1;
    }
    return has_cc && has_ca;
}

static int pica_logic_active(const HwDrawState *st) {
    return !st->blend_enable && (st->logic_op & 0xFu) != 3u;
}

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
    cb.u[176] = (uint32_t)pica_shader_blend(st);
    cb.u[177] = pica_logic_active(st) ? (st->logic_op & 0xFu) : 0u;
    cb.u[178] = st->blend_const;
    cb.u[180] = st->blend_eq_rgb & 7u;
    cb.u[181] = st->blend_eq_a & 7u;
    cb.u[182] = (st->blend_src_rgb & 0xFu) | ((st->blend_dst_rgb & 0xFu) << 8);
    cb.u[183] = (st->blend_src_a & 0xFu) | ((st->blend_dst_a & 0xFu) << 8);
    cb.u[184] = (uint32_t)(st->fog_mode & 7);
    cb.u[185] = (uint32_t)(st->fog_flip ? 1 : 0);
    cb.u[186] = st->fog_color;
    for (i = 0; i < 128; i++) cb.u[188 + i * 4] = st->fog_lut[i];
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
    int gen_check = g_tex_force || st->tex_generation != g_tex_gen_seen;
    int i;

    for (i = 0; i < 3; i++) {
        HwTexSlot *s = &g_texslot[i];
        const HwTexture *t = &st->tex[i];
        if (t->type != 0u || !t->enabled || t->bytes == 0u ||
            st->guest_mem == NULL ||
            (uint64_t)t->offset + t->bytes > st->guest_size) {
            s->valid = 0;
        } else {
            /* Der Schluessel (Adresse/Format/Groesse) kann sich innerhalb
               derselben Kommandoliste aendern: bei JEDEM Draw pruefen. Der
               teure Byte-Hash laeuft nur bei neuer Generation. */
            int key_ok = s->valid && s->offset == t->offset &&
                         s->fmt == t->format && s->w == t->width &&
                         s->h == t->height;
            uint64_t h = 0;
            if (!key_ok) {
                if (texslot_upload(i, st)) s->valid = 1;
                else s->valid = 0;
                if (s->valid && gen_check) s->hash = fnv1a(st->guest_mem + t->offset, t->bytes);
            } else if (gen_check) {
                h = fnv1a(st->guest_mem + t->offset, t->bytes);
                if (s->hash != h) {
                    if (texslot_upload(i, st)) s->valid = 1;
                    else s->valid = 0;
                    if (s->valid) s->hash = h;
                }
            }
        }
        srv[i] = s->valid ? s->srv : g_tex_dummy;
    }
    if (gen_check) {
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
    int new_target = 0;

    if (!g_ready) return 0;
    if (!g_have_state || !same_target(&g_state, st) || g_guest_dirty) {
        /* Bei dirty hat der Software-Pfad (oder der Test) den Gastpuffer
           geaendert; dann darf der alte GPU-Stand nicht zurueckgeschrieben
           werden, sondern wird verworfen und neu aus dem Gast geladen. */
        if (g_have_state && !g_guest_dirty && g_rendered) hw_flush_to_guest();
        if (!create_targets(st)) {
            g_have_state = 0;
            g_rendered = 0;
            return 0;
        }
        new_target = 1;
    }
    g_state = *st;
    /* Der Flush eines Zielwechsels (oben) schreibt das gerade gerenderte
       Ziel in den Gast und erhoeht dabei die Texturgeneration. Erst danach
       lesen: sonst sampelt der erste Draw nach dem Zielwechsel eine Textur,
       die genau dieses Ziel ist, mit dem alten Cache-Stand. */
    g_state.tex_generation = gpu_pica_tex_generation();
    g_have_state = 1;
    if (new_target) shadow_capture();

    bind_textures(&g_state);

    /* Schattenkopie fuer LogicOp/Shader-Blending: eine Momentaufnahme des
       Ziels vor diesem Draw (wie fb.GetPixel in sw_rasterizer.cpp). */
    {
        int shadow = pica_shader_blend(st) || pica_logic_active(st);
        if (shadow) {
            ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_color_shadow,
                                             (ID3D11Resource *)g_color_tex);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 3, 1, &g_color_shadow_srv);
        } else {
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 3, 1, &g_tex_dummy);
        }
    }

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

    dkey = (uint32_t)(st->depth_test | (st->depth_write << 1) |
                      (st->depth_func << 2));
    dkey = dkey * 33u + (uint32_t)(st->stencil_enable |
                                   (st->stencil_func << 1) |
                                   (st->stencil_input_mask << 4) |
                                   (st->stencil_write_mask << 12) |
                                   (st->stencil_fail_op << 20) |
                                   (st->stencil_zfail_op << 23) |
                                   (st->stencil_pass_op << 26));
    if (dkey != g_dss_key) {
        static const D3D11_COMPARISON_FUNC cmpf[8] = {
            D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS,
            D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_NOT_EQUAL,
            D3D11_COMPARISON_LESS, D3D11_COMPARISON_LESS_EQUAL,
            D3D11_COMPARISON_GREATER, D3D11_COMPARISON_GREATER_EQUAL,
        };
        static const D3D11_STENCIL_OP stop[8] = {
            D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO,
            D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_INCR_SAT,
            D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT,
            D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR,
        };
        memset(&ds, 0, sizeof(ds));
        ds.DepthEnable = (st->depth_test || st->depth_write) ? TRUE : FALSE;
        ds.DepthWriteMask = st->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                            : D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = st->depth_test ? cmpf[st->depth_func & 7u]
                                      : D3D11_COMPARISON_ALWAYS;
        ds.StencilEnable = st->stencil_enable ? TRUE : FALSE;
        ds.StencilReadMask = (UINT8)(st->stencil_input_mask & 0xFFu);
        ds.StencilWriteMask = (UINT8)(st->stencil_write_mask & 0xFFu);
        ds.FrontFace.StencilFunc = cmpf[st->stencil_func & 7u];
        ds.FrontFace.StencilFailOp = stop[st->stencil_fail_op & 7u];
        ds.FrontFace.StencilDepthFailOp = stop[st->stencil_zfail_op & 7u];
        ds.FrontFace.StencilPassOp = stop[st->stencil_pass_op & 7u];
        ds.BackFace = ds.FrontFace;
        if (g_dss) { ID3D11DepthStencilState_Release(g_dss); g_dss = NULL; }
        if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &ds, &g_dss))) {
            g_dss_key = 0xFFFFFFFFu;
            return 0;
        }
        g_dss_key = dkey;
    }

    /* Blend-/Schreibmaskenzustand 0x100..0x107/0x113. */
    {
        uint32_t bkey, mask;
        int shader_blend = pica_shader_blend(st);
        int ca_only = 0;
        float bf[4];
        bkey = (uint32_t)(st->blend_enable | (shader_blend << 1) |
                          ((st->logic_op & 0xFu) << 2) |
                          ((st->blend_eq_rgb & 7u) << 6) |
                          ((st->blend_eq_a & 7u) << 9) |
                          ((st->blend_src_rgb & 0xFu) << 12) |
                          ((st->blend_dst_rgb & 0xFu) << 16) |
                          ((st->blend_src_a & 0xFu) << 20) |
                          ((st->blend_dst_a & 0xFu) << 24) |
                          ((st->color_write_mask & 0xFu) << 28));
        bkey = bkey * 33u + (uint32_t)(st->color_write_enable | (st->blend_const << 1));
        if (bkey != g_bs_key) {
            static const D3D11_BLEND blendmap[16] = {
                D3D11_BLEND_ZERO, D3D11_BLEND_ONE,
                D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR,
                D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
                D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA,
                D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA,
                D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR,
                D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR,
                D3D11_BLEND_SRC_ALPHA_SAT, D3D11_BLEND_ONE,
            };
            static const D3D11_BLEND_OP eqmap[5] = {
                D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT,
                D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_OP_MIN,
                D3D11_BLEND_OP_MAX,
            };
            D3D11_BLEND_DESC bl;
            memset(&bl, 0, sizeof(bl));
            bl.AlphaToCoverageEnable = FALSE;
            bl.IndependentBlendEnable = FALSE;
            bl.RenderTarget[0].BlendEnable = (st->blend_enable && !shader_blend) ? TRUE : FALSE;
            bl.RenderTarget[0].SrcBlend = blendmap[st->blend_src_rgb & 0xFu];
            bl.RenderTarget[0].DestBlend = blendmap[st->blend_dst_rgb & 0xFu];
            bl.RenderTarget[0].BlendOp = eqmap[st->blend_eq_rgb < 5u ? st->blend_eq_rgb : 0u];
            bl.RenderTarget[0].SrcBlendAlpha = blendmap[st->blend_src_a & 0xFu];
            bl.RenderTarget[0].DestBlendAlpha = blendmap[st->blend_dst_a & 0xFu];
            bl.RenderTarget[0].BlendOpAlpha = eqmap[st->blend_eq_a < 5u ? st->blend_eq_a : 0u];
            mask = st->color_write_enable ? (st->color_write_mask & 0xFu) : 0u;
            bl.RenderTarget[0].RenderTargetWriteMask =
                (UINT8)(((mask & 1u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
                        ((mask & 2u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
                        ((mask & 4u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
                        ((mask & 8u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
            if (g_bs) { ID3D11BlendState_Release(g_bs); g_bs = NULL; }
            if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bl, &g_bs))) {
                g_bs_key = 0xFFFFFFFFu;
                return 0;
            }
            g_bs_key = bkey;
        }
        /* PICA-Konstanten: 10/11 = Farbe (r,g,b,a), 12/13 = Alpha. Nur
           wenn beide Familien vorkommen, uebernimmt der Shader. */
        {
            uint32_t f[4];
            int i, has_ca = 0, has_cc = 0;
            f[0] = st->blend_src_rgb; f[1] = st->blend_dst_rgb;
            f[2] = st->blend_src_a; f[3] = st->blend_dst_a;
            for (i = 0; i < 4; i++) {
                if ((f[i] & 0xFu) == 12u || (f[i] & 0xFu) == 13u) has_ca = 1;
                if ((f[i] & 0xFu) == 10u || (f[i] & 0xFu) == 11u) has_cc = 1;
            }
            ca_only = has_ca && !has_cc;
        }
        for (bkey = 0; bkey < 4u; bkey++) {
            uint32_t v = ca_only ? ((st->blend_const >> 24) & 0xFFu)
                                 : ((st->blend_const >> (8u * bkey)) & 0xFFu);
            bf[bkey] = (float)v / 255.0f;
        }        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_bs, bf, 0xFFFFFFFFu);
    }

    {
        ID3D11RenderTargetView *rtv[1] = { g_color_rtv };
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, rtv, g_dsv);
    }
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_dss,
                                               (UINT)(st->stencil_ref & 0xFFu));
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
    g_rendered = 1;
    if (g_state.depth_write ||
        (g_state.stencil_enable && g_state.stencil_write_mask != 0u &&
         (g_state.stencil_fail_op != 0u || g_state.stencil_zfail_op != 0u ||
          g_state.stencil_pass_op != 0u)))
        g_depth_dirty = 1;
    return 1;
}

int hw_flush_to_guest(void) {
    D3D11_MAPPED_SUBRESOURCE cm, dm;
    unsigned char *cbase, *dbase;
    uint32_t cbpp, h, w;
    int x, y;

    if (!g_ready || !g_have_state) return 0;
    if (!g_rendered) return 0;
    if (!g_color_tex || !g_color_stage || g_target_w <= 0 || g_target_h <= 0)
        return 0;
    w = (uint32_t)g_target_w;
    h = (uint32_t)g_target_h;
    cbpp = fmt_bpp(g_state.color_format);

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

    /* Tiefe/Stencil nur zurueckschreiben, wenn in diesem Ziel seit dem
       letzten Flush tatsaechlich Tiefe oder Stencil geschrieben wurde. Das
       vermeidet verlustbehaftete D24-Umwege ueber D32_FLOAT. */
    if (g_depth_dirty && g_state.depth_mem != NULL &&
        g_depth_dsv_tex != NULL && g_depth_stage != NULL) {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_depth_stage,
                                         (ID3D11Resource *)g_depth_dsv_tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_depth_stage, 0,
                                           D3D11_MAP_READ, 0, &dm))) {
            dbase = (unsigned char *)dm.pData;
            depth_to_guest(&g_state, dbase, dm.RowPitch, (int)w, (int)h);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_depth_stage, 0);
        }
        g_depth_writes++;
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_color_stage, 0);
    g_flushes++;
    g_rendered = 0;
    g_depth_dirty = 0;
    g_guest_dirty = 0;
    /* Render-to-Texture: der Gastpuffer ist jetzt neuer Texturinhalt. */
    gpu_pica_bump_tex_generation();
    /* Der Gast entspricht jetzt dem D3D-Ziel; Schatten fuer hw_list_begin. */
    shadow_capture();
    return 1;
}

void hw_list_begin(void) {
    if (!g_ready || !g_have_state) return;
    /* Ausstehende Ergebnisse zuerst in den Gast (falls ein Aufrufer den
     * Listenanfang ohne Listenende erreicht hat); danach ist der Gast der
     * gueltige Stand. */
    if (g_rendered && !g_guest_dirty) hw_flush_to_guest();
    if (!g_sh_valid ||
        (g_sh_color_len != 0u && g_state.color_mem != NULL &&
         memcmp(g_state.color_mem, g_sh_color, g_sh_color_len) != 0) ||
        (g_sh_depth_len != 0u && g_state.depth_mem != NULL &&
         memcmp(g_state.depth_mem, g_sh_depth, g_sh_depth_len) != 0)) {
        /* Die Gast-CPU hat das Ziel zwischen zwei Listen veraendert (oder es
         * gibt keine gueltige Schattenkopie): Ziel konservativ verwerfen,
         * der naechste Draw laedt es neu aus dem Gast. */
        g_have_state = 0;
        g_rendered = 0;
        g_guest_dirty = 0;
        shadow_invalidate();
        g_sync_reloads++;
    } else {
        g_sync_kept++;
    }
}
