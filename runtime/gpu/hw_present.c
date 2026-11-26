/* Bildausgabe des Hardware-Renderers ueber DXGI-Swapchains (M17), siehe
 * hw_present.h.
 *
 * Threading: Das Immediate-Context gehoert dem GX-Worker (hw_d3d11.c). Die
 * Anzeige zeichnet im Fenster-Thread in einen Deferred Context und fuehrt
 * die Befehlsliste unter hw_lock() mit RestoreContextState = TRUE aus: der
 * Zustand des Renderers bleibt unberuehrt. Present, ResizeBuffers und GDI
 * (Statuszeilen) laufen ebenfalls unter der Sperre.
 *
 * Abbildung Anzeigeflaeche -> Bildschirm: Die Anzeigeflaeche liegt in
 * Gast-Orientierung (Texel (x, y) = Pixel x der Framebuffer-Zeile y), eine
 * Framebuffer-Zeile ist eine Bildschirmspalte, und Pixel x einer Zeile liegt
 * auf Bildschirmzeile rows-1-x (framebuffer.h: address + x*stride +
 * (rows-1-y)*bpp). Bildschirmpixel (X, Y) in interner Aufloesung kommt also
 * aus Texel (rows*s-1-Y, X).
 */
#define COBJMACROS
#include "hw_present.h"
#include "hw_d3d11.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* IIDs lokal (kein dxguid-Link) */
static const GUID k_iid_dxgi_device =
    {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const GUID k_iid_dxgi_factory2 =
    {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const GUID k_iid_dxgi_surface1 =
    {0x4ae63092, 0x6327, 0x4c1b, {0x80, 0xae, 0xbf, 0xe1, 0x2e, 0xa3, 0x2b, 0x86}};
static const GUID k_iid_d3d11_texture2d =
    {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

static const char *k_hlsl_present =
    "cbuffer P : register(b0) { float4 dst; float4 src; uint4 m; };\n"
    "Texture2D<float4> tsrc : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V QuadVS(uint id : SV_VertexID) {\n"
    "  float2 t = float2(id & 1, id >> 1);\n"
    "  V o;\n"
    "  o.pos = float4(lerp(dst.x, dst.z, t.x), lerp(dst.y, dst.w, t.y), 0, 1);\n"
    "  o.uv = lerp(src.xy, src.zw, t);\n"
    "  return o;\n"
    "}\n"
    /* m.x: 0 = Kanvas abtasten (Fenster), 1 = Anzeigeflaeche (gedreht,
       m.y = rows*s, Ursprung m.zw), 2 = natives CPU-Bild (m.y = s) */
    "float4 QuadPS(V i) : SV_Target {\n"
    "  if (m.x == 0u) return float4(tsrc.Sample(samp, i.uv).rgb, 1);\n"
    "  int2 p = int2(i.pos.xy) - int2(m.zw);\n"
    "  if (m.x == 1u) return float4(tsrc.Load(int3((int)m.y - 1 - p.y, p.x, 0)).rgb, 1);\n"
    "  return float4(tsrc.Load(int3(int2(i.pos.xy) / (int)m.y, 0)).rgb, 1);\n"
    "}\n";

typedef struct {
    HWND hwnd;
    IDXGISwapChain1 *sc;
    ID3D11RenderTargetView *rtv;
    int w, h;
    int gdi;
} HwpChain;

static int g_on;
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;      /* Immediate (nur unter hw_lock) */
static ID3D11DeviceContext *g_dc;       /* Deferred (Fenster-Thread) */
static IDXGIFactory2 *g_fac;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11Buffer *g_cb;
static ID3D11SamplerState *g_samp_point, *g_samp_lin;
static ID3D11RasterizerState *g_rs;
static ID3D11BlendState *g_bs;
static ID3D11DepthStencilState *g_dss;
static ID3D11Texture2D *g_canvas;       /* CW*s x CH*s */
static ID3D11RenderTargetView *g_canvas_rtv;
static ID3D11ShaderResourceView *g_canvas_srv;
static ID3D11Texture2D *g_upload;       /* CW x CH, dynamisch */
static ID3D11ShaderResourceView *g_upload_srv;
static unsigned char *g_rgba;
static int g_cw, g_ch, g_s;
static int g_have_frame;
static HwpChain g_chain[2];
static unsigned long long g_n_gpu, g_n_cpu;

typedef struct { float dst[4]; float src[4]; uint32_t m[4]; } PCb;

static void rel(IUnknown **p)
{
    if (*p) { IUnknown_Release(*p); *p = NULL; }
}
#define REL(x) rel((IUnknown **)&(x))

int hwp_active(void) { return g_on; }

static int create_canvas(void)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width = (UINT)(g_cw * g_s);
    td.Height = (UINT)(g_ch * g_s);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_canvas))) return 0;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_canvas, NULL,
                                                   &g_canvas_rtv))) return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_canvas, NULL,
                                                     &g_canvas_srv))) return 0;
    td.Width = (UINT)g_cw;
    td.Height = (UINT)g_ch;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_upload))) return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_upload, NULL,
                                                     &g_upload_srv))) return 0;
    return 1;
}

int hwp_init(int canvas_w, int canvas_h)
{
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    ID3DBlob *vb, *pb;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bl;
    D3D11_DEPTH_STENCIL_DESC ds;

    if (g_on) return 1;
    g_dev = (ID3D11Device *)hw_d3d_device();
    g_ctx = (ID3D11DeviceContext *)hw_d3d_context();
    if (!g_dev || !g_ctx) return 0;
    g_cw = canvas_w;
    g_ch = canvas_h;
    g_s = hw_scale();
    if (FAILED(ID3D11Device_QueryInterface(g_dev, &k_iid_dxgi_device, (void **)&xd))) goto fail;
    if (FAILED(IDXGIDevice_GetAdapter(xd, &ad))) goto fail;
    if (FAILED(IDXGIAdapter_GetParent(ad, &k_iid_dxgi_factory2, (void **)&g_fac))) goto fail;
    REL(ad);
    REL(xd);
    if (FAILED(ID3D11Device_CreateDeferredContext(g_dev, 0, &g_dc))) goto fail;
    vb = (ID3DBlob *)hw_compile_shader(k_hlsl_present, "QuadVS", "vs_5_0");
    pb = (ID3DBlob *)hw_compile_shader(k_hlsl_present, "QuadPS", "ps_5_0");
    if (!vb || !pb) {
        if (vb) ID3D10Blob_Release(vb);
        if (pb) ID3D10Blob_Release(pb);
        goto fail;
    }
    if (FAILED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vb),
                                               ID3D10Blob_GetBufferSize(vb), NULL, &g_vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(pb),
                                              ID3D10Blob_GetBufferSize(pb), NULL, &g_ps))) {
        ID3D10Blob_Release(vb);
        ID3D10Blob_Release(pb);
        goto fail;
    }
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(PCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb))) goto fail;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_point))) goto fail;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_lin))) goto fail;
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs))) goto fail;
    memset(&bl, 0, sizeof(bl));
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bl, &g_bs))) goto fail;
    memset(&ds, 0, sizeof(ds));
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &ds, &g_dss))) goto fail;
    if (!create_canvas()) goto fail;
    g_rgba = (unsigned char *)calloc((size_t)g_cw * (size_t)g_ch, 4);
    if (!g_rgba) goto fail;
    g_on = 1;
    printf("[present] D3D11-Swapchain aktiv, interne Aufloesung %dx (%dx%d)\n", g_s,
           g_cw * g_s, g_ch * g_s);
    return 1;
fail:
    REL(ad);
    REL(xd);
    hwp_shutdown();
    printf("[present] D3D11-Ausgabe nicht verfuegbar, GDI-Ausgabe\n");
    return 0;
}

void hwp_detach(void *hwnd)
{
    int i;
    for (i = 0; i < 2; i++) {
        HwpChain *c = &g_chain[i];
        if (c->hwnd != (HWND)hwnd || !c->sc) continue;
        hw_lock();
        REL(c->rtv);
        REL(c->sc);
        hw_unlock();
        memset(c, 0, sizeof(*c));
    }
}

void hwp_shutdown(void)
{
    int i;
    hw_lock();
    for (i = 0; i < 2; i++) {
        REL(g_chain[i].rtv);
        REL(g_chain[i].sc);
        memset(&g_chain[i], 0, sizeof(g_chain[i]));
    }
    REL(g_upload_srv);
    REL(g_upload);
    REL(g_canvas_srv);
    REL(g_canvas_rtv);
    REL(g_canvas);
    REL(g_dss);
    REL(g_bs);
    REL(g_rs);
    REL(g_samp_lin);
    REL(g_samp_point);
    REL(g_cb);
    REL(g_ps);
    REL(g_vs);
    REL(g_dc);
    REL(g_fac);
    hw_unlock();
    free(g_rgba);
    g_rgba = NULL;
    g_on = 0;
    g_have_frame = 0;
}

static void set_cb(const PCb *p)
{
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_dc, (ID3D11Resource *)g_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, p, sizeof(*p));
        ID3D11DeviceContext_Unmap(g_dc, (ID3D11Resource *)g_cb, 0);
    }
}

/* Gemeinsamer Zustand im Deferred Context (ist nach FinishCommandList leer). */
static void dc_setup(ID3D11RenderTargetView *rtv, int w, int h)
{
    D3D11_VIEWPORT vp;
    memset(&vp, 0, sizeof(vp));
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_dc, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_dc, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_dc, g_rs);
    ID3D11DeviceContext_OMSetBlendState(g_dc, g_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_dc, g_dss, 0);
    ID3D11DeviceContext_IASetInputLayout(g_dc, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_dc, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(g_dc, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_dc, g_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_dc, 0, 1, &g_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_dc, 0, 1, &g_cb);
}

/* Rechteck (Pixel x,y,w,h im Ziel tw x th) als NDC. */
static void ndc_rect(float out[4], double x, double y, double w, double h, int tw, int th)
{
    out[0] = (float)(x / tw * 2.0 - 1.0);
    out[1] = (float)(1.0 - y / th * 2.0);
    out[2] = (float)((x + w) / tw * 2.0 - 1.0);
    out[3] = (float)(1.0 - (y + h) / th * 2.0);
}

static void dc_execute(void)
{
    ID3D11CommandList *cl = NULL;
    if (SUCCEEDED(ID3D11DeviceContext_FinishCommandList(g_dc, FALSE, &cl)) && cl) {
        ID3D11DeviceContext_ExecuteCommandList(g_ctx, cl, TRUE);
        ID3D11CommandList_Release(cl);
    }
}

void hwp_frame(const unsigned char *mem, size_t mem_size, const HwpScreen scr[2],
               const uint8_t *rgb)
{
    ID3D11ShaderResourceView *srv[2] = { NULL, NULL };
    int i, need_cpu = 0, W = g_cw * g_s, H = g_ch * g_s;
    PCb cb;
    if (!g_on) return;
    hw_lock();
    for (i = 0; i < 2; i++) {
        const HwpScreen *s = &scr[i];
        int tw = 0, th = 0;
        if (s->valid && s->address != 0u && (size_t)s->address < mem_size)
            srv[i] = (ID3D11ShaderResourceView *)hw_display_lookup(
                mem + s->address, s->stride, s->format, s->columns, s->rows, &tw, &th);
        if (srv[i] && (tw != s->rows * g_s || th < s->columns * g_s)) srv[i] = NULL;
        if (srv[i]) g_n_gpu++;
        else { g_n_cpu++; need_cpu = 1; }
    }
    dc_setup(g_canvas_rtv, W, H);
    memset(&cb, 0, sizeof(cb));
    if (need_cpu) {
        /* Natives CPU-Bild (identisch zum Software-Pfad) vergroessert. */
        D3D11_MAPPED_SUBRESOURCE map;
        int x, y;
        for (y = 0; y < g_ch; y++) {
            const uint8_t *sp = rgb + (size_t)y * g_cw * 3;
            unsigned char *dp = g_rgba + (size_t)y * g_cw * 4;
            for (x = 0; x < g_cw; x++) {
                dp[x * 4 + 0] = sp[x * 3 + 0];
                dp[x * 4 + 1] = sp[x * 3 + 1];
                dp[x * 4 + 2] = sp[x * 3 + 2];
                dp[x * 4 + 3] = 255;
            }
        }
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_dc, (ID3D11Resource *)g_upload, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            for (y = 0; y < g_ch; y++)
                memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch,
                       g_rgba + (size_t)y * g_cw * 4, (size_t)g_cw * 4);
            ID3D11DeviceContext_Unmap(g_dc, (ID3D11Resource *)g_upload, 0);
        }
        ndc_rect(cb.dst, 0, 0, W, H, W, H);
        cb.m[0] = 2u;
        cb.m[1] = (uint32_t)g_s;
        set_cb(&cb);
        ID3D11DeviceContext_PSSetShaderResources(g_dc, 0, 1, &g_upload_srv);
        ID3D11DeviceContext_Draw(g_dc, 4, 0);
    } else {
        static const float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(g_dc, g_canvas_rtv, black);
    }
    for (i = 0; i < 2; i++) {
        const HwpScreen *s = &scr[i];
        if (!srv[i]) continue;
        ndc_rect(cb.dst, s->dx * g_s, s->dy * g_s, s->columns * g_s, s->rows * g_s, W, H);
        cb.m[0] = 1u;
        cb.m[1] = (uint32_t)(s->rows * g_s);
        cb.m[2] = (uint32_t)(s->dx * g_s);
        cb.m[3] = (uint32_t)(s->dy * g_s);
        set_cb(&cb);
        ID3D11DeviceContext_PSSetShaderResources(g_dc, 0, 1, &srv[i]);
        ID3D11DeviceContext_Draw(g_dc, 4, 0);
    }
    dc_execute();
    g_have_frame = 1;
    hw_unlock();
}

static HwpChain *chain_for(HWND hwnd, int cw, int ch)
{
    HwpChain *c = NULL;
    int i;
    for (i = 0; i < 2; i++)
        if (g_chain[i].sc && g_chain[i].hwnd == hwnd) c = &g_chain[i];
    if (c == NULL) {
        DXGI_SWAP_CHAIN_DESC1 d;
        for (i = 0; i < 2; i++)
            if (!g_chain[i].sc) { c = &g_chain[i]; break; }
        if (c == NULL) return NULL;
        memset(&d, 0, sizeof(d));
        d.Width = (UINT)cw;
        d.Height = (UINT)ch;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_NONE;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
        c->gdi = 1;
        if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(g_fac, (IUnknown *)g_dev, hwnd, &d,
                                                         NULL, NULL, &c->sc))) {
            d.Flags = 0;
            c->gdi = 0;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(g_fac, (IUnknown *)g_dev, hwnd,
                                                             &d, NULL, NULL, &c->sc))) {
                c->sc = NULL;
                return NULL;
            }
        }
        IDXGIFactory2_MakeWindowAssociation(g_fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
        c->hwnd = hwnd;
        c->w = cw;
        c->h = ch;
    } else if (c->w != cw || c->h != ch) {
        REL(c->rtv);
        if (FAILED(IDXGISwapChain1_ResizeBuffers(c->sc, 0, (UINT)cw, (UINT)ch,
                                                 DXGI_FORMAT_UNKNOWN,
                                                 c->gdi ? DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE : 0)))
            return NULL;
        c->w = cw;
        c->h = ch;
    }
    if (!c->rtv) {
        ID3D11Texture2D *bb = NULL;
        if (FAILED(IDXGISwapChain1_GetBuffer(c->sc, 0, &k_iid_d3d11_texture2d, (void **)&bb)))
            return NULL;
        if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb, NULL,
                                                       &c->rtv)))
            c->rtv = NULL;
        ID3D11Texture2D_Release(bb);
        if (!c->rtv) return NULL;
    }
    return c;
}

int hwp_present(void *hwnd, int cw, int ch, int dst_x, int dst_y, int dst_w, int dst_h,
                int src_x, int src_y, int src_w, int src_h,
                const char *const *lines, int nlines)
{
    static const float black[4] = { 0, 0, 0, 1 };
    HwpChain *c;
    PCb cb;
    int ok = 0;
    if (!g_on || hwnd == NULL || cw <= 0 || ch <= 0) return 0;
    hw_lock();
    c = chain_for((HWND)hwnd, cw, ch);
    if (c == NULL) goto out;
    dc_setup(c->rtv, cw, ch);
    ID3D11DeviceContext_ClearRenderTargetView(g_dc, c->rtv, black);
    if (g_have_frame && src_w > 0 && src_h > 0) {
        /* Vergroessern: naechster Nachbar (wie bisher per GDI), Verkleinern
           gegenueber der internen Aufloesung: linear. */
        double ratio = (double)dst_w / ((double)src_w * g_s);
        ID3D11SamplerState *smp = ratio < 1.0 ? g_samp_lin : g_samp_point;
        memset(&cb, 0, sizeof(cb));
        ndc_rect(cb.dst, dst_x, dst_y, dst_w, dst_h, cw, ch);
        cb.src[0] = (float)src_x / (float)g_cw;
        cb.src[1] = (float)src_y / (float)g_ch;
        cb.src[2] = (float)(src_x + src_w) / (float)g_cw;
        cb.src[3] = (float)(src_y + src_h) / (float)g_ch;
        cb.m[0] = 0u;
        set_cb(&cb);
        ID3D11DeviceContext_PSSetSamplers(g_dc, 0, 1, &smp);
        ID3D11DeviceContext_PSSetShaderResources(g_dc, 0, 1, &g_canvas_srv);
        ID3D11DeviceContext_Draw(g_dc, 4, 0);
    }
    dc_execute();
    if (c->gdi && nlines > 0) {
        IDXGISurface1 *surf = NULL;
        if (SUCCEEDED(IDXGISwapChain1_GetBuffer(c->sc, 0, &k_iid_dxgi_surface1,
                                                (void **)&surf))) {
            HDC dc = NULL;
            if (SUCCEEDED(IDXGISurface1_GetDC(surf, FALSE, &dc)) && dc) {
                int i;
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(255, 255, 255));
                for (i = 0; i < nlines; i++)
                    if (lines[i]) TextOutA(dc, 8, 8 + i * 16, lines[i], (int)strlen(lines[i]));
                IDXGISurface1_ReleaseDC(surf, NULL);
            }
            IDXGISurface1_Release(surf);
        }
    }
    ok = SUCCEEDED(IDXGISwapChain1_Present(c->sc, 0, 0));
out:
    hw_unlock();
    return ok;
}

int hwp_save_bmp(const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE map;
    FILE *f;
    int W = g_cw * g_s, H = g_ch * g_s, x, y, ok = 0;
    unsigned char hdr[54];
    int row = (W * 3 + 3) & ~3, total = 54 + row * H, off = 54, ih = 40;
    short planes = 1, bpp = 24;
    unsigned char *line;
    if (!g_on || !g_have_frame) return 0;
    memset(&td, 0, sizeof(td));
    td.Width = (UINT)W; td.Height = (UINT)H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    line = (unsigned char *)calloc((size_t)row, 1);
    if (!line) return 0;
    hw_lock();
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)g_canvas);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0,
                                              &map))) {
            f = fopen(path, "wb");
            if (f) {
                int data = row * H;
                memset(hdr, 0, sizeof(hdr));
                hdr[0] = 'B'; hdr[1] = 'M';
                memcpy(hdr + 2, &total, 4);
                memcpy(hdr + 10, &off, 4);
                memcpy(hdr + 14, &ih, 4);
                memcpy(hdr + 18, &W, 4);
                memcpy(hdr + 22, &H, 4);
                memcpy(hdr + 26, &planes, 2);
                memcpy(hdr + 28, &bpp, 2);
                memcpy(hdr + 34, &data, 4);
                fwrite(hdr, 1, 54, f);
                for (y = H - 1; y >= 0; y--) {
                    const unsigned char *sp = (const unsigned char *)map.pData +
                                              (size_t)y * map.RowPitch;
                    for (x = 0; x < W; x++) {
                        line[x * 3 + 0] = sp[x * 4 + 2];
                        line[x * 3 + 1] = sp[x * 4 + 1];
                        line[x * 3 + 2] = sp[x * 4 + 0];
                    }
                    fwrite(line, 1, (size_t)row, f);
                }
                fclose(f);
                ok = 1;
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    hw_unlock();
    free(line);
    return ok;
}

void hwp_stats(unsigned long long *gpu_screens, unsigned long long *cpu_screens)
{
    if (gpu_screens) *gpu_screens = g_n_gpu;
    if (cpu_screens) *cpu_screens = g_n_cpu;
}
