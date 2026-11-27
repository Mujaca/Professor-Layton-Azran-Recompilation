/* D3D11-Rasterizer fuer die PICA-Fragment-Pipeline.
 *
 * Nur Rasterisierung + Fragment-Pipeline liegen auf der Host-GPU; Vertex- und
 * Geometry-Shader bleiben der CPU-Interpreter in runtime/gpu/gpu.c. Das Modul
 * ist eine schmale Schnittstelle (hw_d3d11.h); runtime/main.c aktiviert es
 * standardmaessig (M17), `--renderer soft` erzeugt kein Device.
 *
 * Umfang:
 *   - Clip-Space-Dreiecke mit POSITION + PrimaryColor + TEXCOORD0..2
 *   - Viewport, Scissor (Include), Tiefentest/-schreiben, Alpha-Test
 *   - Farbziele RGBA8/RGB8/RGB5A1/RGB565/RGBA4 in PICA-Kachelung
 *   - Tiefenziele D16/D24/D24S8 in PICA-Kachelung
 *   - Texturen (Einheit 0..2, Typ 2D/Disabled): alle PICA-Formate werden mit
 *     dem Software-Dekoder (gpu_pica_decode_texel, inkl. ETC1/ETC1A4) nach
 *     RGBA8 dekodiert und hochgeladen. Cache mit bis zu 32 Texturen (LRU),
 *     Schluessel (Adresse, Format, Breite/Hoehe, Groesse); Inhalt per
 *     FNV-1a einmal pro Kommandoliste je Textur sowie nach GX-Schreib-
 *     zugriffen in den Bereich bzw. HW-Rueckschreiben (Generation).
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
 *   - Fragment-Lighting 0x140..0x1D9 im Pixelshader (light_frag), Schritt
 *     fuer Schritt wie p_compute_fragments (Azahar sw_lighting.cpp
 *     ComputeFragmentsColors): bis 8 Lichter, alle 24 LUTs (Wert+Differenz
 *     als Buffer<float2>, nur geaenderte Zeilen werden hochgeladen),
 *     Abs/Skalierung/Eingaenge 0x1D0..0x1D2, Bump-/Tangenten-Modi,
 *     Clamp-Highlights, Geometriefaktoren, Schatten, Fresnel.
 *     Quaternion/View/tc0_w interpoliert D3D perspektivkorrekt.
 * Tiefen-/Stencilziel wird aus dem Gastpuffer geladen und beim Flush
 * zurueckgeschrieben (D16/D24/D24S8).
 * Noch nicht enthalten: W-Buffer, prozedurale Textur,
 * Cube-/Shadow-/Projection-Texturen. gpu.c nutzt den Hardware-Pfad nur, wenn
 * der Draw-Zustand dazu passt, sonst bleibt der Software-Pfad aktiv.
 */
#define COBJMACROS
#include "hw_d3d11.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

/* d3d11.dll/d3dcompiler_47.dll werden erst mit dem HW-Renderer geladen, damit
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
static ID3D11Buffer *g_lcb;                 /* Lighting-Constant-Buffer (b1) */
static ID3D11Buffer *g_lut_buf;             /* 24 x 256 float2 (t4) */
static ID3D11ShaderResourceView *g_lut_srv;
static float g_lut_cpu[24][256][2];         /* Stand des GPU-Buffers */
static uint32_t g_lut_gen_seen;
static uint32_t g_lut_rowgen_seen[24];
static int g_lut_valid;

/* Layout exakt wie cbuffer LightCB in k_hlsl (69 x 16 Byte). */
typedef struct {
    uint32_t cfg[4];
    float gal[4];
    uint32_t in[4];
    float scl[8];
    float pos[8][4];
    float spot[8][4];
    float spec0[8][4];
    float spec1[8][4];
    float diff[8][4];
    float amb[8][4];
    float dist[8][4];
    uint32_t flag[8][4];
} LightCbData;

static LightCbData g_lcb_last;
static int g_lcb_valid;

/* ---- Dreiecks-Batching -------------------------------------------------
 * gpu.c uebergibt jedes geclippte Dreieck einzeln (hw_begin_draw +
 * hw_triangles). Solange der Eingangszustand byte-gleich bleibt (memcmp
 * gegen g_state_in, dieselbe Texturgeneration, keine Invalidierung
 * dazwischen), werden die Dreiecke nur gesammelt und spaeter mit EINEM
 * Draw aus einem dynamischen Vertexpuffer gezeichnet. D3D11 haelt die
 * Reihenfolge der Primitive innerhalb eines Draws ein (Tiefe, Stencil,
 * Blending), das Ergebnis ist dasselbe wie mit Einzel-Draws.
 * LogicOp/Shader-Blending lesen eine Momentaufnahme des Ziels (g_color_shadow)
 * vor dem Draw. Die Momentaufnahme entsteht deshalb direkt vor jedem
 * Batch-Draw, und ein Batch nimmt nur Dreiecke auf, deren Pixel sich nicht
 * mit einem frueheren Dreieck desselben Batches ueberschneiden (Abdeckung
 * je Pixel in g_cov nach den D3D11-Rasterregeln: 16.8-Festkomma,
 * Pixelmitte, Top-Left-Regel). Ueberschneidung -> Batch zeichnen,
 * neue Momentaufnahme. So liest jedes Fragment genau den Zielwert, den
 * Einzel-Draws (und der Software-Pfad) sehen wuerden. */
#define HW_PEND_CAP 6144                 /* Vertices im CPU-Puffer */
#define HW_VB_CAP   (HW_PEND_CAP * 16)   /* Vertices im D3D-Ringpuffer */
static ID3D11Buffer *g_vb;
static UINT g_vb_pos;
static HwVertex g_pend[HW_PEND_CAP];
static int g_pend_n;
static HwDrawState g_state_in;           /* Eingangszustand des Batches */
static int g_batch_ok;                   /* g_state_in gilt fuer den Fast-Path */
static unsigned long long g_batches;     /* tatsaechliche D3D-Draws */
#define HW_SUBMIT_BATCHES 32             /* D3D-Draws je vorzeitiger Uebergabe */
static unsigned g_submit_cnt;
static unsigned long long g_submits;     /* vorzeitige Uebergaben (Flush) */
static int g_cov_track;                  /* Batch braucht Momentaufnahme */
static unsigned g_begin_serial;          /* volle hw_begin_draw-Aufrufe */
static uint16_t *g_cov;                  /* Batch-Id je Pixel */
static int g_cov_w, g_cov_h;
static uint16_t g_cov_id = 1;
static int g_cov_x0, g_cov_y0, g_cov_x1 = -1, g_cov_y1 = -1;  /* Batch-Rechteck */
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

/* ---- Texturcache -------------------------------------------------------
 * Schluessel: Guest-Offset, Format, Breite/Hoehe und Bytezahl (die
 * PICA-Texturgroesse 0x82 bestimmt Dimensionen und damit die Mip-Stufen; der
 * Port dekodiert wie der Software-Pfad nur Level 0). Der Cache haelt mehrere
 * Texturen gleichzeitig (der Titel wechselt je Draw zwischen vielen), damit
 * ein erneutes Binden ohne Dekodieren/Upload auskommt. Der Inhalt wird per
 * FNV-1a ueber die Quellbytes geprueft:
 *   (a) GX-Schreiboperationen (MemoryFill/DisplayTransfer/TextureCopy/
 *       RequestDma/Y2R) markieren nur die ueberlappenden Eintraege als stale,
 *   (b) HW-Rueckschreiben/SW-Draws erhoehen die Texturgeneration; jeder
 *       Eintrag merkt sich die zuletzt gepruefte Generation (gen_seen).
 *       Eine globale "schon gesehen"-Marke reicht nicht: ein Draw, der nur
 *       Textur B bindet, wuerde die neue Generation fuer A mitverbrauchen,
 *   (c) CPU-Schreibzugriffe erkennt der Hash einmal pro Kommandoliste je
 *       Textur (hw_list_begin erhoeht g_list_serial). */
typedef struct {
    uint32_t offset, bytes, fmt, w, h;
    uint64_t hash;
    uint32_t list_seen;         /* Kommandolisten-Serie der letzten Pruefung */
    uint32_t gen_seen;          /* Texturgeneration der letzten Pruefung */
    uint32_t last_use;          /* LRU */
    int stale;                  /* Schreibzugriff im Bereich -> neu pruefen */
    int valid;                  /* hash passt zum hochgeladenen Bild */
    unsigned char *src;         /* Quellbytes des hochgeladenen Bildes */
    uint32_t src_len;           /* (Kachelbereich, fuer Teil-Uploads) */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} HwTexSlot;

#define HW_TEX_CACHE_N 32
#define HW_TEX_CACHE_BYTES (64ull * 1024ull * 1024ull)

static HwTexSlot g_texcache[HW_TEX_CACHE_N];
static int g_texbind[3] = { -1, -1, -1 };   /* Cache-Index je Einheit */
static unsigned long long g_tex_cache_bytes;
static uint32_t g_tex_clock;                /* LRU-Zeitstempel */
static ID3D11ShaderResourceView *g_tex_dummy;
static uint32_t g_list_serial;      /* hw_list_begin() */
static unsigned long long g_tex_upload_us;
static unsigned long long g_tex_partial;      /* Teil-Uploads (Diagnose) */
static unsigned long long g_tex_upload_key, g_tex_upload_hash;  /* Diagnose */
static int g_tex_diag = -1;         /* HLE_GTIME/HLE_HW_STATE (nur Diagnose) */

/* ---- M17: interne Aufloesung und Anzeige auf der GPU --------------------
 *
 * Gastsichtbar bleibt alles wie bisher: gerendert wird weiter in das native
 * Ziel (g_color_tex, Gastgroesse), und hw_flush_to_guest schreibt dessen
 * Inhalt ins Gast-VRAM. Zusaetzlich gehoert zu jedem Ziel eine
 * "Oberflaeche" (HwSurf) in interner Aufloesung (Faktor g_scale): bei
 * g_scale > 1 zeichnet pend_flush jeden Batch ein zweites Mal hinein
 * (Viewport/Scissor/Tiefe skaliert), bei g_scale == 1 ist sie eine
 * GPU-Kopie des nativen Ziels nach dem Rueckschreiben. Die Oberflaeche wird
 * nie zurueckgelesen; sie dient nur der Anzeige.
 *
 * Gueltigkeit: Jede Oberflaeche merkt sich einen Hash der Gastbytes von
 * Farb- und Tiefenpuffer zu dem Zeitpunkt, an dem GPU-Inhalt und Gast
 * uebereinstimmten (nach dem Laden bzw. Rueckschreiben). Beim naechsten
 * Laden desselben Ziels wird nur bei gleichem Hash weitergerendert, sonst
 * (CPU-/MemoryFill-/Transfer-/Software-Schreibzugriff) wird der neue
 * Gastinhalt auf die interne Aufloesung vergroessert.
 *
 * DisplayTransfer (hw_display_transfer): Liegt die Quelle als gueltige
 * Oberflaeche vor (Hash der Quellbytes passt), rechnet die GPU denselben
 * Transfer (Flip, 2x1/2x2-Mittelung, Formatquantisierung) in interner
 * Aufloesung in eine Anzeigeflaeche (HwDisp) zum Ziel-Framebuffer. Der
 * CPU-Transfer in den Gastspeicher laeuft unveraendert. Die Anzeige
 * (runtime/gpu/hw_present.c) nimmt die Anzeigeflaeche nur, wenn der Hash
 * der aktuellen Framebuffer-Bytes noch dem Stand nach dem Transfer
 * entspricht; sonst zeigt sie das CPU-Bild aus dem Gastspeicher.
 *
 * Sperre: Das Immediate-Context gehoert dem GX-Worker; die Anzeige laeuft im
 * Fenster-Thread. Alle oeffentlichen Einstiege, die den Context benutzen,
 * nehmen g_hw_cs (hw_lock). */
static CRITICAL_SECTION g_hw_cs;
static volatile LONG g_hw_cs_init;
/* Diagnose (HLE_HW_STATE): Wartezeit auf die Sperre, nur wenn sie belegt war. */
static volatile LONG64 g_lock_wait_us;
static unsigned long long hw_now_us(void);
static void hw_lock_enter(void)
{
    if (!g_hw_cs_init) return;
    if (!TryEnterCriticalSection(&g_hw_cs)) {
        unsigned long long t0 = hw_now_us();
        EnterCriticalSection(&g_hw_cs);
        InterlockedExchangeAdd64(&g_lock_wait_us, (LONG64)(hw_now_us() - t0));
    }
}
static void hw_lock_leave(void) { if (g_hw_cs_init) LeaveCriticalSection(&g_hw_cs); }

static int g_scale = 1;

/* Texturfilter der internen Aufloesung: zweite Variante des Pixelshaders
 * (Makro HW_TEXFILTER) plus Geometry-Shader fuer das Texelrechteck je
 * Dreieck; wird beim ersten internen Durchgang uebersetzt und nur dort
 * gebunden. g_filt_state: 0 noch nicht versucht, 1 bereit, -1 fehlgeschlagen. */
static int g_texfilter = HW_TEXFILTER_NONE;
static int g_filt_state;
static int g_filt_cb_mode = -1;
static ID3D11PixelShader *g_ps_filt;
static ID3D11GeometryShader *g_gs_filt;
static ID3D11Buffer *g_filt_cb;

typedef struct {
    int valid;
    unsigned char *color_mem, *depth_mem;
    uint32_t color_fmt, depth_fmt;
    int w, h;                                   /* native Groesse */
    ID3D11Texture2D *ctex;                      /* w*s x h*s RGBA8 */
    ID3D11RenderTargetView *crtv;
    ID3D11ShaderResourceView *csrv;
    ID3D11Texture2D *dtex;                      /* nur s > 1 */
    ID3D11DepthStencilView *dsv;
    uint64_t chash, dhash;
    uint32_t clen, dlen;
    int cvalid, dvalid;
    /* Nur Ziele, die schon einmal Quelle eines DisplayTransfers waren,
     * werden in interner Aufloesung mitgerendert (shown). Zwischenziele
     * (Render-to-Texture, z. B. die 3D-Figuren im Dialog) sampelt der
     * Renderer ohnehin nativ; der zweite Durchgang waere dort nur Last.
     * hr_skipped: seit dem Laden ohne zweiten Durchgang gezeichnet ->
     * interne Kopie veraltet. */
    int shown, hr_skipped;
    unsigned long long last_use;
} HwSurf;
#define HW_SURF_N 8
static HwSurf g_surf[HW_SURF_N];
static HwSurf *g_cur_surf;
static unsigned long long g_surf_clock;
/* Momentaufnahme fuer Shader-Blending in interner Aufloesung */
static ID3D11Texture2D *g_hr_shadow;
static ID3D11ShaderResourceView *g_hr_shadow_srv;
static int g_hr_shadow_w, g_hr_shadow_h;
/* Natives Ziel als SRV (nur s > 1: Vergroessern nach dem Laden) */
static ID3D11ShaderResourceView *g_color_srv, *g_depth_srv, *g_stencil_srv;
/* Native Bindungen des laufenden Draws (pend_flush wechselt bei s > 1) */
static D3D11_VIEWPORT g_vp_nat;
static D3D11_RECT g_rc_nat;
static int g_shadow_on;

typedef struct {
    int valid;
    const unsigned char *dst;                   /* Gast-Framebuffer */
    uint32_t len, fmt, ow, oh;                  /* Bytes, Format, Zeilenlaenge/Zeilen */
    ID3D11Texture2D *tex;                       /* ow*s x oh*s, Gast-Orientierung */
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    int tw, th;
    uint64_t hash;
    unsigned long long last_use;
} HwDisp;
#define HW_DISP_N 6
static HwDisp g_disp[HW_DISP_N];
static unsigned long long g_disp_gpu, g_disp_cpu;   /* Diagnose: GPU-Transfers / ohne Oberflaeche */

/* Hilfspipeline (Vergroessern, Transfer) */
static ID3D11VertexShader *g_fs_vs;
static ID3D11PixelShader *g_up_ps, *g_upd_ps, *g_ups_ps, *g_dt_ps;
static ID3D11Buffer *g_util_cb;
static ID3D11DepthStencilState *g_dss_upd, *g_dss_ups[8];
static ID3D11BlendState *g_bs_util;
static ID3D11RasterizerState *g_rs_util;

static uint64_t fnv1a(const unsigned char *p, uint32_t n);
static void surf_attach(const HwDrawState *st);
static void surf_after_flush(int depth_written);
static void surf_release_all(void);
static void hr_bind(int hires);
static int hr_active(void);
static int util_init(void);
static void util_release(void);
static void filt_release(void);
static void pend_flush(void);
/* Diagnose (HLE_GTIME): Wandzeit je Abschnitt des HW-Pfads. */
static unsigned long long g_dg_flush_us, g_dg_wait_us, g_dg_begin_us, g_dg_tri_us;
static unsigned long long g_dg_hash_us, g_dg_lists, g_dg_ovl, g_dg_trk;

static unsigned long long hw_now_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (unsigned long long)((double)now.QuadPart * 1000000.0 /
                                (double)freq.QuadPart);
}

static int tex_diag_on(void)
{
    if (g_tex_diag < 0) {
        const char *a = getenv("HLE_GTIME"), *b = getenv("HLE_HW_STATE");
        g_tex_diag = ((a && *a) || (b && *b)) ? 1 : 0;
    }
    return g_tex_diag;
}

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
void hw_texture_stats(unsigned long long *uploads, unsigned long long *decode_us) {
    if (uploads) *uploads = g_tex_uploads;
    if (decode_us) *decode_us = g_tex_upload_us;
}
unsigned long long hw_depth_writes(void) { return g_depth_writes; }
unsigned long long hw_texture_partial_uploads(void) { return g_tex_partial; }
void hw_submit_stats(unsigned long long *batches, unsigned long long *submits) {
    if (batches) *batches = g_batches;
    if (submits) *submits = g_submits;
}
unsigned long long hw_target_loads(void) { return g_target_loads; }
int hw_available(void) { return g_ready; }
static void hw_invalidate_guest_l(void) {
    g_guest_dirty = 1; g_rendered = 0; g_pend_n = 0; g_batch_ok = 0;
}
static void hw_invalidate_textures_l(void)
{
    int i;
    g_batch_ok = 0;
    for (i = 0; i < HW_TEX_CACHE_N; i++) g_texcache[i].stale = 1;
}
/* Nur Slots, deren Quellbytes den Bereich ueberlappen, muessen neu geprueft
 * werden (GX-Schreiboperationen). Offset/bytes beziehen sich wie
 * HwTexture.offset auf den Gastpuffer. */
static void hw_invalidate_texture_range_l(uint32_t offset, uint32_t bytes)
{
    int i;
    if (bytes == 0u) return;
    g_batch_ok = 0;
    for (i = 0; i < HW_TEX_CACHE_N; i++) {
        HwTexSlot *s = &g_texcache[i];
        if (!s->valid) continue;
        if ((uint64_t)offset < (uint64_t)s->offset + s->bytes &&
            (uint64_t)s->offset < (uint64_t)offset + bytes)
            s->stale = 1;
    }
}
static void hw_sync_guest_l(void)
{
    if (g_have_state && g_rendered && !g_guest_dirty) hw_flush_to_guest();
    /* Zielzustand verwerfen: der naechste HW-Draw laedt neu aus dem Gast. */
    g_pend_n = 0;
    g_batch_ok = 0;
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
    "cbuffer LightCB : register(b1) {\n"
    "  uint4  l_cfg;\n"
    "  float4 l_gal;\n"
    "  uint4  l_in;\n"
    "  float4 l_scl[2];\n"
    "  float4 l_pos[8];\n"
    "  float4 l_spot[8];\n"
    "  float4 l_spec0[8];\n"
    "  float4 l_spec1[8];\n"
    "  float4 l_diff[8];\n"
    "  float4 l_amb[8];\n"
    "  float4 l_dist[8];\n"
    "  uint4  l_flag[8];\n"
    "};\n"
    "Texture2D tex0 : register(t0);\n"
    "Texture2D tex1 : register(t1);\n"
    "Texture2D tex2 : register(t2);\n"
    "Texture2D tshadow : register(t3);\n"
    "Buffer<float2> g_lut : register(t4);\n"
    "struct VSIn { float4 pos : POSITION; float4 col : COLOR0;\n"
    "              float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float2 uv2 : TEXCOORD2;\n"
    "              float4 quat : TEXCOORD3; float4 vt : TEXCOORD4; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "               float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float2 uv2 : TEXCOORD2;\n"
    "               float4 quat : TEXCOORD3; float4 vt : TEXCOORD4; };\n"
    "struct PSOut { float4 col : SV_Target0; float4 dep : SV_Target1; float d : SV_Depth; };\n"
    "VSOut VSMain(VSIn i) {\n"
    "  VSOut o;\n"
    "  o.pos = float4(i.pos.x, -i.pos.y, -i.pos.z, i.pos.w);\n"
    "  o.col = i.col;\n"
    "  o.uv0 = i.uv0; o.uv1 = i.uv1; o.uv2 = i.uv2;\n"
    "  o.quat = i.quat; o.vt = i.vt;\n"
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
    /* Texelwahl (Nearest, wie p_sample_tex: (int)(u * w)) auf einem Raster
       von 1/4096 Texel: Liegt eine Pixelmitte exakt auf einer Texelgrenze
       (2:1-Verkleinerung der 3D-Zwischenbilder im Dialog), liefert die
       D3D-Interpolation der Grafikkarte knapp k - eps und damit ueberall den
       Nachbartexel (M9-Bildvergleich: ganze 3D-Ebene um einen Texel
       versetzt, Mund-/Augenlinien fehlten; tests/hw_selftest.c Szene X2,
       WARP interpoliert dort exakt). Der Software-Pfad trifft k meist, an
       einzelnen Pixeln mit float32-Baryzentren auch k - ulp.
       Das Raster ist feiner als die PICA-Texturkoordinaten (f24, 16 Bit
       Mantisse; Azahar interpoliert Attribute in f24) und groeber als der
       D3D-Interpolationsfehler; 1/16384 war gemessen zu fein. */
    "  int s = (int)trunc(round(uv.x * (float)w * 4096.0) / 4096.0);\n"
    "  int t = (int)trunc(round(uv.y * (float)h * 4096.0) / 4096.0);\n"
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
    /* Texturfilter fuer den Durchgang in interner Aufloesung (nur Anzeige,
       docs/FRONTEND.md "Texturfilter"). Alles zwischen #ifdef HW_TEXFILTER
       und #endif existiert nur in der zweiten Shader-Variante; ohne das
       Makro ergibt der Praeprozessor exakt den bisherigen Quelltext, der
       native Durchgang bleibt also bitgleich (gleicher Bytecode). */
    "#ifdef HW_TEXFILTER\n"
    "cbuffer FiltCB : register(b2) { uint4 f_cfg; };\n"
    "struct PSIn { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "              float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float2 uv2 : TEXCOORD2;\n"
    "              float4 quat : TEXCOORD3; float4 vt : TEXCOORD4;\n"
    "              nointerpolation int4 rc0 : TEXCOORD5; nointerpolation int4 rc1 : TEXCOORD6;\n"
    "              nointerpolation int4 rc2 : TEXCOORD7; nointerpolation uint rfl : TEXCOORD8; };\n"
    "float2 uv_of(VSOut v, uint u) { return (u == 0u) ? v.uv0 : ((u == 1u) ? v.uv1 : v.uv2); }\n"
    /* Je Dreieck und Einheit das gesampelte Texelrechteck (Atlas: Nachbar-
       Glyphen/-Sprites liegen ausserhalb). Ist das Dreieck im Texturraum
       ein rechtwinkliges mit achsparallelen Katheten (Haelfte eines
       Sprite-Quads: Flaeche = halbe Bounding-Box), klemmt der Filter seine
       Nachbartexel auf dieses Rechteck (rfl Bit 2u: s, Bit 2u+1: t);
       sonst (3D-Netze, Kacheln ueber die ganze Textur) gilt der Wrap-Modus. */
    "[maxvertexcount(3)]\n"
    "void GSMain(triangle VSOut v[3], inout TriangleStream<PSIn> os) {\n"
    "  int4 rc[3];\n"
    "  uint fl = 0u;\n"
    "  [unroll] for (uint u = 0u; u < 3u; u++) {\n"
    "    float2 sz = float2((float)k_tex0[u].x, (float)k_tex0[u].y);\n"
    "    float2 a = uv_of(v[0], u) * sz, b = uv_of(v[1], u) * sz, c = uv_of(v[2], u) * sz;\n"
    "    float2 mn = min(a, min(b, c)), mx = max(a, max(b, c));\n"
    "    float2 ex = mx - mn;\n"
    "    float ar = abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));\n"
    "    bool rect = ex.x > 0.0 && ex.y > 0.0 && abs(ar - ex.x * ex.y) <= 0.01 * ex.x * ex.y;\n"
    "    int2 lo = (int2)floor(mn + 1.0 / 256.0);\n"
    "    int2 hi = max((int2)ceil(mx - 1.0 / 256.0) - 1, lo);\n"
    "    rc[u] = int4(lo, hi);\n"
    "    if (rect && ex.x < sz.x) fl |= 1u << (2u * u);\n"
    "    if (rect && ex.y < sz.y) fl |= 2u << (2u * u);\n"
    "  }\n"
    "  [unroll] for (uint j = 0u; j < 3u; j++) {\n"
    "    PSIn o;\n"
    "    o.pos = v[j].pos; o.col = v[j].col;\n"
    "    o.uv0 = v[j].uv0; o.uv1 = v[j].uv1; o.uv2 = v[j].uv2;\n"
    "    o.quat = v[j].quat; o.vt = v[j].vt;\n"
    "    o.rc0 = rc[0]; o.rc1 = rc[1]; o.rc2 = rc[2]; o.rfl = fl;\n"
    "    os.Append(o);\n"
    "  }\n"
    "}\n"
    /* Texel (s, t) im PICA-Texturraum, vormultipliziert (rgb * a): so
       mischen transparente Texel mit beliebigem RGB keinen Farbsaum ein.
       Gepackt als RGBA8 in einem uint: der xBRZ-Kern haelt 25 Texel, als
       float4 waeren das 100 Register fuer den ganzen Shader (gemessen:
       weniger Wellen je Recheneinheit, auch wo der Filter nicht greift). */
    "uint ft_texp(uint unit, int s, int t, int w, int h, int4 rc, uint fl) {\n"
    "  if ((fl & 1u) != 0u) s = clamp(s, rc.x, rc.z);\n"
    "  if ((fl & 2u) != 0u) t = clamp(t, rc.y, rc.w);\n"
    "  s = clamp(wrap_coord(k_tex0[unit].z, s, w), 0, w - 1);\n"
    "  t = clamp(wrap_coord(k_tex0[unit].w, t, h), 0, h - 1);\n"
    "  uint4 c = fetch_tex(unit, s, h - 1 - t);\n"
    "  c.rgb = (c.rgb * c.a + 127u) / 255u;\n"
    "  return c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);\n"
    "}\n"
    "float4 ft_un(uint v) {\n"
    "  return float4(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24) / 255.0;\n"
    "}\n"
    "float4 ft_linear(uint unit, float2 p, int w, int h, int4 rc, uint fl) {\n"
    "  float2 f = p - 0.5;\n"
    "  float2 f0 = floor(f);\n"
    "  int2 i0 = (int2)f0;\n"
    "  float2 a = f - f0;\n"
    "  float4 c00 = ft_un(ft_texp(unit, i0.x, i0.y, w, h, rc, fl));\n"
    "  float4 c10 = ft_un(ft_texp(unit, i0.x + 1, i0.y, w, h, rc, fl));\n"
    "  float4 c01 = ft_un(ft_texp(unit, i0.x, i0.y + 1, w, h, rc, fl));\n"
    "  float4 c11 = ft_un(ft_texp(unit, i0.x + 1, i0.y + 1, w, h, rc, fl));\n"
    "  return lerp(lerp(c00, c10, a.x), lerp(c01, c11, a.x), a.y);\n"
    "}\n"
    /* xBRZ-artiger Kantenfilter mit freier Skalierung (eigene Umsetzung nach
       der veroeffentlichten Beschreibung von xBRZ): Farbabstand in YCbCr
       (+ Alpha), Eckentyp je 2x2-Block aus den Gradienten der beiden
       Diagonalen (keiner/normal/dominant, Schwelle 3,6), dann je Ecke des
       Texels eine Mischregion: Diagonale (x+y > 1,5), flache/steile Linie
       (Steigung 1:2 durch die Kantenmitte, Schwelle 2,2) oder runde Ecke.
       Die Region wird analytisch gegen die Position im Texel geprueft und
       ueber eine Ausgabepixelbreite (rho Texel) geglaettet. */
    "float ft_dist(uint pa, uint pb) {\n"
    "  float4 a = ft_un(pa), b = ft_un(pb);\n"
    "  float3 d = a.rgb - b.rgb;\n"
    "  float y = dot(d, float3(0.2627, 0.6780, 0.0593));\n"
    "  float cb = 0.5 / (1.0 - 0.0593) * (d.b - y);\n"
    "  float cr = 0.5 / (1.0 - 0.2627) * (d.r - y);\n"
    "  return sqrt(y * y + cb * cb + cr * cr) + abs(a.a - b.a);\n"
    "}\n"
    "bool ft_eq(uint a, uint b) { return ft_dist(a, b) < 30.0 / 255.0; }\n"
    "bool ft_same(uint a, uint b) { return a == b; }\n"
    /* 2x2-Block f g / j k im 4x4-Kern a..p; Ergebnis (f, g, j, k):
       0 keine, 1 normale, 2 dominante Mischung an der gemeinsamen Ecke. */
    "uint4 ft_pre(uint b, uint c, uint e, uint f, uint g, uint h,\n"
    "             uint i, uint j, uint k, uint l, uint n, uint o) {\n"
    "  uint4 r = uint4(0u, 0u, 0u, 0u);\n"
    "  if ((ft_same(f, g) && ft_same(j, k)) || (ft_same(f, j) && ft_same(g, k))) return r;\n"
    "  float jg = ft_dist(i, f) + ft_dist(f, c) + ft_dist(n, k) + ft_dist(k, h) + 4.0 * ft_dist(j, g);\n"
    "  float fk = ft_dist(e, j) + ft_dist(j, o) + ft_dist(b, g) + ft_dist(g, l) + 4.0 * ft_dist(f, k);\n"
    "  if (jg < fk) {\n"
    "    uint t = (3.6 * jg < fk) ? 2u : 1u;\n"
    "    if (!ft_same(f, g) && !ft_same(f, j)) r.x = t;\n"
    "    if (!ft_same(k, j) && !ft_same(k, g)) r.w = t;\n"
    "  } else if (fk < jg) {\n"
    "    uint t = (3.6 * fk < jg) ? 2u : 1u;\n"
    "    if (!ft_same(j, f) && !ft_same(j, k)) r.z = t;\n"
    "    if (!ft_same(g, f) && !ft_same(g, k)) r.y = t;\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "#define FK(X, Y) K[((Y) + 2) * 5 + (X) + 2]\n"
    "#define FT_BLOCK(X, Y) ft_pre(FK(X + 1, Y), FK(X + 2, Y), FK(X, Y + 1), FK(X + 1, Y + 1),\\\n"
    "  FK(X + 2, Y + 1), FK(X + 3, Y + 1), FK(X, Y + 2), FK(X + 1, Y + 2), FK(X + 2, Y + 2),\\\n"
    "  FK(X + 3, Y + 2), FK(X + 1, Y + 3), FK(X + 2, Y + 3))\n"
    /* Drehung r (0..3, je 90 Grad) eines Versatzes aus dem gedrehten Rahmen
       in den Originalrahmen; Ecke (1,1) des gedrehten Rahmens ist die
       bearbeitete Ecke. */
    "int2 ft_rot(uint r, int2 p) {\n"
    "  if (r == 1u) return int2(-p.y, p.x);\n"
    "  if (r == 2u) return int2(-p.x, -p.y);\n"
    "  if (r == 3u) return int2(p.y, -p.x);\n"
    "  return p;\n"
    "}\n"
    "float2 ft_unrot(uint r, float2 q) {\n"
    "  if (r == 1u) return float2(q.y, -q.x);\n"
    "  if (r == 2u) return float2(-q.x, -q.y);\n"
    "  if (r == 3u) return float2(-q.y, q.x);\n"
    "  return q;\n"
    "}\n"
    /* ct = Eckentypen des Mitteltexels (x: +s+t, y: -s+t, z: +s-t, w: -s-t) */
    "uint ft_ct(int2 c, uint4 ct) {\n"
    "  if (c.x > 0) return (c.y > 0) ? ct.x : ct.z;\n"
    "  return (c.y > 0) ? ct.y : ct.w;\n"
    "}\n"
    /* Kernindex des gedrehten Versatzes (x, y); als Makro, damit kein
       Array kopiert wird (ein Funktionsparameter float4 K[25] kostete je
       Aufruf eine Kopie im lokalen Speicher, gemessen: Titel 75 -> 27
       Host-fps im deterministischen Lauf). */
    "#define FKR(R, X, Y) K[ft_ki(R, X, Y)]\n"
    "int ft_ki(uint r, int x, int y) {\n"
    "  int2 o = ft_rot(r, int2(x, y));\n"
    "  return (o.y + 2) * 5 + o.x + 2;\n"
    "}\n"
    "float4 ft_xbrz(uint unit, float2 p, float rho, int w, int h, int4 rc, uint fl) {\n"
    "  float2 p0 = floor(p);\n"
    "  int2 c0 = (int2)p0;\n"
    "  float2 fr = p - p0;\n"
    "  uint K[25];\n"
    /* Erst die 3x3-Nachbarschaft: ist sie einfarbig, gibt es an keiner
       Ecke des Mitteltexels eine Mischung (jeder 2x2-Block besteht aus
       gleichen Texeln) -> Texel unveraendert, ohne die aeusseren Texel. */
    "  bool flat = true;\n"
    "  [unroll] for (int y3 = -1; y3 <= 1; y3++)\n"
    "    [unroll] for (int x3 = -1; x3 <= 1; x3++)\n"
    "      FK(x3, y3) = ft_texp(unit, c0.x + x3, c0.y + y3, w, h, rc, fl);\n"
    "  [unroll] for (int k3 = 0; k3 < 9; k3++)\n"
    "    flat = flat && ft_same(K[(k3 / 3 + 1) * 5 + k3 % 3 + 1], FK(0, 0));\n"
    "  [branch] if (flat) return ft_un(FK(0, 0));\n"
    "  [unroll] for (int k = 0; k < 25; k++) {\n"
    "    int x = k % 5 - 2, y = k / 5 - 2;\n"
    "    if (abs(x) == 2 || abs(y) == 2)\n"
    "      K[k] = (abs(x) == 2 && abs(y) == 2) ? 0u\n"
    "             : ft_texp(unit, c0.x + x, c0.y + y, w, h, rc, fl);\n"
    "  }\n"
    "  uint4 ct = uint4(FT_BLOCK(-1, -1).x, FT_BLOCK(-2, -1).y,\n"
    "                   FT_BLOCK(-1, -2).z, FT_BLOCK(-2, -2).w);\n"
    "  uint e = FK(0, 0);\n"
    "  float4 res = ft_un(e);\n"
    "  [branch] if (all(ct == 0u)) return res;\n"
    "  float aa = 1.0 / max(rho, 1.0 / 64.0);\n"
    "  [unroll] for (uint r = 0u; r < 4u; r++) {\n"
    "    uint br = ft_ct(ft_rot(r, int2(1, 1)), ct);\n"
    "    if (br == 0u) continue;\n"
    "    uint tr = ft_ct(ft_rot(r, int2(1, -1)), ct);\n"
    "    uint bl = ft_ct(ft_rot(r, int2(-1, 1)), ct);\n"
    "    uint b = FKR(r, 0, -1), c = FKR(r, 1, -1), d = FKR(r, -1, 0);\n"
    "    uint f = FKR(r, 1, 0), g = FKR(r, -1, 1), hh = FKR(r, 0, 1);\n"
    "    uint i = FKR(r, 1, 1);\n"
    "    bool lb = true;\n"
    "    if (br >= 2u) lb = true;\n"
    "    else if (tr != 0u && !ft_eq(e, g)) lb = false;\n"
    "    else if (bl != 0u && !ft_eq(e, c)) lb = false;\n"
    "    else if (!ft_eq(e, i) && ft_eq(g, hh) && ft_eq(hh, i) && ft_eq(i, f) && ft_eq(f, c)) lb = false;\n"
    "    uint px = (ft_dist(e, f) <= ft_dist(e, hh)) ? f : hh;\n"
    "    float2 q = ft_unrot(r, fr - 0.5) + 0.5;\n"
    "    float sd;\n"
    "    if (lb) {\n"
    "      float fg = ft_dist(f, g), hc = ft_dist(hh, c);\n"
    "      bool shallow = 2.2 * fg <= hc && !ft_same(e, g) && !ft_same(d, g);\n"
    "      bool steep = 2.2 * hc <= fg && !ft_same(e, c) && !ft_same(b, c);\n"
    "      float s1 = (0.5 * q.x + q.y - 1.0) * 0.8944272;\n"
    "      float s2 = (q.x + 0.5 * q.y - 1.0) * 0.8944272;\n"
    "      if (shallow && steep) sd = max(s1, s2);\n"
    "      else if (shallow) sd = s1;\n"
    "      else if (steep) sd = s2;\n"
    "      else sd = (q.x + q.y - 1.5) * 0.7071068;\n"
    "    } else {\n"
    "      sd = min(length(q - 0.5) - 0.5, min(q.x, q.y) - 0.5);\n"
    "    }\n"
    "    res = lerp(res, ft_un(px), saturate(0.5 + sd * aa));\n"
    "  }\n"
    "  return res;\n"
    "}\n"
    /* Gefiltertes Sampling: nur 2D-Texturen, nur wenn ein Texel groesser
       als ein Ausgabepixel ist (Vergroesserung, rho < 1). Filter nach
       f_cfg.x (1 linear, 2 xbrz). Ausnahme: vergroessert das Spiel die
       Textur schon in nativer Aufloesung (rho * Skala < 1) und verlangt
       dafuer lineare Filterung (0x83 Bit 1), wird bilinear gefiltert wie
       auf der Konsole; was nativ 1:1 erscheint (Schrift, 2D-Grafik), bekommt
       den gewaehlten Filter. Randfarbe (ClampToBorder ausserhalb) bleibt
       wie nativ. f_cfg.y = Skala. */
    "uint4 sample_filt(uint unit, float2 uv, float2 dx, float2 dy, int4 rc, uint rfl) {\n"
    "  uint4 c0 = k_tex0[unit];\n"
    "  uint4 c1 = k_tex1[unit];\n"
    "  uint4 c2 = k_tex2[unit];\n"
    "  int w = (int)c0.x, h = (int)c0.y;\n"
    "  float2 sz = float2((float)max(w, 1), (float)max(h, 1));\n"
    "  float rho = max(length(dx * sz), length(dy * sz));\n"
    "  float2 p = uv * sz;\n"
    "  bool bs = (c0.z == 1u) ? (p.x < 0.0 || p.x >= sz.x) : (c0.z == 5u && p.x >= sz.x);\n"
    "  bool bt = (c0.w == 1u) ? (p.y < 0.0 || p.y >= sz.y) : (c0.w == 5u && p.y >= sz.y);\n"
    "  bool use = f_cfg.x != 0u && c1.y != 0u && c1.w == 0u && c2.x != 0u &&\n"
    "             w > 0 && h > 0 && rho < 1.0 && !bs && !bt;\n"
    /* [branch] statt ?: bzw. abgeflachter Verzweigung: HLSL wertet beim ?:
       beide Seiten aus (linear kostete sonst so viel wie xbrz). */
    "  [branch] if (!use) return sample_unit(unit, uv);\n"
    "  uint fl = (rfl >> (2u * unit)) & 3u;\n"
    "  float4 r;\n"
    "  [branch] if ((c2.y != 0u && rho * (float)f_cfg.y < 0.99) || f_cfg.x == 1u)\n"
    "    r = ft_linear(unit, p, w, h, rc, fl);\n"
    "  else r = ft_xbrz(unit, p, rho, w, h, rc, fl);\n"
    "  float3 rgb = (r.a > 0.0) ? r.rgb / r.a : float3(0.0, 0.0, 0.0);\n"
    "  return (uint4)round(saturate(float4(rgb, r.a)) * 255.0);\n"
    "}\n"
    /* Alle drei Einheiten in einer Schleife (der Filter steht so nur einmal
       im Bytecode; dreifach eingebettet dauerte das Uebersetzen doppelt so
       lang). Ableitungen vorher, ausserhalb jeder Verzweigung. */
    "void ft_all(PSIn i, out uint4 r[3]) {\n"
    "  float2 uv[3] = { i.uv0, i.uv1, i.uv2 };\n"
    "  float2 dx[3] = { ddx(i.uv0), ddx(i.uv1), ddx(i.uv2) };\n"
    "  float2 dy[3] = { ddy(i.uv0), ddy(i.uv1), ddy(i.uv2) };\n"
    "  int4 rc[3] = { i.rc0, i.rc1, i.rc2 };\n"
    "  [loop] for (uint u = 0u; u < 3u; u++)\n"
    "    r[u] = sample_filt(u, uv[u], dx[u], dy[u], rc[u], i.rfl);\n"
    "}\n"
    "#define PS_IN PSIn\n"
    "#define FT_PREPARE uint4 ftr[3]; ft_all(i, ftr);\n"
    "#define SAMPLE_UNIT(U, UV, RC) ftr[U]\n"
    "#else\n"
    "#define PS_IN VSOut\n"
    "#define FT_PREPARE\n"
    "#define SAMPLE_UNIT(U, UV, RC) sample_unit(U, UV)\n"
    "#endif\n"
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
    "/* ---- Fragment-Lighting: p_compute_fragments (gpu.c) Schritt fuer Schritt */\n"
    "float lut_get(uint l, uint idx, float delta) {\n"
    "  float2 e = g_lut.Load((int)(l * 256u + (idx & 255u)));\n"
    "  return e.x + e.y * delta;\n"
    "}\n"
    "float3 safe_norm(float3 v) {\n"
    "  float l = sqrt(dot(v, v));\n"
    "  return (l != 0.0) ? v / l : v;\n"
    "}\n"
    "float3 qrot(float4 q, float3 v) {\n"
    "  float3 t = cross(q.xyz, v) + v * q.w;\n"
    "  return v + 2.0 * cross(q.xyz, t);\n"
    "}\n"
    "uint4 pick_tex(uint s, uint4 t0, uint4 t1, uint4 t2) {\n"
    "  if (s == 0u) return t0;\n"
    "  if (s == 1u) return t1;\n"
    "  if (s == 2u) return t2;\n"
    "  return uint4(0, 0, 0, 0);\n"
    "}\n"
    "float lut_sample(uint slot, uint lut, uint lcfg, float3 N, float3 H, float3 V,\n"
    "                 float3 L, float3 S, float3 T) {\n"
    "  uint inp = (l_in.x >> (4u * slot)) & 7u;\n"
    "  float r = 0.0;\n"
    "  if (inp == 0u) r = dot(N, safe_norm(H));\n"
    "  else if (inp == 1u) r = dot(V, safe_norm(H));\n"
    "  else if (inp == 2u) r = dot(N, V);\n"
    "  else if (inp == 3u) r = dot(L, N);\n"
    "  else if (inp == 4u) r = dot(L, S);\n"
    "  else if (inp == 5u && l_cfg.z == 8u) {\n"
    "    float3 nh = safe_norm(H);\n"
    "    float3 pr = nh - N * dot(N, nh);\n"
    "    r = dot(pr, T);\n"
    "  }\n"
    "  uint idx;\n"
    "  float delta;\n"
    "  if (((l_in.y >> slot) & 1u) != 0u) {\n"
    "    if ((lcfg & 2u) != 0u) r = abs(r);\n"
    "    else if (r < 0.0) r = 0.0;\n"
    "    float f = clamp(floor(r * 256.0), 0.0, 255.0);\n"
    "    idx = (uint)f;\n"
    "    delta = r * 256.0 - f;\n"
    "  } else {\n"
    "    float f = clamp(floor(r * 128.0), -128.0, 127.0);\n"
    "    int si = (int)f;\n"
    "    delta = r * 128.0 - f;\n"
    "    idx = ((uint)si) & 255u;\n"
    "  }\n"
    "  return l_scl[slot >> 2][slot & 3u] * lut_get(lut, idx, delta);\n"
    "}\n"
    "void light_frag(float4 quat_in, float3 view, uint4 t0, uint4 t1, uint4 t2,\n"
    "                out uint4 pfc, out uint4 sfc) {\n"
    "  uint c0 = l_cfg.x;\n"
    "  float4 q = quat_in;\n"
    "  float ql = sqrt(dot(q, q));\n"
    "  if (ql != 0.0) q = q / ql;\n"
    "  float4 shadow = float4(1.0, 1.0, 1.0, 1.0);\n"
    "  if ((c0 & 1u) != 0u) {\n"
    "    shadow = (float4)pick_tex((c0 >> 24) & 3u, t0, t1, t2) / 255.0;\n"
    "    if ((c0 & (1u << 18)) != 0u) shadow = 1.0 - shadow;\n"
    "  }\n"
    "  float3 sn = float3(0.0, 0.0, 0.0), st = float3(0.0, 0.0, 0.0);\n"
    "  uint bump = (c0 >> 28) & 3u;\n"
    "  if (bump != 0u) {\n"
    "    float3 pert = (float3)pick_tex((c0 >> 22) & 3u, t0, t1, t2).rgb / 127.5 - 1.0;\n"
    "    if (bump == 1u) {\n"
    "      if ((c0 & (1u << 30)) == 0u) {\n"
    "        float z2 = 1.0 - (pert.x * pert.x + pert.y * pert.y);\n"
    "        pert.z = sqrt(z2 > 0.0 ? z2 : 0.0);\n"
    "      }\n"
    "      sn = pert;\n"
    "      st = float3(1.0, 0.0, 0.0);\n"
    "    } else if (bump == 2u) {\n"
    "      sn = float3(0.0, 0.0, 1.0);\n"
    "      st = pert;\n"
    "    }\n"
    "  } else {\n"
    "    sn = float3(0.0, 0.0, 1.0);\n"
    "    st = float3(1.0, 0.0, 0.0);\n"
    "  }\n"
    "  float3 N = qrot(q, sn);\n"
    "  float3 T = qrot(q, st);\n"
    "  float3 V = safe_norm(view);\n"
    "  float4 dsum = float4(0.0, 0.0, 0.0, 1.0);\n"
    "  float4 ssum = float4(0.0, 0.0, 0.0, 1.0);\n"
    "  [loop] for (uint li = 0u; li < l_cfg.w; li++) {\n"
    "    uint lcfg = l_flag[li].x, num = l_flag[li].y, en = l_flag[li].z;\n"
    "    float3 lv = ((lcfg & 1u) != 0u) ? l_pos[li].xyz : l_pos[li].xyz + view;\n"
    "    float len = sqrt(dot(lv, lv));\n"
    "    if (len != 0.0) lv = lv / len;\n"
    "    float3 hv = V + lv;\n"
    "    float3 S = l_spot[li].xyz;\n"
    "    float dist = 1.0, spot = 1.0, d0 = 1.0, d1 = 1.0;\n"
    "    if ((en & 1u) != 0u) {\n"
    "      float sl = saturate(l_dist[li].x * len + l_dist[li].y);\n"
    "      float f = clamp(floor(sl * 256.0), 0.0, 255.0);\n"
    "      dist = lut_get(16u + num, (uint)f, sl * 256.0 - f);\n"
    "    }\n"
    "    if ((en & 2u) != 0u) spot = lut_sample(0u, 8u + num, lcfg, N, hv, V, lv, S, T);\n"
    "    if ((en & 4u) != 0u) d0 = lut_sample(1u, 0u, lcfg, N, hv, V, lv, S, T);\n"
    "    float3 s0 = d0 * l_spec0[li].rgb;\n"
    "    float3 refl;\n"
    "    refl.r = ((en & 128u) != 0u) ? lut_sample(4u, 6u, lcfg, N, hv, V, lv, S, T) : 1.0;\n"
    "    refl.g = ((en & 256u) != 0u) ? lut_sample(5u, 5u, lcfg, N, hv, V, lv, S, T) : refl.r;\n"
    "    refl.b = ((en & 512u) != 0u) ? lut_sample(6u, 4u, lcfg, N, hv, V, lv, S, T) : refl.r;\n"
    "    if ((en & 8u) != 0u) d1 = lut_sample(2u, 1u, lcfg, N, hv, V, lv, S, T);\n"
    "    float3 s1 = d1 * refl * l_spec1[li].rgb;\n"
    "    if ((en & 16u) != 0u) {\n"
    "      float fr = lut_sample(3u, 3u, lcfg, N, hv, V, lv, S, T);\n"
    "      if ((c0 & 4u) != 0u) dsum.a = fr;\n"
    "      if ((c0 & 8u) != 0u) ssum.a = fr;\n"
    "    }\n"
    "    float dp = dot(lv, N);\n"
    "    if ((lcfg & 2u) != 0u) dp = abs(dp);\n"
    "    else if (dp < 0.0) dp = 0.0;\n"
    "    float clampf = 1.0;\n"
    "    if ((c0 & (1u << 27)) != 0u) clampf = (dp == 0.0) ? 0.0 : 1.0;\n"
    "    if ((lcfg & 0xCu) != 0u) {\n"
    "      float g = dot(hv, hv);\n"
    "      g = (g == 0.0) ? 0.0 : min(dp / g, 1.0);\n"
    "      if ((lcfg & 4u) != 0u) s0 = s0 * g;\n"
    "      if ((lcfg & 8u) != 0u) s1 = s1 * g;\n"
    "    }\n"
    "    float3 sp = ((en & 32u) != 0u) ? shadow.rgb : float3(1.0, 1.0, 1.0);\n"
    "    float3 ss = ((en & 64u) != 0u) ? shadow.rgb : float3(1.0, 1.0, 1.0);\n"
    "    dsum.rgb += (l_diff[li].rgb * dp * sp + l_amb[li].rgb) * dist * spot;\n"
    "    ssum.rgb += (s0 + s1) * clampf * dist * spot * ss;\n"
    "  }\n"
    "  if ((c0 & (1u << 19)) != 0u) {\n"
    "    if ((c0 & 4u) != 0u) dsum.a *= shadow.a;\n"
    "    if ((c0 & 8u) != 0u) ssum.a *= shadow.a;\n"
    "  }\n"
    "  dsum.rgb += l_gal.rgb;\n"
    "  pfc = (uint4)(saturate(dsum) * 255.0);\n"
    "  sfc = (uint4)(saturate(ssum) * 255.0);\n"
    "}\n"
    "PSOut PSMain(PS_IN i) {\n"
    "  PSOut o;\n"
    "  uint4 prim = (uint4)round(saturate(i.col) * 255.0);\n"
    "  uint4 zero = uint4(0, 0, 0, 0);\n"
    "  uint4 pfc = zero, sfc = zero;\n"
    "  FT_PREPARE\n"
    "  uint4 t0 = SAMPLE_UNIT(0, i.uv0, rc0);\n"
    "  uint4 t1 = SAMPLE_UNIT(1, i.uv1, rc1);\n"
    "  uint4 t2 = SAMPLE_UNIT(2, i.uv2, rc2);\n"
    "  if (l_cfg.w != 0u) light_frag(i.quat, i.vt.xyz, t0, t1, t2, pfc, sfc);\n"
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
    "  float dq = (k_param.z > 0.0) ? floor(d * k_param.z) / k_param2.z : d;\n"
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
    "  o.dep = float4(dq, dq, dq, 1.0);\n"
    "  o.d = dq;\n"
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
            /* Wie p_depth_set: D16 = 2 Byte, D24/D24S8 = 3 Byte. */
            st->depth_mem[off] = (unsigned char)(z & 0xFFu);
            st->depth_mem[off + 1u] = (unsigned char)((z >> 8) & 0xFFu);
            if (st->depth_format != 0u)
                st->depth_mem[off + 2u] = (unsigned char)((z >> 16) & 0xFFu);
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
    if (g_color_srv) { ID3D11ShaderResourceView_Release(g_color_srv); g_color_srv = NULL; }
    if (g_depth_srv) { ID3D11ShaderResourceView_Release(g_depth_srv); g_depth_srv = NULL; }
    if (g_stencil_srv) { ID3D11ShaderResourceView_Release(g_stencil_srv); g_stencil_srv = NULL; }
    g_cur_surf = NULL;
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
    /* M17: bei interner Aufloesung > 1 wird das native Ziel nach dem Laden
       vergroessert (surf_attach) und braucht dafuer eine SRV. */
    if (g_scale > 1) td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_color_tex))) return 0;
    memset(&rd, 0, sizeof(rd));
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_color_tex,
                                                   &rd, &g_color_rtv))) return 0;
    if (g_scale > 1 && FAILED(ID3D11Device_CreateShaderResourceView(
            g_dev, (ID3D11Resource *)g_color_tex, NULL, &g_color_srv))) return 0;

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
    if (g_scale > 1) {
        /* typenlos, damit surf_upscale_depth Tiefe/Stencil lesen kann */
        td.Format = (st->depth_format == 0u) ? DXGI_FORMAT_R16_TYPELESS :
                    (st->depth_format == 3u) ? DXGI_FORMAT_R24G8_TYPELESS :
                                               DXGI_FORMAT_R32_TYPELESS;
        td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_depth_dsv_tex))) return 0;
    td.Format = dfmt;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (g_scale > 1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv;
        memset(&sv, 0, sizeof(sv));
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        sv.Format = (st->depth_format == 0u) ? DXGI_FORMAT_R16_UNORM :
                    (st->depth_format == 3u) ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS :
                                               DXGI_FORMAT_R32_FLOAT;
        if (FAILED(ID3D11Device_CreateShaderResourceView(
                g_dev, (ID3D11Resource *)g_depth_dsv_tex, &sv, &g_depth_srv))) return 0;
        if (st->depth_format == 3u) {
            sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            if (FAILED(ID3D11Device_CreateShaderResourceView(
                    g_dev, (ID3D11Resource *)g_depth_dsv_tex, &sv, &g_stencil_srv))) return 0;
        }
    }
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
    surf_attach(st);
    return 1;
}

static int init_pipeline(void) {
    D3D11_INPUT_ELEMENT_DESC elems[7];
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
    elems[5].SemanticName = "TEXCOORD"; elems[5].SemanticIndex = 3;
    elems[5].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    elems[5].AlignedByteOffset = 56;           /* quat */
    elems[6].SemanticName = "TEXCOORD"; elems[6].SemanticIndex = 4;
    elems[6].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    elems[6].AlignedByteOffset = 72;           /* view.xyz, tc0_w */
    elems[2].InputSlotClass = elems[3].InputSlotClass = elems[4].InputSlotClass =
        elems[5].InputSlotClass = elems[6].InputSlotClass =
        D3D11_INPUT_PER_VERTEX_DATA;
    if (FAILED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb),
                                               ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs))) {
        ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb); return 0;
    }
    if (FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb),
                                              ID3D10Blob_GetBufferSize(psb), NULL, &g_ps))) {
        ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb); return 0;
    }
    if (FAILED(ID3D11Device_CreateInputLayout(g_dev, elems, 7,
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

    /* Dynamischer Vertex-Ringpuffer fuer die gebuendelten Dreiecke. */
    {
        D3D11_BUFFER_DESC vd;
        memset(&vd, 0, sizeof(vd));
        vd.ByteWidth = (UINT)(sizeof(HwVertex) * (size_t)HW_VB_CAP);
        vd.Usage = D3D11_USAGE_DYNAMIC;
        vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &vd, NULL, &g_vb))) return 0;
        g_vb_pos = 0;
    }

    /* Lighting-Constant-Buffer (LightCB, b1) */
    bd.ByteWidth = (UINT)sizeof(LightCbData);
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_lcb))) return 0;

    /* Lighting-LUTs: 24 x 256 float2 (Wert, Differenz) */
    {
        D3D11_BUFFER_DESC lb;
        D3D11_SHADER_RESOURCE_VIEW_DESC lv;
        memset(&lb, 0, sizeof(lb));
        lb.ByteWidth = (UINT)sizeof(g_lut_cpu);
        lb.Usage = D3D11_USAGE_DEFAULT;
        lb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &lb, NULL, &g_lut_buf))) return 0;
        memset(&lv, 0, sizeof(lv));
        lv.Format = DXGI_FORMAT_R32G32_FLOAT;
        lv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        lv.Buffer.FirstElement = 0;
        lv.Buffer.NumElements = 24u * 256u;
        if (FAILED(ID3D11Device_CreateShaderResourceView(
                g_dev, (ID3D11Resource *)g_lut_buf, &lv, &g_lut_srv)))
            return 0;
        g_lut_valid = 0;
    }

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
    /* M17: Hilfspipeline (Vergroessern, GPU-DisplayTransfer). Ohne sie
       laeuft der Renderer wie bisher (Anzeige ueber das CPU-Bild). */
    if (!util_init()) {
        printf("[hw] Hilfspipeline nicht verfuegbar, interne Aufloesung 1\n");
        util_release();
        g_scale = 1;
    }
    return 1;
}

int hw_init(const char *driver) {
    D3D11_FEATURE_DATA_THREADING dummy;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr;
    (void)dummy;
    if (InterlockedCompareExchange(&g_hw_cs_init, 2, 0) == 0) {
        InitializeCriticalSection(&g_hw_cs);
        g_hw_cs_init = 1;
    }
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
    for (i = 0; i < HW_TEX_CACHE_N; i++) {
        if (g_texcache[i].srv) { ID3D11ShaderResourceView_Release(g_texcache[i].srv); g_texcache[i].srv = NULL; }
        if (g_texcache[i].tex) { ID3D11Texture2D_Release(g_texcache[i].tex); g_texcache[i].tex = NULL; }
        memset(&g_texcache[i], 0, sizeof(g_texcache[i]));
    }
    g_tex_cache_bytes = 0;
    g_texbind[0] = g_texbind[1] = g_texbind[2] = -1;
    if (g_tex_dummy) { ID3D11ShaderResourceView_Release(g_tex_dummy); g_tex_dummy = NULL; }
    release_targets();
    surf_release_all();
    util_release();
    filt_release();
    if (g_bs) { ID3D11BlendState_Release(g_bs); g_bs = NULL; }
    if (g_dss) { ID3D11DepthStencilState_Release(g_dss); g_dss = NULL; }
    if (g_rs) { ID3D11RasterizerState_Release(g_rs); g_rs = NULL; }
    if (g_cb) { ID3D11Buffer_Release(g_cb); g_cb = NULL; }
    if (g_lcb) { ID3D11Buffer_Release(g_lcb); g_lcb = NULL; }
    if (g_vb) { ID3D11Buffer_Release(g_vb); g_vb = NULL; }
    free(g_cov); g_cov = NULL; g_cov_w = g_cov_h = 0;
    g_pend_n = 0;
    g_batch_ok = 0;
    if (g_lut_srv) { ID3D11ShaderResourceView_Release(g_lut_srv); g_lut_srv = NULL; }
    if (g_lut_buf) { ID3D11Buffer_Release(g_lut_buf); g_lut_buf = NULL; }
    g_lut_valid = 0;
    g_lcb_valid = 0;
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

/* LUT-Zeilen nur bei geaenderter Generation pruefen und nur geaenderte
 * Zeilen (2 KB) hochladen: der Titel laedt LUTs oft je Material neu. */
static void light_upload_luts(const HwLighting *hl)
{
    static float row[256][2];
    uint32_t l, i;
    if (hl->lut_val == NULL || hl->lut_diff == NULL) return;
    if (g_lut_valid && hl->lut_gen == g_lut_gen_seen) return;
    for (l = 0; l < 24u; l++) {
        /* Nur LUTs mit neuen Schreibzugriffen pruefen (der Titel laedt
           LUTs je Material neu, meist mit demselben Inhalt). */
        if (g_lut_valid && hl->lut_rowgen != NULL &&
            hl->lut_rowgen[l] == g_lut_rowgen_seen[l])
            continue;
        if (hl->lut_rowgen != NULL) g_lut_rowgen_seen[l] = hl->lut_rowgen[l];
        for (i = 0; i < 256u; i++) {
            row[i][0] = hl->lut_val[l * 256u + i];
            row[i][1] = hl->lut_diff[l * 256u + i];
        }
        if (!g_lut_valid || memcmp(row, g_lut_cpu[l], sizeof(row)) != 0) {
            D3D11_BOX box;
            memcpy(g_lut_cpu[l], row, sizeof(row));
            box.left = (UINT)(l * sizeof(row));
            box.right = (UINT)((l + 1u) * sizeof(row));
            box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_lut_buf,
                                                  0, &box, row, 0, 0);
        }
    }
    g_lut_valid = 1;
    g_lut_gen_seen = hl->lut_gen;
}

static void set_light_constants(const HwDrawState *st)
{
    LightCbData cb;
    D3D11_MAPPED_SUBRESOURCE map;
    const HwLighting *hl = &st->light;
    uint32_t i;

    memset(&cb, 0, sizeof(cb));
    if (hl->enabled) {
        light_upload_luts(hl);
        cb.cfg[0] = hl->config0;
        cb.cfg[1] = hl->config1;
        cb.cfg[2] = hl->config;
        cb.cfg[3] = hl->nlights > 8u ? 8u : hl->nlights;
        cb.gal[0] = hl->gal[0]; cb.gal[1] = hl->gal[1]; cb.gal[2] = hl->gal[2];
        cb.in[0] = hl->inputs;
        cb.in[1] = hl->abs_bits;
        for (i = 0; i < 7u; i++) cb.scl[i] = hl->scale[i];
        for (i = 0; i < cb.cfg[3]; i++) {
            const HwLight *h = &hl->light[i];
            memcpy(cb.pos[i], h->position, 3 * sizeof(float));
            memcpy(cb.spot[i], h->spot_dir, 3 * sizeof(float));
            memcpy(cb.spec0[i], h->spec0, 3 * sizeof(float));
            memcpy(cb.spec1[i], h->spec1, 3 * sizeof(float));
            memcpy(cb.diff[i], h->diffuse, 3 * sizeof(float));
            memcpy(cb.amb[i], h->ambient, 3 * sizeof(float));
            cb.dist[i][0] = h->dist_scale;
            cb.dist[i][1] = h->dist_bias;
            cb.flag[i][0] = h->lcfg;
            cb.flag[i][1] = h->num & 7u;
            cb.flag[i][2] = h->enables;
        }
    }
    if (!g_lcb_valid || memcmp(&cb, &g_lcb_last, sizeof(cb)) != 0) {
        if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_lcb, 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &map)))
            return;
        memcpy(map.pData, &cb, sizeof(cb));
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_lcb, 0);
        g_lcb_last = cb;
        g_lcb_valid = 1;
    }
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 1, 1, &g_lcb);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 4, 1, &g_lut_srv);
}

/* Shader-Blending ist noetig, wenn D3D die PICA-Semantik nicht nativ
 * abbildet:
 *  - Min/Max werten die Faktoren nicht aus,
 *  - jeder Faktor ausser Zero/One: die PICA (Azahar sw_framebuffer.cpp
 *    EvaluateBlendEquation, p_blend_eq) rechnet ganzzahlig
 *    (s*sf + d*df) / 255 und schneidet ab, das D3D-Festfunktions-Blending
 *    rechnet in Gleitkomma und rundet zum Naechsten. Je Blend-Ebene ergibt
 *    das bis zu +1 LSB, bei gestapelten Ebenen (Titel: Nebel-Ebenen)
 *    gemessen bis +5 (M9-Bildvergleich, docs/GAME_NOTES.md).
 * Konstantfarbe + Konstantalpha in einem Draw (verschiedene D3D-Blend-
 * Konstanten) faellt unter den zweiten Punkt. */
static int pica_shader_blend(const HwDrawState *st) {
    uint32_t f[4];
    int i;
    if (!st->blend_enable) return 0;
    if (st->blend_eq_rgb >= 3u || st->blend_eq_a >= 3u) return 1;
    f[0] = st->blend_src_rgb; f[1] = st->blend_dst_rgb;
    f[2] = st->blend_src_a; f[3] = st->blend_dst_a;
    for (i = 0; i < 4; i++)
        if ((f[i] & 0xFu) > 1u) return 1;       /* nicht Zero/One */
    return 0;
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
    /* Tiefe wie p_depth_stencil_test: z = (uint)(depth * (2^bits - 1)).
       D16/D24S8 (UNORM) runden beim Schreiben auf genau diese Ganzzahl,
       D24 liegt als D32_FLOAT mit Skala 2^24 vor (depth_from_guest). */
    if (st->depth_format == 0u) {
        cb.f[6] = 65535.0f;
        cb.f[10] = 65535.0f;
    } else if (st->depth_format == 3u) {
        cb.f[6] = 16777215.0f;
        cb.f[10] = 16777215.0f;
    } else {
        cb.f[6] = 16777215.0f;
        cb.f[10] = 16777216.0f;
    }
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
        cb.u[160 + i * 4 + 0] = (g_texbind[i] >= 0) ? 1u : 0u;
        /* k_tex2[i].y: nur die Texturfilter-Variante liest es */
        cb.u[160 + i * 4 + 1] = st->tex[i].mag_linear ? 1u : 0u;
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
/* Inhaltshash der Texturbytes: vier unabhaengige 64-Bit-Spuren (Wort fuer
 * Wort multiplizieren + Xorshift), Rest byteweise FNV-1a. Rund 8x schneller
 * als reines FNV-1a; die Texturen werden je Kommandoliste und nach jedem
 * Rueckschreiben neu geprueft. */
static uint64_t fnv1a(const unsigned char *p, uint32_t n)
{
    const uint64_t m = 0xFF51AFD7ED558CCDULL;
    uint64_t h0 = 1469598103934665603ULL, h1 = 0x9E3779B97F4A7C15ULL;
    uint64_t h2 = 0xC2B2AE3D27D4EB4FULL, h3 = 0x165667B19E3779F9ULL;
    uint32_t i = 0;
    for (; i + 32u <= n; i += 32u) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p + i, 8);
        memcpy(&w1, p + i + 8u, 8);
        memcpy(&w2, p + i + 16u, 8);
        memcpy(&w3, p + i + 24u, 8);
        h0 = (h0 ^ w0) * m; h0 ^= h0 >> 29;
        h1 = (h1 ^ w1) * m; h1 ^= h1 >> 29;
        h2 = (h2 ^ w2) * m; h2 ^= h2 >> 29;
        h3 = (h3 ^ w3) * m; h3 ^= h3 >> 29;
    }
    h0 ^= (h1 * 31u) ^ (h2 * 961u) ^ (h3 * 29791u) ^ (uint64_t)n;
    for (; i < n; i++) {
        h0 ^= (uint64_t)p[i];
        h0 *= 1099511628211ULL;
    }
    return h0;
}

/* Gibt die D3D-Ressourcen eines Cache-Eintrags frei. */
static void texslot_release(HwTexSlot *s)
{
    free(s->src);
    if (s->srv) { ID3D11ShaderResourceView_Release(s->srv); s->srv = NULL; }
    if (s->tex) {
        ID3D11Texture2D_Release(s->tex);
        s->tex = NULL;
        g_tex_cache_bytes -= (unsigned long long)s->w * s->h * 4u;
    }
    memset(s, 0, sizeof(*s));
}

/* Dekodiert die Kachelzeilen [trow0, trow1) eines PICA-Texturbilds nach
 * RGBA8; out zeigt auf Pixelzeile trow0*8 eines Bildes der Breite w.
 * Kachelreihenfolge wie p_lookup_texel: ti = (y/8)*(w/8) + x/8, innerhalb
 * der Kachel dekodiert gpu_pica_decode_texel (alle Formate inkl. ETC1).
 * RGBA8 (Format 0, Gastbytes A,B,G,R) direkt: je Morton-Index ein
 * Byte-Tausch, bitgleich zu gpu_pica_decode_texel (Selbsttest). */
void hw_tex_decode_rows(const unsigned char *src, uint32_t fmt, uint32_t w,
                        uint32_t trow0, uint32_t trow1, unsigned char *out)
{
    uint32_t ts = gpu_pica_tex_tile_size(fmt);
    uint32_t tw = w / 8u, tx, ty, k;
    if (fmt == 0u) {
        /* Morton-Index -> (x, y) innerhalb der 8x8-Kachel */
        static const uint8_t mx[64] = {
            0,1,0,1,2,3,2,3, 0,1,0,1,2,3,2,3, 4,5,4,5,6,7,6,7, 4,5,4,5,6,7,6,7,
            0,1,0,1,2,3,2,3, 0,1,0,1,2,3,2,3, 4,5,4,5,6,7,6,7, 4,5,4,5,6,7,6,7 };
        static const uint8_t my[64] = {
            0,0,1,1,0,0,1,1, 2,2,3,3,2,2,3,3, 0,0,1,1,0,0,1,1, 2,2,3,3,2,2,3,3,
            4,4,5,5,4,4,5,5, 6,6,7,7,6,6,7,7, 4,4,5,5,4,4,5,5, 6,6,7,7,6,6,7,7 };
        for (ty = trow0; ty < trow1; ty++) {
            unsigned char *orow = out + (size_t)(ty - trow0) * 8u * w * 4u;
            for (tx = 0; tx < tw; tx++) {
                const unsigned char *tile = src + ((size_t)ty * tw + tx) * 256u;
                unsigned char *o = orow + (size_t)tx * 32u;
                for (k = 0; k < 64u; k++) {
                    uint32_t v;
                    memcpy(&v, tile + k * 4u, 4);
                    v = (v >> 24) | ((v >> 8) & 0xFF00u) |
                        ((v << 8) & 0xFF0000u) | (v << 24);
                    memcpy(o + ((size_t)my[k] * w + mx[k]) * 4u, &v, 4);
                }
            }
        }
        return;
    }
    for (ty = trow0; ty < trow1; ty++) {
        for (tx = 0; tx < tw; tx++) {
            const unsigned char *tile = src + ((size_t)ty * tw + tx) * ts;
            for (k = 0; k < 64u; k++) {
                uint32_t px_x = k & 7u, px_y = k >> 3;
                unsigned char px[4];
                gpu_pica_decode_texel(tile, fmt, px_x, px_y, px);
                memcpy(out + ((((size_t)(ty - trow0) * 8u + px_y) * w +
                               tx * 8u + px_x) * 4u), px, 4);
            }
        }
    }
}

/* Dekodiert das Texturbild nach RGBA8 und laedt es hoch. Ist derselbe
 * Eintrag (Adresse/Format/Groesse) schon hochgeladen, werden nur die
 * Kachelzeilen von der ersten bis zur letzten geaenderten neu dekodiert und
 * per Box hochgeladen (Vergleich mit den gemerkten Quellbytes): der Titel
 * sampelt im 3D-Dialog eine 512x1024-RGBA8-Textur, die das gerade gerenderte
 * 240x400-Ziel enthaelt; je Gast-Frame aendert sich nur dieser Teil.
 * Unveraenderte Quellbytes ergeben unveraenderte Texel: bitgleich zum
 * vollen Upload. */
static int texslot_upload(HwTexSlot *s, const HwDrawState *st, const HwTexture *t)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    unsigned char *stage;
    const unsigned char *src;
    uint32_t ts, tiles, strip, th, r0, r1;
    unsigned long long t0 = 0;

    ts = gpu_pica_tex_tile_size(t->format);
    tiles = (t->width / 8u) * (t->height / 8u);
    if (ts == 0u || tiles == 0u) return 0;
    if (t->width == 0u || t->height == 0u || (uint64_t)tiles * ts > t->bytes)
        return 0;
    src = st->guest_mem + t->offset;
    th = t->height / 8u;
    strip = (t->width / 8u) * ts;             /* Bytes je Kachelzeile */

    if (!s->tex || s->w != t->width || s->h != t->height) {
        if (s->tex) {
            texslot_release(s);
        }
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
        g_tex_cache_bytes += (unsigned long long)t->width * t->height * 4u;
    }

    if (tex_diag_on()) t0 = hw_now_us();
    r0 = 0u;
    r1 = th;
    if (s->src != NULL && s->src_len == tiles * ts && s->fmt == t->format &&
        s->offset == t->offset) {
        /* Teil-Upload: erste und letzte geaenderte Kachelzeile. */
        while (r0 < th && memcmp(s->src + (size_t)r0 * strip,
                                 src + (size_t)r0 * strip, strip) == 0)
            r0++;
        if (r0 == th) {                       /* Bild unveraendert */
            s->bytes = t->bytes;
            if (t0 != 0ull) g_tex_upload_us += hw_now_us() - t0;
            return 1;
        }
        while (r1 > r0 + 1u && memcmp(s->src + (size_t)(r1 - 1u) * strip,
                                      src + (size_t)(r1 - 1u) * strip, strip) == 0)
            r1--;
    } else {
        free(s->src);
        s->src = (unsigned char *)malloc((size_t)tiles * ts);
        s->src_len = s->src ? tiles * ts : 0u;
    }
    stage = (unsigned char *)malloc((size_t)t->width * (r1 - r0) * 8u * 4u);
    if (!stage) return 0;
    hw_tex_decode_rows(src, t->format, t->width, r0, r1, stage);
    if (r0 == 0u && r1 == th) {
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)s->tex, 0,
                                              NULL, stage, t->width * 4u, 0);
    } else {
        D3D11_BOX box;
        box.left = 0; box.right = t->width;
        box.top = r0 * 8u; box.bottom = r1 * 8u;
        box.front = 0; box.back = 1;
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)s->tex, 0,
                                              &box, stage, t->width * 4u, 0);
        g_tex_partial++;
    }
    free(stage);
    if (s->src != NULL)
        memcpy(s->src + (size_t)r0 * strip, src + (size_t)r0 * strip,
               (size_t)(r1 - r0) * strip);
    s->offset = t->offset;
    s->fmt = t->format;
    s->bytes = t->bytes;
    g_tex_uploads++;
    if (t0 != 0ull) g_tex_upload_us += hw_now_us() - t0;
    return 1;
}

/* Sucht einen gueltigen Eintrag mit exakt diesem Schluessel. */
static int texslot_find(const HwTexture *t)
{
    int i;
    for (i = 0; i < HW_TEX_CACHE_N; i++) {
        HwTexSlot *s = &g_texcache[i];
        if (s->valid && s->offset == t->offset && s->fmt == t->format &&
            s->w == t->width && s->h == t->height && s->bytes == t->bytes)
            return i;
    }
    return -1;
}

/* Freien Platz schaffen (leerer Eintrag, sonst LRU). Eintraege, die in
 * diesem Draw bereits an eine fruehere Einheit gebunden sind, bleiben. */
static int texslot_alloc(const HwTexture *t, int unit)
{
    unsigned long long need = (unsigned long long)t->width * t->height * 4u;
    int i, j, victim = -1;
    uint32_t oldest = 0xFFFFFFFFu;

    for (i = 0; i < HW_TEX_CACHE_N; i++)
        if (!g_texcache[i].valid && g_texcache[i].tex == NULL) { victim = i; break; }
    if (victim < 0) {
        for (i = 0; i < HW_TEX_CACHE_N; i++) {
            int bound = 0;
            for (j = 0; j < unit; j++) if (g_texbind[j] == i) bound = 1;
            if (bound) continue;
            if (g_texcache[i].last_use <= oldest) {
                oldest = g_texcache[i].last_use;
                victim = i;
            }
        }
    }
    if (victim < 0) victim = 0;
    texslot_release(&g_texcache[victim]);
    /* Bytebudget: weitere LRU-Eintraege freigeben, bis der Upload passt. */
    while (need <= HW_TEX_CACHE_BYTES &&
           g_tex_cache_bytes + need > HW_TEX_CACHE_BYTES) {
        int drop = -1;
        uint32_t o = 0xFFFFFFFFu;
        for (i = 0; i < HW_TEX_CACHE_N; i++) {
            int bound = (i == victim);
            for (j = 0; j < unit; j++) if (g_texbind[j] == i) bound = 1;
            if (bound || !g_texcache[i].valid) continue;
            if (g_texcache[i].last_use <= o) { o = g_texcache[i].last_use; drop = i; }
        }
        if (drop < 0) break;
        texslot_release(&g_texcache[drop]);
    }
    return victim;
}

static void bind_textures(const HwDrawState *st)
{
    ID3D11ShaderResourceView *srv[3];
    int i;

    for (i = 0; i < 3; i++) {
        const HwTexture *t = &st->tex[i];
        HwTexSlot *s;
        int idx;

        g_texbind[i] = -1;
        srv[i] = g_tex_dummy;
        if (t->type != 0u || !t->enabled || t->bytes == 0u ||
            st->guest_mem == NULL ||
            (uint64_t)t->offset + t->bytes > st->guest_size)
            continue;
        /* Schluessel (Adresse, Format, Breite/Hoehe, Bytezahl) kann sich
           innerhalb derselben Kommandoliste aendern: bei JEDEM Draw suchen.
           Der Byte-Hash laeuft hoechstens einmal pro Kommandoliste je
           Textur, bei neuer Texturgeneration oder nach einem
           GX-Schreibzugriff in den Bereich. */
        idx = texslot_find(t);
        if (idx < 0) {
            idx = texslot_alloc(t, i);
            s = &g_texcache[idx];
            if (!texslot_upload(s, st, t)) {
                texslot_release(s);
                continue;
            }
            s->valid = 1;
            s->hash = fnv1a(st->guest_mem + t->offset, t->bytes);
            s->list_seen = g_list_serial;
            s->gen_seen = st->tex_generation;
            s->stale = 0;
            g_tex_upload_key++;
        } else {
            s = &g_texcache[idx];
            if (s->gen_seen != st->tex_generation || s->stale ||
                s->list_seen != g_list_serial) {
                unsigned long long th = tex_diag_on() ? hw_now_us() : 0ull;
                uint64_t h = fnv1a(st->guest_mem + t->offset, t->bytes);
                if (th) g_dg_hash_us += hw_now_us() - th;
                if (s->hash != h) {
                    if (!texslot_upload(s, st, t)) {
                        texslot_release(s);
                        continue;
                    }
                    s->hash = h;
                    g_tex_upload_hash++;
                }
                s->list_seen = g_list_serial;
                s->gen_seen = st->tex_generation;
                s->stale = 0;
            }
        }
        s->last_use = ++g_tex_clock;
        g_texbind[i] = idx;
        srv[i] = s->srv;
    }
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 3, srv);
}

/* ---- M17: Hilfspipeline, Oberflaechen, Anzeigeflaechen ----------------- */

/* Vollbild-Dreieck; u0.x = Faktor s. UpPS/UpDPS/UpSPS vergroessern das
 * native Ziel (naechster Nachbar), DtPS rechnet einen DisplayTransfer wie
 * p_display_transfer_run (gpu.c) in interner Aufloesung:
 *   u0 = (s, hs, vs, flip), u1 = (Eingangsformat, Ausgangsformat,
 *   Ausgabezeilen intern, Quellzeilen intern), Formate in DisplayTransfer-
 *   Nummerierung (0 RGBA8, 1 RGB8, 2 RGB565, 3 RGB5A1, 4 RGBA4).
 * Die Ausgabe liegt in Gast-Orientierung: Texel (x, y) = Pixel x der
 * Framebuffer-Zeile y (eine Zeile = eine Bildschirmspalte). */
static const char *k_hlsl_util =
    "cbuffer UtilCB : register(b0) { uint4 u0; uint4 u1; };\n"
    "Texture2D<float4> usrc : register(t0);\n"
    "Texture2D<float> udep : register(t1);\n"
    "Texture2D<uint2> usten : register(t2);\n"
    "float4 FsVS(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 t = float2((id << 1) & 2, id & 2);\n"
    "  return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 UpPS(float4 p : SV_Position) : SV_Target {\n"
    "  int2 q = int2(p.xy) / (int)u0.x;\n"
    "  return usrc.Load(int3(q, 0));\n"
    "}\n"
    "float UpDPS(float4 p : SV_Position) : SV_Depth {\n"
    "  int2 q = int2(p.xy) / (int)u0.x;\n"
    "  return udep.Load(int3(q, 0));\n"
    "}\n"
    "void UpSPS(float4 p : SV_Position) {\n"
    "  int2 q = int2(p.xy) / (int)u0.x;\n"
    "  uint v = usten.Load(int3(q, 0)).g;\n"
    "  if (((v >> u0.y) & 1u) == 0u) discard;\n"
    "}\n"
    /* Kodieren in das Format und Dekodieren wie p_decode_pixel/fb_decode_pixel
       (Bitreplikation). */
    "uint4 q_fb(uint f, uint4 c) {\n"
    "  if (f == 1u) return uint4(c.rgb, 255u);\n"
    "  if (f == 2u) {\n"
    "    uint3 v = uint3(c.r >> 3, c.g >> 2, c.b >> 3);\n"
    "    return uint4((v.r << 3) | (v.r >> 2), (v.g << 2) | (v.g >> 4), (v.b << 3) | (v.b >> 2), 255u);\n"
    "  }\n"
    "  if (f == 3u) {\n"
    "    uint3 v = c.rgb >> 3;\n"
    "    return uint4((v << 3) | (v >> 2), (c.a >> 7) * 255u);\n"
    "  }\n"
    "  if (f == 4u) { uint4 v = c >> 4; return (v << 4) | v; }\n"
    "  return c;\n"
    "}\n"
    "uint4 dt_fetch(int ix, int iy) {\n"
    "  uint4 c = (uint4)round(usrc.Load(int3(ix, (int)u1.w - 1 - iy, 0)) * 255.0);\n"
    "  return q_fb(u1.x, c);\n"
    "}\n"
    "float4 DtPS(float4 p : SV_Position) : SV_Target {\n"
    "  int X = (int)p.x, Y = (int)p.y;\n"
    "  int yy = (u0.w != 0u) ? ((int)u1.z - 1 - Y) : Y;\n"
    "  int ix = X << u0.y, iy = yy << u0.z;\n"
    "  uint4 c = dt_fetch(ix, iy);\n"
    "  if (u0.y != 0u) {\n"
    "    uint4 c1 = dt_fetch(ix + 1, iy);\n"
    "    if (u0.z != 0u) {\n"
    "      uint4 c2 = dt_fetch(ix, iy + 1);\n"
    "      uint4 c3 = dt_fetch(ix + 1, iy + 1);\n"
    "      c = (c + c1 + c2 + c3) / 4u;\n"
    "    } else {\n"
    "      c = (c + c1) / 2u;\n"
    "    }\n"
    "  }\n"
    "  c = q_fb(u1.y, c);\n"
    "  return float4(c) / 255.0;\n"
    "}\n";

static ID3D11PixelShader *util_ps(const char *entry)
{
    ID3DBlob *b = NULL, *err = NULL;
    ID3D11PixelShader *ps = NULL;
    if (FAILED(g_pfn_d3d_compile(k_hlsl_util, strlen(k_hlsl_util), "hw_util", NULL, NULL,
                                 entry, "ps_5_0", 0, 0, &b, &err))) {
        if (err) {
            fprintf(stderr, "[hw] %s: %s\n", entry, (char *)ID3D10Blob_GetBufferPointer(err));
            ID3D10Blob_Release(err);
        }
        return NULL;
    }
    if (FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b),
                                              ID3D10Blob_GetBufferSize(b), NULL, &ps)))
        ps = NULL;
    ID3D10Blob_Release(b);
    return ps;
}

static int util_init(void)
{
    ID3DBlob *b = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bl;
    int i;
    if (FAILED(g_pfn_d3d_compile(k_hlsl_util, strlen(k_hlsl_util), "hw_util", NULL, NULL,
                                 "FsVS", "vs_5_0", 0, 0, &b, &err))) {
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (FAILED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(b),
                                               ID3D10Blob_GetBufferSize(b), NULL, &g_fs_vs))) {
        ID3D10Blob_Release(b);
        return 0;
    }
    ID3D10Blob_Release(b);
    g_up_ps = util_ps("UpPS");
    g_upd_ps = util_ps("UpDPS");
    g_ups_ps = util_ps("UpSPS");
    g_dt_ps = util_ps("DtPS");
    if (!g_up_ps || !g_upd_ps || !g_ups_ps || !g_dt_ps) return 0;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_util_cb))) return 0;
    memset(&ds, 0, sizeof(ds));
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &ds, &g_dss_upd))) return 0;
    for (i = 0; i < 8; i++) {
        memset(&ds, 0, sizeof(ds));
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = 0xFF;
        ds.StencilWriteMask = (UINT8)(1u << i);
        ds.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        ds.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        ds.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        ds.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        ds.BackFace = ds.FrontFace;
        if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &ds, &g_dss_ups[i]))) return 0;
    }
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs_util))) return 0;
    memset(&bl, 0, sizeof(bl));
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bl, &g_bs_util))) return 0;
    return 1;
}

static void util_release(void)
{
    int i;
    if (g_fs_vs) { ID3D11VertexShader_Release(g_fs_vs); g_fs_vs = NULL; }
    if (g_up_ps) { ID3D11PixelShader_Release(g_up_ps); g_up_ps = NULL; }
    if (g_upd_ps) { ID3D11PixelShader_Release(g_upd_ps); g_upd_ps = NULL; }
    if (g_ups_ps) { ID3D11PixelShader_Release(g_ups_ps); g_ups_ps = NULL; }
    if (g_dt_ps) { ID3D11PixelShader_Release(g_dt_ps); g_dt_ps = NULL; }
    if (g_util_cb) { ID3D11Buffer_Release(g_util_cb); g_util_cb = NULL; }
    if (g_dss_upd) { ID3D11DepthStencilState_Release(g_dss_upd); g_dss_upd = NULL; }
    for (i = 0; i < 8; i++)
        if (g_dss_ups[i]) { ID3D11DepthStencilState_Release(g_dss_ups[i]); g_dss_ups[i] = NULL; }
    if (g_rs_util) { ID3D11RasterizerState_Release(g_rs_util); g_rs_util = NULL; }
    if (g_bs_util) { ID3D11BlendState_Release(g_bs_util); g_bs_util = NULL; }
}

/* Gemeinsamer Zustand fuer einen Vollbild-Durchgang (der naechste HW-Draw
 * bindet seinen Zustand ohnehin vollstaendig neu: hw_begin_draw_impl). */
static void util_pass(int w, int h, ID3D11PixelShader *ps, const uint32_t u[8])
{
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_util_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, u, 32);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_util_cb, 0);
    }
    memset(&vp, 0, sizeof(vp));
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs_util);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_fs_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_util_cb);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_bs_util, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
}

static void util_unbind(void)
{
    ID3D11ShaderResourceView *null3[3] = { NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 3, null3);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    g_batch_ok = 0;
}

static void surf_free(HwSurf *s)
{
    if (s->csrv) { ID3D11ShaderResourceView_Release(s->csrv); s->csrv = NULL; }
    if (s->crtv) { ID3D11RenderTargetView_Release(s->crtv); s->crtv = NULL; }
    if (s->ctex) { ID3D11Texture2D_Release(s->ctex); s->ctex = NULL; }
    if (s->dsv) { ID3D11DepthStencilView_Release(s->dsv); s->dsv = NULL; }
    if (s->dtex) { ID3D11Texture2D_Release(s->dtex); s->dtex = NULL; }
    if (g_cur_surf == s) g_cur_surf = NULL;
    memset(s, 0, sizeof(*s));
}

static void disp_free(HwDisp *d)
{
    if (d->srv) { ID3D11ShaderResourceView_Release(d->srv); d->srv = NULL; }
    if (d->rtv) { ID3D11RenderTargetView_Release(d->rtv); d->rtv = NULL; }
    if (d->tex) { ID3D11Texture2D_Release(d->tex); d->tex = NULL; }
    memset(d, 0, sizeof(*d));
}

static void surf_release_all(void)
{
    int i;
    for (i = 0; i < HW_SURF_N; i++) surf_free(&g_surf[i]);
    for (i = 0; i < HW_DISP_N; i++) disp_free(&g_disp[i]);
    if (g_hr_shadow_srv) { ID3D11ShaderResourceView_Release(g_hr_shadow_srv); g_hr_shadow_srv = NULL; }
    if (g_hr_shadow) { ID3D11Texture2D_Release(g_hr_shadow); g_hr_shadow = NULL; }
    g_hr_shadow_w = g_hr_shadow_h = 0;
    g_cur_surf = NULL;
}

static int tex_rt_srv(int w, int h, ID3D11Texture2D **tex, ID3D11RenderTargetView **rtv,
                      ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, tex))) { *tex = NULL; return 0; }
    if (rtv && FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)*tex,
                                                          NULL, rtv))) { *rtv = NULL; return 0; }
    if (srv && FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)*tex,
                                                            NULL, srv))) { *srv = NULL; return 0; }
    return 1;
}

/* Laenge des Farb-/Tiefenpuffers im Gast (0 = nicht vollstaendig adressierbar). */
static uint32_t surf_color_len(const HwDrawState *st, int w, int h)
{
    uint64_t n = (uint64_t)w * (uint64_t)h * fmt_bpp(st->color_format);
    return (st->color_mem != NULL && n <= st->color_size) ? (uint32_t)n : 0u;
}
static uint32_t surf_depth_len(const HwDrawState *st, int w, int h)
{
    uint64_t n = (uint64_t)w * (uint64_t)h * depth_bpp(st->depth_format);
    return (st->depth_mem != NULL && n <= st->depth_size) ? (uint32_t)n : 0u;
}

/* Hochskalierte Tiefe/Stencil aus dem nativen Ziel (s > 1). */
static void surf_upscale_depth(HwSurf *sf)
{
    uint32_t u[8] = {0};
    int s = g_scale, b;
    u[0] = (uint32_t)s;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, sf->dsv);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 1, &g_depth_srv);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_dss_upd, 0);
    util_pass(sf->w * s, sf->h * s, g_upd_ps, u);
    if (sf->depth_fmt == 3u && g_stencil_srv) {
        ID3D11DeviceContext_ClearDepthStencilView(g_ctx, sf->dsv, D3D11_CLEAR_STENCIL, 0.0f, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 2, 1, &g_stencil_srv);
        for (b = 0; b < 8; b++) {
            u[1] = (uint32_t)b;
            ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_dss_ups[b], 0xFF);
            util_pass(sf->w * s, sf->h * s, g_ups_ps, u);
        }
    }
    util_unbind();
}

/* Aufruf am Ende von create_targets (natives Ziel frisch aus dem Gast). */
static void surf_attach(const HwDrawState *st)
{
    HwSurf *sf = NULL;
    int i, w = g_target_w, h = g_target_h, s = g_scale;
    uint32_t clen, dlen;
    uint64_t ch = 0, dh = 0;

    g_cur_surf = NULL;
    if (!g_fs_vs || w <= 0 || h <= 0) return;
    for (i = 0; i < HW_SURF_N; i++) {
        HwSurf *t = &g_surf[i];
        if (t->valid && t->color_mem == st->color_mem && t->depth_mem == st->depth_mem &&
            t->color_fmt == st->color_format && t->depth_fmt == st->depth_format &&
            t->w == w && t->h == h) { sf = t; break; }
    }
    if (sf == NULL) {
        HwSurf *lru = &g_surf[0];
        for (i = 0; i < HW_SURF_N; i++) {
            if (!g_surf[i].valid) { lru = &g_surf[i]; break; }
            if (g_surf[i].last_use < lru->last_use) lru = &g_surf[i];
        }
        sf = lru;
        surf_free(sf);
        sf->color_mem = st->color_mem;
        sf->depth_mem = st->depth_mem;
        sf->color_fmt = st->color_format;
        sf->depth_fmt = st->depth_format;
        sf->w = w;
        sf->h = h;
        if (!tex_rt_srv(w * s, h * s, &sf->ctex, &sf->crtv, &sf->csrv)) { surf_free(sf); return; }
        if (s > 1) {
            D3D11_TEXTURE2D_DESC td;
            memset(&td, 0, sizeof(td));
            td.Width = (UINT)(w * s); td.Height = (UINT)(h * s);
            td.MipLevels = 1; td.ArraySize = 1;
            td.Format = depth_dxgi(st->depth_format);
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &sf->dtex)) ||
                FAILED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)sf->dtex,
                                                           NULL, &sf->dsv))) {
                surf_free(sf);
                return;
            }
        }
        sf->valid = 1;
    }
    clen = surf_color_len(st, w, h);
    dlen = surf_depth_len(st, w, h);
    if (clen) ch = fnv1a(st->color_mem, clen);
    if (dlen) dh = fnv1a(st->depth_mem, dlen);
    if (!(sf->cvalid && clen != 0u && clen == sf->clen && ch == sf->chash)) {
        if (s > 1) {
            uint32_t u[8] = {0};
            u[0] = (uint32_t)s;
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &sf->crtv, NULL);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_color_srv);
            util_pass(w * s, h * s, g_up_ps, u);
            util_unbind();
        } else {
            ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)sf->ctex,
                                             (ID3D11Resource *)g_color_tex);
        }
        sf->chash = ch;
        sf->clen = clen;
        sf->cvalid = (clen != 0u);
    }
    if (s > 1 && !(sf->dvalid && dlen != 0u && dlen == sf->dlen && dh == sf->dhash)) {
        surf_upscale_depth(sf);
        sf->dhash = dh;
        sf->dlen = dlen;
        sf->dvalid = (dlen != 0u);
    }
    sf->last_use = ++g_surf_clock;
    g_cur_surf = sf;
    if (s > 1 && (g_hr_shadow_w != w * s || g_hr_shadow_h != h * s)) {
        if (g_hr_shadow_srv) { ID3D11ShaderResourceView_Release(g_hr_shadow_srv); g_hr_shadow_srv = NULL; }
        if (g_hr_shadow) { ID3D11Texture2D_Release(g_hr_shadow); g_hr_shadow = NULL; }
        g_hr_shadow_w = g_hr_shadow_h = 0;
        if (tex_rt_srv(w * s, h * s, &g_hr_shadow, NULL, &g_hr_shadow_srv)) {
            g_hr_shadow_w = w * s;
            g_hr_shadow_h = h * s;
        }
    }
}

/* Nach dem Rueckschreiben: Oberflaeche und Gast stimmen ueberein. */
static void surf_after_flush(int depth_written)
{
    HwSurf *sf = g_cur_surf;
    uint32_t clen, dlen;
    if (sf == NULL || !sf->valid) return;
    if (g_scale == 1)
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)sf->ctex,
                                         (ID3D11Resource *)g_color_tex);
    if (g_scale > 1 && sf->hr_skipped) {
        /* interne Kopie nicht mitgerendert: beim naechsten Laden neu
           vergroessern, bis dahin kein GPU-Transfer aus ihr */
        sf->hr_skipped = 0;
        sf->cvalid = 0;
        sf->dvalid = 0;
        sf->last_use = ++g_surf_clock;
        return;
    }
    clen = surf_color_len(&g_state, g_target_w, g_target_h);
    sf->chash = clen ? fnv1a(g_state.color_mem, clen) : 0;
    sf->clen = clen;
    sf->cvalid = (clen != 0u);
    if (depth_written && g_scale > 1) {
        dlen = surf_depth_len(&g_state, g_target_w, g_target_h);
        sf->dhash = dlen ? fnv1a(g_state.depth_mem, dlen) : 0;
        sf->dlen = dlen;
        sf->dvalid = (dlen != 0u);
    }
    sf->last_use = ++g_surf_clock;
}

/* ---- Texturfilter der internen Aufloesung ------------------------------ */
/* Uebersetzen dauert einige Sekunden (der Pixelshader ist gross): es laeuft
 * ab hw_set_texture_filter in einem eigenen Thread, waehrend der Port das
 * Spiel startet; bis zum Ende zeigt der interne Durchgang ungefiltert. Mit
 * hw_texture_filter_sync(1) (--deterministic, Selbsttest) wartet der erste
 * gefilterte Durchgang stattdessen: Bildfolge unabhaengig vom Compiler. */
static HANDLE g_filt_thread;
static int g_filt_sync;              /* hw_texture_filter_sync: warten statt ungefiltert */
static ID3DBlob *g_filt_blob[2], *g_filt_err;
static HRESULT g_filt_hr;

static DWORD WINAPI filt_compile_thread(LPVOID arg)
{
    static const D3D_SHADER_MACRO defs[2] = { { "HW_TEXFILTER", "1" }, { NULL, NULL } };
    (void)arg;
    g_filt_hr = g_pfn_d3d_compile(k_hlsl, strlen(k_hlsl), "pica_hw_filt", defs, NULL,
                                  "PSMain", "ps_5_0", 0, 0, &g_filt_blob[0], &g_filt_err);
    if (SUCCEEDED(g_filt_hr))
        g_filt_hr = g_pfn_d3d_compile(k_hlsl, strlen(k_hlsl), "pica_hw_filt", defs, NULL,
                                      "GSMain", "gs_5_0", 0, 0, &g_filt_blob[1], &g_filt_err);
    return 0;
}

static void filt_start_compile(void)
{
    if (g_filt_thread || g_filt_state != 0 || !g_pfn_d3d_compile) return;
    g_filt_thread = CreateThread(NULL, 0, filt_compile_thread, NULL, 0, NULL);
}

static void filt_join(void)
{
    if (!g_filt_thread) return;
    WaitForSingleObject(g_filt_thread, INFINITE);
    CloseHandle(g_filt_thread);
    g_filt_thread = NULL;
}

static void filt_release(void)
{
    int i;
    filt_join();
    for (i = 0; i < 2; i++)
        if (g_filt_blob[i]) { ID3D10Blob_Release(g_filt_blob[i]); g_filt_blob[i] = NULL; }
    if (g_filt_err) { ID3D10Blob_Release(g_filt_err); g_filt_err = NULL; }
    if (g_ps_filt) { ID3D11PixelShader_Release(g_ps_filt); g_ps_filt = NULL; }
    if (g_gs_filt) { ID3D11GeometryShader_Release(g_gs_filt); g_gs_filt = NULL; }
    if (g_filt_cb) { ID3D11Buffer_Release(g_filt_cb); g_filt_cb = NULL; }
    g_filt_state = 0;
    g_filt_cb_mode = -1;
}

/* Legt die Filter-Variante beim ersten Bedarf an. Rueckgabe 1, wenn der
 * interne Durchgang gefiltert werden soll. */
static int filt_active(void)
{
    D3D11_BUFFER_DESC bd;
    unsigned long long tw;
    if (g_texfilter == HW_TEXFILTER_NONE || g_filt_state < 0) return 0;
    if (g_filt_state == 0) {
        g_filt_state = -1;
        tw = hw_now_us();
        if (!g_filt_thread && !g_filt_blob[0]) {
            if (!g_pfn_d3d_compile) goto fail;
            filt_compile_thread(NULL);
        }
        /* Uebersetzung laeuft noch: bis dahin ungefiltert anzeigen statt
           das Spiel anzuhalten (gemessen ~13 s nach dem Start). */
        if (g_filt_thread && !g_filt_sync &&
            WaitForSingleObject(g_filt_thread, 0) == WAIT_TIMEOUT) {
            g_filt_state = 0;
            return 0;
        }
        filt_join();
        if (FAILED(g_filt_hr) || !g_filt_blob[0] || !g_filt_blob[1]) {
            fprintf(stderr, "[hw] Texturfilter-Shader: %s\n",
                    g_filt_err ? (char *)ID3D10Blob_GetBufferPointer(g_filt_err) : "?");
            goto fail;
        }
        if (FAILED(ID3D11Device_CreatePixelShader(
                g_dev, ID3D10Blob_GetBufferPointer(g_filt_blob[0]),
                ID3D10Blob_GetBufferSize(g_filt_blob[0]), NULL, &g_ps_filt)) ||
            FAILED(ID3D11Device_CreateGeometryShader(
                g_dev, ID3D10Blob_GetBufferPointer(g_filt_blob[1]),
                ID3D10Blob_GetBufferSize(g_filt_blob[1]), NULL, &g_gs_filt)))
            goto fail;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_filt_cb))) goto fail;
        g_filt_state = 1;
        printf("[hw] Texturfilter %s fuer die interne Aufloesung bereit (gewartet %llu ms)\n",
               hw_texture_filter_name(g_texfilter), (hw_now_us() - tw) / 1000ull);
    }
    if (g_filt_cb_mode != g_texfilter * 16 + g_scale) {
        uint32_t u[4] = { (uint32_t)g_texfilter, (uint32_t)g_scale, 0u, 0u };
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_filt_cb, 0, NULL,
                                              u, 0, 0);
        g_filt_cb_mode = g_texfilter * 16 + g_scale;
    }
    return 1;
fail:
    filt_release();
    g_filt_state = -1;
    printf("[hw] Texturfilter nicht verfuegbar, Anzeige ungefiltert\n");
    return 0;
}

/* Bindungen fuer den nativen (0) bzw. internen (1) Durchgang in pend_flush. */
static void hr_bind(int hires)
{
    if (!hires) {
        ID3D11RenderTargetView *rtv[1] = { g_color_rtv };
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, rtv, g_dsv);
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &g_vp_nat);
        ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &g_rc_nat);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 3, 1,
            g_shadow_on ? &g_color_shadow_srv : &g_tex_dummy);
    } else {
        int s = g_scale;
        D3D11_VIEWPORT vp = g_vp_nat;
        D3D11_RECT rc = g_rc_nat;
        ID3D11RenderTargetView *rtv[1] = { g_cur_surf->crtv };
        vp.TopLeftX *= (float)s; vp.TopLeftY *= (float)s;
        vp.Width *= (float)s; vp.Height *= (float)s;
        rc.left *= s; rc.top *= s; rc.right *= s; rc.bottom *= s;
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, rtv, g_cur_surf->dsv);
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &rc);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 3, 1,
            g_shadow_on ? &g_hr_shadow_srv : &g_tex_dummy);
    }
}

/* Texturfilter: Shader des internen Durchgangs an (1) bzw. zurueck auf den
 * unveraenderten nativen Pixelshader ohne Geometry-Shader (0). */
static void filt_bind(int on)
{
    if (on) {
        ID3D11DeviceContext_GSSetShader(g_ctx, g_gs_filt, NULL, 0);
        ID3D11DeviceContext_GSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps_filt, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 2, 1, &g_filt_cb);
    } else {
        ID3D11DeviceContext_GSSetShader(g_ctx, NULL, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    }
}

static int hr_active(void)
{
    if (g_scale > 1 && g_cur_surf != NULL && !g_cur_surf->shown) {
        g_cur_surf->hr_skipped = 1;
        return 0;
    }
    return g_scale > 1 && g_cur_surf != NULL && g_cur_surf->crtv != NULL &&
           g_cur_surf->dsv != NULL && g_hr_shadow != NULL;
}

/* PICA-Farbzielformat (0x117) -> DisplayTransfer-/Framebuffer-Format. */
static uint32_t cb_to_fb_fmt(uint32_t f)
{
    static const uint32_t m[5] = { 0u, 1u, 3u, 2u, 4u };
    return f < 5u ? m[f] : 0xFFu;
}

static uint32_t fb_fmt_bpp(uint32_t f)
{
    switch (f & 7u) {
    case 0: return 4u;
    case 1: return 3u;
    case 2: case 3: case 4: return 2u;
    default: return 0u;
    }
}

static void disp_invalidate_range(const unsigned char *dst, uint32_t extent)
{
    int i;
    for (i = 0; i < HW_DISP_N; i++) {
        HwDisp *d = &g_disp[i];
        if (!d->valid) continue;
        if (dst < d->dst + d->len && d->dst < dst + extent) d->valid = 0;
    }
}

void hw_display_transfer(const unsigned char *src, const unsigned char *dst,
                         uint32_t in_size, uint32_t out_size, uint32_t flags,
                         uint32_t extent)
{
    const uint32_t in_w = in_size & 0xFFFFu, in_h = (in_size >> 16) & 0xFFFFu;
    const uint32_t out_w = out_size & 0xFFFFu, out_h = (out_size >> 16) & 0xFFFFu;
    const uint32_t in_fmt = (flags >> 8) & 7u, out_fmt = (flags >> 12) & 7u;
    const uint32_t scaling = (flags >> 24) & 3u;
    uint32_t hs, vs, ow, oh, len, u[8];
    HwSurf *sf = NULL;
    HwDisp *d = NULL;
    int i, s = g_scale;

    hw_lock_enter();
    if (!g_ready || !g_dt_ps) goto out;
    disp_invalidate_range(dst, extent ? extent : 1u);
    /* Nur der gekachelte -> lineare Standardfall (Bildschirmausgabe). */
    if ((flags & 2u) || (flags & 0x20u) || ((flags & 1u) && (flags & 4u)) ||
        scaling > 2u || fb_fmt_bpp(out_fmt) == 0u || in_w == 0u || in_h == 0u)
        goto cpu;
    hs = scaling != 0u ? 1u : 0u;
    vs = scaling == 2u ? 1u : 0u;
    ow = out_w >> hs;
    oh = out_h >> vs;
    if (ow == 0u || oh == 0u || out_w > in_w || out_h > in_h) goto cpu;
    for (i = 0; i < HW_SURF_N; i++) {
        HwSurf *t = &g_surf[i];
        if (!t->valid || t->color_mem != src) continue;
        if (t->w != (int)in_w || t->h != (int)in_h || cb_to_fb_fmt(t->color_fmt) != in_fmt)
            continue;
        if (sf == NULL || t->last_use > sf->last_use) sf = t;
    }
    if (sf == NULL) goto cpu;
    if (g_scale > 1 && !sf->shown) {
        /* erstes Mal Bildschirmquelle: ab jetzt intern mitrendern */
        sf->shown = 1;
        goto cpu;
    }
    if (!sf->cvalid || fnv1a(src, sf->clen) != sf->chash) goto cpu;
    len = ow * oh * fb_fmt_bpp(out_fmt);
    for (i = 0; i < HW_DISP_N; i++)
        if (g_disp[i].tex && g_disp[i].dst == dst) { d = &g_disp[i]; break; }
    if (d == NULL) {
        d = &g_disp[0];
        for (i = 0; i < HW_DISP_N; i++) {
            if (!g_disp[i].tex) { d = &g_disp[i]; break; }
            if (g_disp[i].last_use < d->last_use) d = &g_disp[i];
        }
    }
    if (!d->tex || d->tw != (int)ow * s || d->th != (int)oh * s) {
        disp_free(d);
        if (!tex_rt_srv((int)ow * s, (int)oh * s, &d->tex, &d->rtv, &d->srv)) {
            disp_free(d);
            goto cpu;
        }
        d->tw = (int)ow * s;
        d->th = (int)oh * s;
    }
    pend_flush();
    u[0] = (uint32_t)s; u[1] = hs; u[2] = vs; u[3] = flags & 1u;
    u[4] = in_fmt; u[5] = out_fmt; u[6] = oh * (uint32_t)s; u[7] = in_h * (uint32_t)s;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &d->rtv, NULL);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &sf->csrv);
    util_pass(d->tw, d->th, g_dt_ps, u);
    util_unbind();
    d->dst = dst;
    d->len = len;
    d->fmt = out_fmt;
    d->ow = ow;
    d->oh = oh;
    d->hash = fnv1a(dst, len);
    d->valid = 1;
    d->last_use = ++g_surf_clock;
    g_disp_gpu++;
    goto out;
cpu:
    g_disp_cpu++;
out:
    hw_lock_leave();
}

/* Anzeigeflaeche zum Framebuffer ptr (Spaltenabstand stride, GSP-Format),
 * nur wenn die Gastbytes noch dem Stand nach dem GPU-Transfer entsprechen.
 * Aufrufer haelt hw_lock(). */
void *hw_display_lookup(const unsigned char *ptr, uint32_t stride, uint32_t fmt,
                        int columns, int rows, int *tex_w, int *tex_h)
{
    int i;
    uint32_t bpp = fb_fmt_bpp(fmt);
    if (!g_ready || bpp == 0u || ptr == NULL) return NULL;
    if (stride != 0u && stride != (uint32_t)rows * bpp) return NULL;
    for (i = 0; i < HW_DISP_N; i++) {
        HwDisp *d = &g_disp[i];
        if (!d->valid || d->dst != ptr || d->fmt != (fmt & 7u)) continue;
        if (d->ow != (uint32_t)rows || d->oh < (uint32_t)columns) continue;
        if (fnv1a(ptr, d->len) != d->hash) return NULL;
        if (tex_w) *tex_w = d->tw;
        if (tex_h) *tex_h = d->th;
        return d->srv;
    }
    return NULL;
}

void hw_display_stats(unsigned long long *gpu, unsigned long long *cpu)
{
    if (gpu) *gpu = g_disp_gpu;
    if (cpu) *cpu = g_disp_cpu;
}

void hw_lock(void) { hw_lock_enter(); }
void hw_unlock(void) { hw_lock_leave(); }
void *hw_d3d_device(void) { return g_dev; }
void *hw_d3d_context(void) { return g_ctx; }
int hw_scale(void) { return g_scale; }

void hw_set_scale(int s)
{
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    hw_lock_enter();
    if (s != g_scale) {
        if (g_ready) {
            hw_sync_guest();
            surf_release_all();
        }
        g_scale = s;
    }
    hw_lock_leave();
}

void hw_set_texture_filter(int mode)
{
    if (mode < HW_TEXFILTER_NONE || mode > HW_TEXFILTER_XBRZ) mode = HW_TEXFILTER_NONE;
    hw_lock_enter();
    if (mode != g_texfilter) {
        /* laufenden Batch noch mit dem alten Filter zeichnen */
        if (g_ready) pend_flush();
        g_texfilter = mode;
    }
    /* nur bei interner Aufloesung gebraucht: dann sofort im Hintergrund
       uebersetzen */
    if (mode != HW_TEXFILTER_NONE && g_scale > 1 && g_ready) filt_start_compile();
    hw_lock_leave();
}

int hw_texture_filter(void) { return g_texfilter; }

void hw_texture_filter_sync(int on) { g_filt_sync = on != 0; }

int hw_texture_filter_parse(const char *name)
{
    if (name == NULL) return -1;
    if (_stricmp(name, "none") == 0) return HW_TEXFILTER_NONE;
    if (_stricmp(name, "linear") == 0) return HW_TEXFILTER_LINEAR;
    if (_stricmp(name, "xbrz") == 0) return HW_TEXFILTER_XBRZ;
    return -1;
}

const char *hw_texture_filter_name(int mode)
{
    switch (mode) {
    case HW_TEXFILTER_LINEAR: return "linear";
    case HW_TEXFILTER_XBRZ: return "xbrz";
    default: return "none";
    }
}

int hw_surface_read(const unsigned char *color_mem, unsigned char *out, int cap,
                    int *w, int *h)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11Texture2D *st = NULL;
    HwSurf *sf = NULL;
    int i, y, ok = 0;
    hw_lock_enter();
    if (!g_ready) goto out;
    pend_flush();
    for (i = 0; i < HW_SURF_N; i++)
        if (g_surf[i].valid && g_surf[i].color_mem == color_mem && g_surf[i].ctex &&
            (sf == NULL || g_surf[i].last_use > sf->last_use))
            sf = &g_surf[i];
    if (sf == NULL) goto out;
    ID3D11Texture2D_GetDesc(sf->ctex, &td);
    if ((long long)td.Width * td.Height * 4 > cap) goto out;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &st))) goto out;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)sf->ctex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ,
                                          0, &map))) {
        for (y = 0; y < (int)td.Height; y++)
            memcpy(out + (size_t)y * td.Width * 4u,
                   (const unsigned char *)map.pData + (size_t)y * map.RowPitch,
                   (size_t)td.Width * 4u);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        if (w) *w = (int)td.Width;
        if (h) *h = (int)td.Height;
        ok = 1;
    }
    ID3D11Texture2D_Release(st);
out:
    hw_lock_leave();
    return ok;
}

/* Gesammelte Dreiecke mit einem Draw zeichnen (Ringpuffer: anhaengen mit
 * NO_OVERWRITE, bei vollem Puffer DISCARD und von vorn). */
static void pend_flush(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT stride = sizeof(HwVertex), offset = 0;
    UINT n = (UINT)g_pend_n;
    D3D11_BOX box;
    int hires, have_box = 0;

    if (n == 0u) return;
    g_pend_n = 0;
    if (!g_ready || g_vb == NULL) return;
    if (g_vb_pos + n > (UINT)HW_VB_CAP) {
        mode = D3D11_MAP_WRITE_DISCARD;
        g_vb_pos = 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_vb, 0, mode, 0, &map)))
        return;
    memcpy((HwVertex *)map.pData + g_vb_pos, g_pend, sizeof(HwVertex) * (size_t)n);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    hires = hr_active();
    if (hires) hr_bind(0);
    /* Momentaufnahme nur des Rechtecks, das der Batch abdeckt (der Shader
       liest tshadow ausschliesslich an der eigenen Pixelposition). */
    if (g_cov_track && g_color_shadow && g_color_tex && g_cov_x1 >= g_cov_x0 &&
        g_cov_y1 >= g_cov_y0) {
        box.left = (UINT)g_cov_x0; box.right = (UINT)g_cov_x1 + 1u;
        box.top = (UINT)g_cov_y0; box.bottom = (UINT)g_cov_y1 + 1u;
        box.front = 0; box.back = 1;
        have_box = 1;
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_color_shadow,
                                                  0, box.left, box.top, 0,
                                                  (ID3D11Resource *)g_color_tex, 0, &box);
    }
    g_cov_x0 = g_cov_y0 = 0x7FFFFFFF;
    g_cov_x1 = g_cov_y1 = -1;
    ID3D11DeviceContext_Draw(g_ctx, n, g_vb_pos);
    /* M17: derselbe Batch in interner Aufloesung (nur Anzeige). Die
       Ueberschneidungspruefung laeuft nativ; die Momentaufnahme des
       skalierten Rechtecks folgt denselben Batch-Grenzen. */
    if (hires) {
        UINT s = (UINT)g_scale;
        int filt = filt_active();
        hr_bind(1);
        if (have_box) {
            box.left *= s; box.right *= s; box.top *= s; box.bottom *= s;
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_hr_shadow,
                                                      0, box.left, box.top, 0,
                                                      (ID3D11Resource *)g_cur_surf->ctex, 0, &box);
        }
        if (filt) filt_bind(1);
        ID3D11DeviceContext_Draw(g_ctx, n, g_vb_pos);
        if (filt) filt_bind(0);
    }
    g_vb_pos += n;
    g_batches++;
    /* Befehle frueh an die GPU uebergeben: sonst sammelt der Treiber die
       Draws der ganzen Kommandoliste und die GPU beginnt erst beim Map des
       Rueckschreibens (hw_flush_to_guest wartete dann die volle GPU-Zeit
       der Liste ab). Reine Uebergabe, keine Aenderung der Ergebnisse. */
    if (++g_submit_cnt >= HW_SUBMIT_BATCHES) {
        g_submit_cnt = 0;
        ID3D11DeviceContext_Flush(g_ctx);
        g_submits++;
    }
    /* Neuer Batch: Abdeckung zuruecksetzen (Id weiterzaehlen). */
    if (++g_cov_id == 0u) {
        if (g_cov) memset(g_cov, 0, sizeof(uint16_t) * (size_t)g_cov_w * (size_t)g_cov_h);
        g_cov_id = 1;
    }
}

/* Dreieck nach D3D11-Rasterregeln: Perspektivdivision und Viewport in
   float32, Einrasten auf 1/256 Pixel (16.8-Festkomma, round-to-nearest-
   even), Abtastung in der Pixelmitte, Top-Left-Regel (Y nach unten).
   Ein Pixel (px,py) ist abgedeckt, wenn fuer alle drei Kanten
   e = ex*(sy-y) - ey*(sx-x) > 0 gilt (= 0 nur auf Top-Left-Kanten), mit
   sx = 256*px+128, sy = 256*py+128. */
typedef struct {
    long long x[3], y[3], ex[3], ey[3];
    int tl[3];
    int x0, x1, y0, y1;     /* Pixel-Rechteck, auf das Ziel beschnitten */
} CovTri;

static long long cov_fdiv(long long a, long long b)   /* floor(a/b), b > 0 */
{
    long long q = a / b;
    if ((a % b) != 0 && a < 0) q--;
    return q;
}

static int cov_setup(CovTri *c, const HwVertex *t, float half_w, float half_h,
                     int corner_x, int corner_y, int w, int h)
{
    long long area, minx, maxx, miny, maxy;
    int i;
    for (i = 0; i < 3; i++) {
        float iw = 1.0f / t[i].pos[3];
        float fx = (t[i].pos[0] * iw + 1.0f) * half_w + (float)corner_x;
        float fy = (t[i].pos[1] * iw + 1.0f) * half_h + (float)corner_y;
        c->x[i] = llrint((double)fx * 256.0);
        c->y[i] = llrint((double)fy * 256.0);
    }
    area = (c->x[1] - c->x[0]) * (c->y[2] - c->y[0]) -
           (c->x[2] - c->x[0]) * (c->y[1] - c->y[0]);
    if (area == 0) return 0;                    /* D3D zeichnet nichts */
    if (area < 0) {                             /* Innenseite: E > 0 */
        long long tx = c->x[1], ty = c->y[1];
        c->x[1] = c->x[2]; c->y[1] = c->y[2];
        c->x[2] = tx; c->y[2] = ty;
    }
    for (i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        c->ex[i] = c->x[j] - c->x[i];
        c->ey[i] = c->y[j] - c->y[i];
        c->tl[i] = (c->ey[i] < 0) || (c->ey[i] == 0 && c->ex[i] > 0);
    }
    minx = maxx = c->x[0];
    miny = maxy = c->y[0];
    for (i = 1; i < 3; i++) {
        if (c->x[i] < minx) minx = c->x[i];
        if (c->x[i] > maxx) maxx = c->x[i];
        if (c->y[i] < miny) miny = c->y[i];
        if (c->y[i] > maxy) maxy = c->y[i];
    }
    c->x0 = (int)ceil((double)(minx - 128) / 256.0);
    c->x1 = (int)floor((double)(maxx - 128) / 256.0);
    c->y0 = (int)ceil((double)(miny - 128) / 256.0);
    c->y1 = (int)floor((double)(maxy - 128) / 256.0);
    if (c->x0 < 0) c->x0 = 0;
    if (c->y0 < 0) c->y0 = 0;
    if (c->x1 > w - 1) c->x1 = w - 1;
    if (c->y1 > h - 1) c->y1 = h - 1;
    return c->x0 <= c->x1 && c->y0 <= c->y1;
}

/* Abgedeckte Pixel der Zeile py als ein Intervall [lo, hi] (das Dreieck ist
   konvex und e je Kante linear in px: e(px) = C - D*px mit D = 256*ey).
   Bedingung e >= thr (thr = 0 auf Top-Left-Kanten, sonst 1) ergibt je
   Kante eine Halbgerade, exakt per Ganzzahl-Division statt Pixeltest. */
static int cov_row_span(const CovTri *c, int py, int *lo_out, int *hi_out)
{
    long long sy = (long long)py * 256 + 128;
    long long lo = c->x0, hi = c->x1;
    int k;
    for (k = 0; k < 3; k++) {
        long long C = c->ex[k] * (sy - c->y[k]) - c->ey[k] * (128 - c->x[k]);
        long long D = 256 * c->ey[k];
        long long thr = c->tl[k] ? 0 : 1;
        if (D == 0) {
            if (C < thr) return 0;
        } else if (D > 0) {             /* px <= floor((C-thr)/D) */
            long long b = cov_fdiv(C - thr, D);
            if (b < hi) hi = b;
        } else {                        /* px >= ceil((thr-C)/-D) */
            long long b = -cov_fdiv(C - thr, -D);
            if (b > lo) lo = b;
        }
        if (lo > hi) return 0;
    }
    *lo_out = (int)lo;
    *hi_out = (int)hi;
    return 1;
}

/* Markiert die Pixel eines Dreiecks im aktuellen Batch. Rueckgabe 1, wenn
 * ein Pixel schon von einem frueheren Dreieck dieses Batches belegt ist.
 * Je Zeile wird nur die abgedeckte Spanne besucht (cov_row_span). */
static int cov_mark(const HwVertex *t)
{
    const HwDrawState *st = &g_state;
    CovTri c;
    int px, py, hit = 0;

    if (g_cov == NULL || g_cov_w != g_target_w || g_cov_h != g_target_h) {
        free(g_cov);
        g_cov_w = g_target_w;
        g_cov_h = g_target_h;
        g_cov = (uint16_t *)calloc((size_t)g_cov_w * (size_t)g_cov_h, sizeof(uint16_t));
        g_cov_id = 1;
        if (g_cov == NULL) {
            /* Ohne Abdeckungsfeld: jedes Dreieck einzeln, volle Kopie. */
            g_cov_w = g_cov_h = 0;
            g_cov_x0 = 0; g_cov_y0 = 0;
            g_cov_x1 = g_target_w - 1; g_cov_y1 = g_target_h - 1;
            return 1;
        }
    }
    if (!cov_setup(&c, t, st->viewport_half_w, st->viewport_half_h,
                   st->viewport_corner_x, st->viewport_corner_y,
                   g_cov_w, g_cov_h))
        return 0;
    if (c.x0 < g_cov_x0) g_cov_x0 = c.x0;
    if (c.y0 < g_cov_y0) g_cov_y0 = c.y0;
    if (c.x1 > g_cov_x1) g_cov_x1 = c.x1;
    if (c.y1 > g_cov_y1) g_cov_y1 = c.y1;
    for (py = c.y0; py <= c.y1; py++) {
        uint16_t *row = g_cov + (size_t)py * (size_t)g_cov_w;
        int lo, hi;
        if (!cov_row_span(&c, py, &lo, &hi)) continue;
        for (px = lo; px <= hi; px++) {
            hit |= (row[px] == g_cov_id);
            row[px] = g_cov_id;
        }
    }
    return hit;
}

/* Testzugang (tests/hw_selftest.c): Abdeckung eines Dreiecks als Maske. */
int hw_cov_tri_mask(const HwVertex t[3], float half_w, float half_h,
                    int corner_x, int corner_y, int w, int h,
                    unsigned char *mask)
{
    CovTri c;
    int py, px, n = 0;
    memset(mask, 0, (size_t)w * (size_t)h);
    if (!cov_setup(&c, t, half_w, half_h, corner_x, corner_y, w, h)) return 0;
    for (py = c.y0; py <= c.y1; py++) {
        int lo, hi;
        if (!cov_row_span(&c, py, &lo, &hi)) continue;
        for (px = lo; px <= hi; px++) { mask[(size_t)py * w + px] = 1; n++; }
    }
    return n;
}

unsigned hw_draw_serial(void) { return g_begin_serial; }

int hw_continue_draw(unsigned serial) {
    return g_ready && g_have_state && g_batch_ok && !g_guest_dirty &&
           serial == g_begin_serial &&
           g_state.tex_generation == gpu_pica_tex_generation();
}

static int hw_begin_draw_impl(const HwDrawState *st);
static int hw_begin_draw_l(const HwDrawState *st) {
    unsigned long long t0;
    int r;
    /* Fast-Path: derselbe Zustand wie der laufende Batch. */
    if (g_ready && g_have_state && g_batch_ok && !g_guest_dirty &&
        g_state.tex_generation == gpu_pica_tex_generation() &&
        memcmp(st, &g_state_in, sizeof(*st)) == 0)
        return 1;
    t0 = tex_diag_on() ? hw_now_us() : 0ull;
    pend_flush();
    g_batch_ok = 0;
    r = hw_begin_draw_impl(st);
    if (r) {
        g_state_in = *st;
        g_begin_serial++;
        g_batch_ok = 1;
        g_cov_track = pica_shader_blend(st) || pica_logic_active(st);
    }
    if (t0) g_dg_begin_us += hw_now_us() - t0;
    return r;
}

static int hw_begin_draw_impl(const HwDrawState *st) {
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
        g_shadow_on = shadow;
        if (shadow) {
            /* Die Kopie selbst entsteht in pend_flush direkt vor dem Draw. */
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
    g_vp_nat = vp;

    if (st->scissor_mode == 3u) {
        rc.left = st->scissor_x1; rc.top = st->scissor_y1;
        rc.right = st->scissor_x2; rc.bottom = st->scissor_y2;
    } else {
        rc.left = 0; rc.top = 0; rc.right = g_target_w; rc.bottom = g_target_h;
    }
    ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &rc);
    g_rc_nat = rc;

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
    set_light_constants(st);
    return 1;
}

static int hw_triangles_impl(const HwVertex *v, int n);
static int hw_triangles_l(const HwVertex *v, int n) {
    unsigned long long t0 = tex_diag_on() ? hw_now_us() : 0ull;
    int r = hw_triangles_impl(v, n);
    if (t0) g_dg_tri_us += hw_now_us() - t0;
    return r;
}

static int hw_triangles_impl(const HwVertex *v, int n) {
    int i;

    if (!g_ready || !g_have_state || v == NULL || n < 3) return 0;
    if (n % 3) n -= n % 3;
    for (i = 0; i < n; i++) {
        if (v[i].pos[3] == 0.0f) return 0;
    }
    if (g_cov_track) {
        /* Dreieck fuer Dreieck: bei Ueberschneidung mit dem laufenden
           Batch erst zeichnen (neue Momentaufnahme), dann anhaengen. */
        for (i = 0; i + 3 <= n; i += 3) {
            if (g_pend_n + 3 > HW_PEND_CAP) pend_flush();
            g_dg_trk++;
            if (cov_mark(v + i)) {
                g_dg_ovl++;
                pend_flush();
                cov_mark(v + i);
            }
            memcpy(g_pend + g_pend_n, v + i, sizeof(HwVertex) * 3u);
            g_pend_n += 3;
        }
    } else {
        while (n > 0) {
            int k = n;
            if (k > HW_PEND_CAP) k = HW_PEND_CAP - HW_PEND_CAP % 3;
            if (g_pend_n + k > HW_PEND_CAP) pend_flush();
            memcpy(g_pend + g_pend_n, v, sizeof(HwVertex) * (size_t)k);
            g_pend_n += k;
            v += k;
            n -= k;
        }
    }
    if (!g_batch_ok) pend_flush();
    g_draws++;
    g_rendered = 1;
    if (g_state.depth_write ||
        (g_state.stencil_enable && g_state.stencil_write_mask != 0u &&
         (g_state.stencil_fail_op != 0u || g_state.stencil_zfail_op != 0u ||
          g_state.stencil_pass_op != 0u)))
        g_depth_dirty = 1;
    return 1;
}

static int hw_flush_to_guest_l(void) {
    unsigned long long t0 = 0;
    D3D11_MAPPED_SUBRESOURCE cm, dm;
    unsigned char *cbase, *dbase;
    uint32_t cbpp, h, w;
    int x, y;

    if (!g_ready || !g_have_state) return 0;
    if (!g_rendered) return 0;
    if (!g_color_tex || !g_color_stage || g_target_w <= 0 || g_target_h <= 0)
        return 0;
    pend_flush();
    w = (uint32_t)g_target_w;
    h = (uint32_t)g_target_h;
    cbpp = fmt_bpp(g_state.color_format);
    if (tex_diag_on()) t0 = hw_now_us();

    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_color_stage,
                                     (ID3D11Resource *)g_color_tex);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_color_stage, 0,
                                       D3D11_MAP_READ, 0, &cm)))
        return 0;
    if (t0) g_dg_wait_us += hw_now_us() - t0;
    cbase = (unsigned char *)cm.pData;

    /* Farbpuffer: PICA kachelt 8x8-Morton und legt die Zeilen von unten
       nach oben (y' = height-1-y), wie p_fb_write im Software-Pfad. */
    if (g_state.color_mem != NULL && g_state.color_format == 0u &&
        (w & 7u) == 0u && (h & 7u) == 0u &&
        (uint64_t)w * h * 4u <= g_state.color_size) {
        /* RGBA8 (Gast: A,B,G,R je Pixel) ohne Bereichspruefung je Pixel:
           das ganze Ziel liegt im Gastpuffer. Je 8er-Kachelzeile sind die
           Morton-Offsets xlut[x&7] fest; Ergebnis bitgleich zur
           allgemeinen Schleife unten (color_encode Format 0). */
        static const uint32_t xlut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
        static const uint32_t ylut[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
        for (y = 0; y < (int)h; y++) {
            uint32_t yp = h - 1u - (uint32_t)y;
            const uint32_t *row = (const uint32_t *)(cbase + (size_t)y * cm.RowPitch);
            unsigned char *dst = g_state.color_mem +
                                 ((size_t)(yp & ~7u) * w + ylut[yp & 7u]) * 4u;
            uint32_t tx;
            for (tx = 0; tx < w; tx += 8u) {
                unsigned char *tile = dst + (size_t)tx * 8u * 4u;
                int k;
                for (k = 0; k < 8; k++) {
                    uint32_t v = row[tx + (uint32_t)k];   /* R,G,B,A (LE) */
                    v = (v >> 24) | ((v >> 8) & 0xFF00u) |
                        ((v << 8) & 0xFF0000u) | (v << 24);
                    memcpy(tile + xlut[k] * 4u, &v, 4);
                }
            }
        }
    } else if (g_state.color_mem != NULL) {
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
    surf_after_flush(g_depth_dirty && g_state.depth_mem != NULL);
    g_flushes++;
    g_rendered = 0;
    g_depth_dirty = 0;
    g_guest_dirty = 0;
    /* Render-to-Texture: der Gastpuffer ist jetzt neuer Texturinhalt. */
    gpu_pica_bump_tex_generation();
    /* Der Gast entspricht jetzt dem D3D-Ziel; Schatten fuer hw_list_begin. */
    shadow_capture();
    if (t0) g_dg_flush_us += hw_now_us() - t0;
    return 1;
}

static void hw_list_begin_l(void) {
    /* Neue Kommandoliste: jede gebundene Textur wird hoechstens einmal per
       Byte-Hash gegen den Gastspeicher geprueft (CPU-Schreibzugriffe). */
    g_list_serial++;
    g_batch_ok = 0;
    g_dg_lists++;
    /* Diagnose (HLE_HW_STATE/HLE_GTIME): Upload- und Dekodierrate. */
    if (tex_diag_on()) {
        static unsigned long long t0, up0, dec0, start;
        static unsigned long long d0, f0, l0, fl0, wt0, bg0, tr0, hs0, b0, ov0, tk0, lw0;
        unsigned long long now = hw_now_us();
        if (start == 0ull) start = now;
        if (t0 == 0ull) { t0 = now; up0 = 0ull; dec0 = 0ull; }
        if (now - t0 >= 5000000ull && now > t0) {
            double sec = (double)(now - t0) / 1000000.0;
            printf("[texcache] t=%.1fs uploads=%llu (%.1f/s) key=%llu hash=%llu teil=%llu "
                   "dekodier_ms=%llu (%.1f%%)\n", (double)(now - start) / 1000000.0,
                   g_tex_uploads - up0, (double)(g_tex_uploads - up0) / sec,
                   g_tex_upload_key, g_tex_upload_hash, g_tex_partial,
                   (g_tex_upload_us - dec0) / 1000ull,
                   100.0 * (double)(g_tex_upload_us - dec0) / 1000000.0 / sec);
            fflush(stdout);
            printf("[hwtime] listen=%llu draws=%llu batches=%llu flushes=%llu flush_ms=%llu "
                   "(warten %llu) begin_ms=%llu tri_ms=%llu hash_ms=%llu ovl=%llu/%llu "
                   "sperre_ms=%llu\n",
                   g_dg_lists - l0, g_draws - d0, g_batches - b0, g_flushes - f0,
                   (g_dg_flush_us - fl0) / 1000ull, (g_dg_wait_us - wt0) / 1000ull,
                   (g_dg_begin_us - bg0) / 1000ull, (g_dg_tri_us - tr0) / 1000ull,
                   (g_dg_hash_us - hs0) / 1000ull, g_dg_ovl - ov0, g_dg_trk - tk0,
                   ((unsigned long long)g_lock_wait_us - lw0) / 1000ull);
            lw0 = (unsigned long long)g_lock_wait_us;
            fflush(stdout);
            l0 = g_dg_lists; d0 = g_draws; f0 = g_flushes; fl0 = g_dg_flush_us;
            wt0 = g_dg_wait_us; bg0 = g_dg_begin_us; tr0 = g_dg_tri_us;
            hs0 = g_dg_hash_us; b0 = g_batches; ov0 = g_dg_ovl; tk0 = g_dg_trk;
            t0 = now; up0 = g_tex_uploads; dec0 = g_tex_upload_us;
        }
    }
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

/* ---- Oeffentliche Einstiege unter der Context-Sperre (M17) ------------- */
int hw_begin_draw(const HwDrawState *st)
{ int r; hw_lock_enter(); r = hw_begin_draw_l(st); hw_lock_leave(); return r; }
int hw_triangles(const HwVertex *v, int n)
{ int r; hw_lock_enter(); r = hw_triangles_l(v, n); hw_lock_leave(); return r; }
int hw_flush_to_guest(void)
{ int r; hw_lock_enter(); r = hw_flush_to_guest_l(); hw_lock_leave(); return r; }
void hw_list_begin(void) { hw_lock_enter(); hw_list_begin_l(); hw_lock_leave(); }
void hw_invalidate_guest(void) { hw_lock_enter(); hw_invalidate_guest_l(); hw_lock_leave(); }
void hw_invalidate_textures(void) { hw_lock_enter(); hw_invalidate_textures_l(); hw_lock_leave(); }
void hw_invalidate_texture_range(uint32_t offset, uint32_t bytes)
{ hw_lock_enter(); hw_invalidate_texture_range_l(offset, bytes); hw_lock_leave(); }
void hw_sync_guest(void) { hw_lock_enter(); hw_sync_guest_l(); hw_lock_leave(); }

/* HLSL-Quelltext uebersetzen (d3dcompiler wird erst mit dem HW-Renderer
 * geladen); Rueckgabe ID3DBlob* oder NULL. Fuer runtime/gpu/hw_present.c. */
void *hw_compile_shader(const char *src, const char *entry, const char *profile)
{
    ID3DBlob *b = NULL, *err = NULL;
    if (!g_pfn_d3d_compile) return NULL;
    if (FAILED(g_pfn_d3d_compile(src, strlen(src), "hw", NULL, NULL, entry, profile, 0, 0,
                                 &b, &err))) {
        if (err) {
            fprintf(stderr, "[hw] %s: %s\n", entry, (char *)ID3D10Blob_GetBufferPointer(err));
            ID3D10Blob_Release(err);
        }
        return NULL;
    }
    return b;
}
