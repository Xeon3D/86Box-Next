/* d3dchk -- Direct3D 7 rasterizer conformance. Every case is drawn with
   pre-transformed (TL) vertices, so only the rasterizer is under test:
   once by the card's HAL device into a video-memory target and once by
   the DirectX "RGB Emulation" software rasterizer into a system-memory
   target (the oracle). Results are compared with a tolerance, since real
   hardware differs from software in precision, edge rules and filtering:
   PASS = close everywhere, WARN = small regions differ, FAIL = large
   regions differ (a missing or broken feature).

   The card result is drawn at the window's left, the reference at its
   right, so the run can also be watched. Needs DirectX 7 (Win98 with DX7
   installed, Win2000). Run at 16 bpp (and 32 bpp if the card can).
   Switches: /Q no pauses; /VIDTEX card textures in video memory loaded by
   Blt instead of the DirectX 7 texture manager. */
#include "ct.h"
#include <ddraw.h>
#include <d3d.h>

#define TW 256
#define TH 256

typedef struct {
    float x, y, z, rhw;
    DWORD c, s;
    float u0, v0, u1, v1;
} V;
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX2)

typedef struct {
    const GUID           *guid;
    IDirect3DDevice7     *dev;
    IDirectDrawSurface7  *rt, *z;
    D3DDEVICEDESC7        caps;
    DDPIXELFORMAT         tf[32];
    int                   ntf;
    DDPIXELFORMAT         zf;
    int                   havez;
    IDirectDrawSurface7  *tex[8];
} side;

static IDirectDraw7        *dd;
static IDirect3D7          *d3d;
static IDirectDrawSurface7 *prim;
static DDSURFACEDESC2       pdesc;
static side                 hw, sw;
static DWORD               *buf_s, *buf_r;
static int                  quick, devfound, vidtex;
static HWND                 win;
static HRESULT(WINAPI *pDDCreateEx)(GUID *, LPVOID *, REFIID, IUnknown *);

/* ---- enumeration ----------------------------------------------------- */

static HRESULT CALLBACK
dev_cb(LPSTR desc, LPSTR name, LPD3DDEVICEDESC7 d, LPVOID ctx)
{
    ct_log("INFO  device \"%s\" (%s)", name, desc);
    ct_log("INFO    raster=%08x zcmp=%08x src=%08x dst=%08x acmp=%08x shade=%08x", d->dpcTriCaps.dwRasterCaps,
           d->dpcTriCaps.dwZCmpCaps, d->dpcTriCaps.dwSrcBlendCaps, d->dpcTriCaps.dwDestBlendCaps,
           d->dpcTriCaps.dwAlphaCmpCaps, d->dpcTriCaps.dwShadeCaps);
    ct_log("INFO    tex=%08x filter=%08x blend=%08x addr=%08x max %dx%d stages=%d textures=%d",
           d->dpcTriCaps.dwTextureCaps, d->dpcTriCaps.dwTextureFilterCaps,
           d->dpcTriCaps.dwTextureBlendCaps, d->dpcTriCaps.dwTextureAddressCaps, d->dwMaxTextureWidth,
           d->dwMaxTextureHeight, d->wMaxTextureBlendStages, d->wMaxSimultaneousTextures);
    ct_log("INFO    render depths=%08x z depths=%08x texop=%08x", d->dwDeviceRenderBitDepth,
           d->dwDeviceZBufferBitDepth, d->dwTextureOpCaps);
    if (!memcmp(&d->deviceGUID, &IID_IDirect3DHALDevice, sizeof(GUID)))
        devfound |= 1;
    return D3DENUMRET_OK;
}

static HRESULT CALLBACK
zfmt_cb(LPDDPIXELFORMAT f, LPVOID ctx)
{
    side *s = ctx;

    if (f->dwFlags & DDPF_ZBUFFER && f->dwZBufferBitDepth == 16) {
        s->zf    = *f;
        s->havez = 1;
        return D3DENUMRET_CANCEL;
    }
    if (f->dwFlags & DDPF_ZBUFFER && !s->havez) {
        s->zf    = *f;
        s->havez = 1;
    }
    return D3DENUMRET_OK;
}

static HRESULT CALLBACK
tfmt_cb(LPDDPIXELFORMAT f, LPVOID ctx)
{
    side *s = ctx;

    if (s->ntf < 32 && (f->dwFlags & DDPF_RGB) && !(f->dwFlags & DDPF_PALETTEINDEXED8))
        s->tf[s->ntf++] = *f;
    return D3DENUMRET_OK;
}

static int
mshift(DWORD m)
{
    int n = 0;

    if (!m)
        return 0;
    while (!(m & 1)) {
        m >>= 1;
        n++;
    }
    return n;
}

static int
mbits(DWORD m)
{
    int n = 0;

    while (m) {
        n += m & 1;
        m >>= 1;
    }
    return n;
}

static DWORD
expand(DWORD v, DWORD mask)
{
    int n = mbits(mask);

    if (!n)
        return 0;
    v = (v & mask) >> mshift(mask);
    v <<= 8 - n;
    return (v | (v >> n)) & 0xff;
}

static DWORD
pack(const DDPIXELFORMAT *f, DWORD argb)
{
    DWORD a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255, v;

    v = ((r >> (8 - mbits(f->dwRBitMask))) << mshift(f->dwRBitMask)) |
        ((g >> (8 - mbits(f->dwGBitMask))) << mshift(f->dwGBitMask)) |
        ((b >> (8 - mbits(f->dwBBitMask))) << mshift(f->dwBBitMask));
    if (f->dwFlags & DDPF_ALPHAPIXELS && f->dwRGBAlphaBitMask)
        v |= (a >> (8 - mbits(f->dwRGBAlphaBitMask))) << mshift(f->dwRGBAlphaBitMask);
    return v;
}

/* Find a texture format by its bit layout on one side. */
static const DDPIXELFORMAT *
find_tf(side *s, int bits, DWORD a, DWORD r, DWORD g, DWORD b)
{
    int i;

    for (i = 0; i < s->ntf; i++)
        if ((int) s->tf[i].dwRGBBitCount == bits && s->tf[i].dwRBitMask == r && s->tf[i].dwGBitMask == g &&
            s->tf[i].dwBBitMask == b && ((s->tf[i].dwFlags & DDPF_ALPHAPIXELS) ? s->tf[i].dwRGBAlphaBitMask : 0) == a)
            return &s->tf[i];
    return NULL;
}

/* ---- set-up ---------------------------------------------------------- */

static IDirectDrawSurface7 *
surface(DWORD caps, DWORD caps2, int w, int h, const DDPIXELFORMAT *pf, int mips)
{
    DDSURFACEDESC2       d;
    IDirectDrawSurface7 *s = NULL;

    memset(&d, 0, sizeof(d));
    d.dwSize          = sizeof(d);
    d.dwFlags         = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (pf ? DDSD_PIXELFORMAT : 0) | (mips ? DDSD_MIPMAPCOUNT : 0);
    d.ddsCaps.dwCaps  = caps | (mips ? DDSCAPS_MIPMAP | DDSCAPS_COMPLEX : 0);
    d.ddsCaps.dwCaps2 = caps2;
    d.dwWidth         = w;
    d.dwHeight        = h;
    d.dwMipMapCount   = mips;
    if (pf)
        d.ddpfPixelFormat = *pf;
    if (IDirectDraw7_CreateSurface(dd, &d, &s, NULL) != DD_OK)
        return NULL;
    return s;
}

static int
make_side(side *s, const GUID *g, DWORD memcap)
{
    s->guid = g;
    IDirect3D7_EnumZBufferFormats(d3d, g, zfmt_cb, s);
    s->rt = surface(DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | memcap, 0, TW, TH, &pdesc.ddpfPixelFormat, 0);
    if (!s->rt)
        return 0;
    if (s->havez) {
        s->z = surface(DDSCAPS_ZBUFFER | memcap, 0, TW, TH, &s->zf, 0);
        if (s->z)
            IDirectDrawSurface7_AddAttachedSurface(s->rt, s->z);
    }
    if (IDirect3D7_CreateDevice(d3d, g, s->rt, &s->dev) != D3D_OK)
        return 0;
    IDirect3DDevice7_GetCaps(s->dev, &s->caps);
    IDirect3DDevice7_EnumTextureFormats(s->dev, tfmt_cb, s);
    return 1;
}

/* Texture patterns, generated identically for both sides. */
static DWORD
texel(int kind, int level, int x, int y)
{
    static const DWORD lv[8] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff, 0xff00ffff, 0xffffffff, 0xff808080 };

    switch (kind) {
        case 0: /* checker + gradient */
            return 0xff000000 | (((x ^ y) & 8) ? 0xe0e0e0 : 0x202020) | ((x * 4) << 16 & 0xff0000) | ((y * 4) & 0xff);
        case 1: /* alpha holes */
            return ((((x >> 3) + (y >> 3)) & 1) ? 0xff000000 : 0x40000000) | ((x * 4) << 8 & 0xff00) | 0xc00000;
        case 2: /* colour key: magenta squares */
            return ((((x >> 2) ^ (y >> 2)) & 3) == 0) ? 0xffff00ff : (0xff000000 | ((y * 4) << 8 & 0xff00) | 0x30);
        case 3: /* mip levels: one colour per level, dark grid */
            return ((x & 7) == 0 || (y & 7) == 0) ? 0xff000000 : lv[level & 7];
        default: /* stripes for the second stage */
            return (y & 4) ? 0xffffffff : 0xff4060ff;
    }
}

/* Fill every level of a texture (a mip chain walks its attached levels). */
static void
fill_tex(IDirectDrawSurface7 *t, const DDPIXELFORMAT *pf, int kind, int mips)
{
    IDirectDrawSurface7 *lvl = t, *next;
    DDSURFACEDESC2       d;
    DDSCAPS2             mc;
    int                  level = 0, x, y, bpp = pf->dwRGBBitCount / 8;

    IDirectDrawSurface7_AddRef(lvl);
    while (lvl) {
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        if (IDirectDrawSurface7_Lock(lvl, NULL, &d, DDLOCK_WAIT, NULL) == DD_OK) {
            for (y = 0; y < (int) d.dwHeight; y++)
                for (x = 0; x < (int) d.dwWidth; x++) {
                    DWORD v = pack(pf, texel(kind, level, x * 64 / d.dwWidth, y * 64 / d.dwHeight));

                    memcpy((BYTE *) d.lpSurface + y * d.lPitch + x * bpp, &v, bpp);
                }
            IDirectDrawSurface7_Unlock(lvl, NULL);
        }
        memset(&mc, 0, sizeof(mc));
        mc.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
        next      = NULL;
        if (!mips || IDirectDrawSurface7_GetAttachedSurface(lvl, &mc, &next) != DD_OK)
            next = NULL;
        IDirectDrawSurface7_Release(lvl);
        lvl = next;
        level++;
    }
}

/* Blt every level of src into dst (same shape). */
static void
blt_levels(IDirectDrawSurface7 *dst, IDirectDrawSurface7 *src)
{
    IDirectDrawSurface7 *a = dst, *b = src, *na, *nb;
    DDSCAPS2             mc;

    IDirectDrawSurface7_AddRef(a);
    IDirectDrawSurface7_AddRef(b);
    while (a && b) {
        IDirectDrawSurface7_Blt(a, NULL, b, NULL, DDBLT_WAIT, NULL);
        memset(&mc, 0, sizeof(mc));
        mc.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
        na = nb = NULL;
        if (IDirectDrawSurface7_GetAttachedSurface(a, &mc, &na) != DD_OK)
            na = NULL;
        if (IDirectDrawSurface7_GetAttachedSurface(b, &mc, &nb) != DD_OK)
            nb = NULL;
        IDirectDrawSurface7_Release(a);
        IDirectDrawSurface7_Release(b);
        a = na;
        b = nb;
    }
    if (a)
        IDirectDrawSurface7_Release(a);
    if (b)
        IDirectDrawSurface7_Release(b);
}

/* Card textures: the DirectX 7 texture manager by default; with /VIDTEX
   a video-memory texture loaded by Blt from a system-memory copy (the
   DirectX 5/6 way most games of the time used). Reference textures:
   system memory, written directly. */
static IDirectDrawSurface7 *
make_tex(side *s, const DDPIXELFORMAT *pf, int kind, int mips)
{
    IDirectDrawSurface7 *t, *stage;

    if (s == &hw && vidtex) {
        t = surface(DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY, 0, 64, 64, pf, mips);
        stage = surface(DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY, 0, 64, 64, pf, mips);
        if (!t || !stage)
            return NULL;
        fill_tex(stage, pf, kind, mips);
        blt_levels(t, stage);
        IDirectDrawSurface7_Release(stage);
    } else {
        t = surface(DDSCAPS_TEXTURE | ((s == &hw) ? 0 : DDSCAPS_SYSTEMMEMORY),
                    (s == &hw) ? DDSCAPS2_TEXTUREMANAGE : 0, 64, 64, pf, mips);
        if (!t)
            return NULL;
        fill_tex(t, pf, kind, mips);
    }
    if (kind == 2) {
        DDCOLORKEY k;

        k.dwColorSpaceLowValue = k.dwColorSpaceHighValue = pack(pf, 0xffff00ff);
        IDirectDrawSurface7_SetColorKey(t, DDCKEY_SRCBLT, &k);
    }
    return t;
}

/* ---- drawing helpers -------------------------------------------------- */

static void
vtx(V *v, float x, float y, float z, float rhw, DWORD c, DWORD s, float u, float t)
{
    v->x   = x;
    v->y   = y;
    v->z   = z;
    v->rhw = rhw;
    v->c   = c;
    v->s   = s;
    v->u0  = u;
    v->v0  = t;
    v->u1  = u * 2;
    v->v1  = t * 2;
}

static void
quad(IDirect3DDevice7 *d, float x0, float y0, float x1, float y1, DWORD c, float u0, float v0, float u1, float v1)
{
    V v[4];

    vtx(&v[0], x0, y0, 0.5f, 1, c, 0, u0, v0);
    vtx(&v[1], x1, y0, 0.5f, 1, c, 0, u1, v0);
    vtx(&v[2], x0, y1, 0.5f, 1, c, 0, u0, v1);
    vtx(&v[3], x1, y1, 0.5f, 1, c, 0, u1, v1);
    IDirect3DDevice7_DrawPrimitive(d, D3DPT_TRIANGLESTRIP, FVF, v, 4, 0);
}

/* A floor receding to the horizon: perspective-correct texturing. */
static void
floor_quad(IDirect3DDevice7 *d, DWORD c, float rep)
{
    V v[4];

    vtx(&v[0], 100, 40, 0.9f, 0.1f, c, 0, 0, 0);
    vtx(&v[1], 156, 40, 0.9f, 0.1f, c, 0, rep, 0);
    vtx(&v[2], 0, 255, 0.1f, 1.0f, c, 0, 0, rep);
    vtx(&v[3], 255, 255, 0.1f, 1.0f, c, 0, rep, rep);
    IDirect3DDevice7_DrawPrimitive(d, D3DPT_TRIANGLESTRIP, FVF, v, 4, 0);
}

static void
rs(IDirect3DDevice7 *d, D3DRENDERSTATETYPE s, DWORD v)
{
    IDirect3DDevice7_SetRenderState(d, s, v);
}

static void
ts(IDirect3DDevice7 *d, DWORD st, D3DTEXTURESTAGESTATETYPE s, DWORD v)
{
    IDirect3DDevice7_SetTextureStageState(d, st, s, v);
}

static void
defaults(IDirect3DDevice7 *d)
{
    rs(d, D3DRENDERSTATE_DITHERENABLE, FALSE);
    rs(d, D3DRENDERSTATE_ZENABLE, FALSE);
    rs(d, D3DRENDERSTATE_ZWRITEENABLE, TRUE);
    rs(d, D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
    rs(d, D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    rs(d, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    rs(d, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
    rs(d, D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
    rs(d, D3DRENDERSTATE_COLORKEYENABLE, FALSE);
    rs(d, D3DRENDERSTATE_FOGENABLE, FALSE);
    rs(d, D3DRENDERSTATE_SPECULARENABLE, FALSE);
    rs(d, D3DRENDERSTATE_TEXTUREPERSPECTIVE, TRUE);
    rs(d, D3DRENDERSTATE_LIGHTING, FALSE);
    ts(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    ts(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    ts(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    ts(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    ts(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    ts(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    ts(d, 0, D3DTSS_MAGFILTER, D3DTFG_POINT);
    ts(d, 0, D3DTSS_MINFILTER, D3DTFN_POINT);
    ts(d, 0, D3DTSS_MIPFILTER, D3DTFP_NONE);
    ts(d, 0, D3DTSS_ADDRESS, D3DTADDRESS_WRAP);
    ts(d, 0, D3DTSS_TEXCOORDINDEX, 0);
    ts(d, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    ts(d, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice7_SetTexture(d, 0, NULL);
    IDirect3DDevice7_SetTexture(d, 1, NULL);
}

/* ---- cases ------------------------------------------------------------ */

/* Every case draws on device d of side s; returns 0 to skip (with *why). */
typedef int (*d3dcase)(side *s, IDirect3DDevice7 *d, const char **why);

static int
k_flat(side *s, IDirect3DDevice7 *d, const char **why)
{
    V   v[3];
    int i;

    for (i = 0; i < 2; i++) {
        rs(d, D3DRENDERSTATE_SHADEMODE, i ? D3DSHADE_GOURAUD : D3DSHADE_FLAT);
        vtx(&v[0], 10 + i * 120, 10, 0.5f, 1, 0xffff0000, 0, 0, 0);
        vtx(&v[1], 120 + i * 120, 60, 0.5f, 1, 0xff00ff00, 0, 0, 0);
        vtx(&v[2], 40 + i * 120, 240, 0.5f, 1, 0xff0000ff, 0, 0, 0);
        IDirect3DDevice7_DrawPrimitive(d, D3DPT_TRIANGLELIST, FVF, v, 3, 0);
    }
    /* A fan of thin slivers: edge rules and long skinny spans. */
    for (i = 0; i < 24; i++) {
        vtx(&v[0], 128, 250, 0.5f, 1, 0xffffffff, 0, 0, 0);
        vtx(&v[1], 5.0f + i * 10.5f, 200, 0.5f, 1, 0xff000000 | (i * 0x0a0b0c), 0, 0, 0);
        vtx(&v[2], 10.0f + i * 10.5f, 200, 0.5f, 1, 0xff808080, 0, 0, 0);
        IDirect3DDevice7_DrawPrimitive(d, D3DPT_TRIANGLELIST, FVF, v, 3, 0);
    }
    return 1;
}

static int
k_zbuf(side *s, IDirect3DDevice7 *d, const char **why)
{
    V   v[3];
    int i;

    if (!s->z) {
        *why = "no z-buffer";
        return 0;
    }
    rs(d, D3DRENDERSTATE_ZENABLE, TRUE);
    /* Interpenetrating triangles: z varies across each one. */
    for (i = 0; i < 6; i++) {
        float o = i * 30.0f;

        vtx(&v[0], 10 + o, 20, 0.1f + i * 0.1f, 1, 0xff000000 | (0x3f << (i % 3 * 8)) * (1 + i / 3), 0, 0, 0);
        vtx(&v[1], 160 + o * 0.3f, 128, 0.9f - i * 0.1f, 1, 0xffffffff, 0, 0, 0);
        vtx(&v[2], 30 + o, 240, 0.5f, 1, 0xff000000 | (0x7f << (i % 3 * 8)), 0, 0, 0);
        IDirect3DDevice7_DrawPrimitive(d, D3DPT_TRIANGLELIST, FVF, v, 3, 0);
    }
    return 1;
}

static int
k_zfunc(side *s, IDirect3DDevice7 *d, const char **why)
{
    int f;

    if (!s->z) {
        *why = "no z-buffer";
        return 0;
    }
    rs(d, D3DRENDERSTATE_ZENABLE, TRUE);
    rs(d, D3DRENDERSTATE_ZFUNC, D3DCMP_ALWAYS);
    /* Base layer: z ramps 0..1 left to right. */
    {
        V v[4];

        vtx(&v[0], 0, 0, 0.0f, 1, 0xff404040, 0, 0, 0);
        vtx(&v[1], 256, 0, 1.0f, 1, 0xff404040, 0, 0, 0);
        vtx(&v[2], 0, 256, 0.0f, 1, 0xff404040, 0, 0, 0);
        vtx(&v[3], 256, 256, 1.0f, 1, 0xff404040, 0, 0, 0);
        IDirect3DDevice7_DrawPrimitive(d, D3DPT_TRIANGLESTRIP, FVF, v, 4, 0);
    }
    /* Eight bands at constant z = 0.5, one per compare function. */
    for (f = D3DCMP_NEVER; f <= D3DCMP_ALWAYS; f++) {
        rs(d, D3DRENDERSTATE_ZFUNC, f);
        quad(d, 0, (f - 1) * 32 + 2, 256, f * 32 - 2, 0xff000000 | (f * 0x1f2f3f), 0, 0, 0, 0);
    }
    return 1;
}

static int
use_tex(side *s, IDirect3DDevice7 *d, int slot, const char **why)
{
    if (!s->tex[slot]) {
        *why = "texture format not offered";
        return 0;
    }
    IDirect3DDevice7_SetTexture(d, 0, s->tex[slot]);
    ts(d, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    ts(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    return 1;
}

static int
k_texpoint(side *s, IDirect3DDevice7 *d, const char **why)
{
    if (!use_tex(s, d, 0, why))
        return 0;
    quad(d, 0, 0, 128, 128, 0xffffffff, 0, 0, 1, 1);       /* 2x magnified */
    quad(d, 128, 0, 160, 32, 0xffffffff, 0, 0, 1, 1);      /* minified */
    quad(d, 0, 128, 256, 256, 0xffffff80, 0.3f, 0.2f, 2.3f, 1.2f); /* modulated, repeated */
    return 1;
}

static int
k_texbilin(side *s, IDirect3DDevice7 *d, const char **why)
{
    if (!use_tex(s, d, 0, why))
        return 0;
    ts(d, 0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
    ts(d, 0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    quad(d, 0, 0, 256, 256, 0xffffffff, 0, 0, 1, 1); /* 4x magnified */
    return 1;
}

static int
k_persp(side *s, IDirect3DDevice7 *d, const char **why)
{
    if (!use_tex(s, d, 0, why))
        return 0;
    floor_quad(d, 0xffffffff, 4);
    return 1;
}

static int
k_alphablend(side *s, IDirect3DDevice7 *d, const char **why)
{
    int i;

    quad(d, 0, 0, 256, 128, 0xff2050ff, 0, 0, 0, 0);
    quad(d, 0, 128, 256, 256, 0xffffe020, 0, 0, 0, 0);
    rs(d, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    rs(d, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    rs(d, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    for (i = 0; i < 8; i++)
        quad(d, 10 + i * 30, 20, 40 + i * 30, 236, ((DWORD) (i * 36) << 24) | 0xff2020, 0, 0, 0, 0);
    rs(d, D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE);
    rs(d, D3DRENDERSTATE_DESTBLEND, D3DBLEND_ONE);
    quad(d, 60, 90, 200, 170, 0xff404040, 0, 0, 0, 0); /* additive */
    return 1;
}

static int
k_blendmodes(side *s, IDirect3DDevice7 *d, const char **why)
{
    static const DWORD f[7] = { D3DBLEND_ZERO, D3DBLEND_ONE, D3DBLEND_SRCCOLOR, D3DBLEND_INVSRCCOLOR,
                                D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLEND_DESTCOLOR };
    int                i, j;

    for (i = 0; i < 7; i++)
        for (j = 0; j < 7; j++) {
            DWORD sb = 1u << (f[i] - 1), db = 1u << (f[j] - 1);

            rs(d, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
            quad(d, j * 36, i * 36, j * 36 + 34, i * 36 + 34, 0xff000000 | (0x30 + j * 0x18) << 8 | 0x804000, 0, 0, 0, 0);
            /* Only combinations both devices claim. */
            if (!(hw.caps.dpcTriCaps.dwSrcBlendCaps & sb) || !(hw.caps.dpcTriCaps.dwDestBlendCaps & db))
                continue;
            rs(d, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
            rs(d, D3DRENDERSTATE_SRCBLEND, f[i]);
            rs(d, D3DRENDERSTATE_DESTBLEND, f[j]);
            quad(d, j * 36 + 4, i * 36 + 4, j * 36 + 30, i * 36 + 30, 0x80c040a0, 0, 0, 0, 0);
        }
    return 1;
}

static int
k_alphatest(side *s, IDirect3DDevice7 *d, const char **why)
{
    quad(d, 0, 0, 256, 256, 0xff2040a0, 0, 0, 0, 0);
    if (!use_tex(s, d, 1, why))
        return 0;
    rs(d, D3DRENDERSTATE_ALPHATESTENABLE, TRUE);
    rs(d, D3DRENDERSTATE_ALPHAREF, 0x80);
    rs(d, D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATER);
    quad(d, 0, 0, 128, 256, 0xffffffff, 0, 0, 2, 4);
    /* Blended instead of tested on the right. */
    rs(d, D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
    rs(d, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    rs(d, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    rs(d, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    quad(d, 128, 0, 256, 256, 0xffffffff, 0, 0, 2, 4);
    return 1;
}

static int
k_ckey(side *s, IDirect3DDevice7 *d, const char **why)
{
    quad(d, 0, 0, 256, 256, 0xff30a030, 0, 0, 0, 0);
    if (!use_tex(s, d, 2, why))
        return 0;
    rs(d, D3DRENDERSTATE_COLORKEYENABLE, TRUE);
    quad(d, 0, 0, 256, 256, 0xffffffff, 0, 0, 1, 1);
    return 1;
}

static int
k_fog(side *s, IDirect3DDevice7 *d, const char **why)
{
    V   v[4];
    int i;

    if (!use_tex(s, d, 0, why))
        return 0;
    rs(d, D3DRENDERSTATE_FOGENABLE, TRUE);
    rs(d, D3DRENDERSTATE_FOGCOLOR, 0x00a0b0c0);
    rs(d, D3DRENDERSTATE_FOGVERTEXMODE, D3DFOG_NONE);
    rs(d, D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_NONE);
    /* Vertex fog: the specular alpha is the fog factor. */
    for (i = 0; i < 4; i++) {
        vtx(&v[i], (i & 1) ? 256.0f : 0.0f, (i & 2) ? 128.0f : 0.0f, 0.5f, 1, 0xffffffff,
            (i & 2) ? 0x00000000 : 0xff000000, (i & 1) ? 2.0f : 0.0f, (i & 2) ? 1.0f : 0.0f);
    }
    IDirect3DDevice7_DrawPrimitive(d, D3DPT_TRIANGLESTRIP, FVF, v, 4, 0);
    return 1;
}

static int
k_specular(side *s, IDirect3DDevice7 *d, const char **why)
{
    V   v[4];
    int i;

    if (!use_tex(s, d, 0, why))
        return 0;
    rs(d, D3DRENDERSTATE_SPECULARENABLE, TRUE);
    for (i = 0; i < 4; i++)
        vtx(&v[i], (i & 1) ? 256.0f : 0.0f, (i & 2) ? 256.0f : 0.0f, 0.5f, 1, 0xff8080ff,
            (i == 3) ? 0x00ffffff : (i == 0) ? 0x00000000 : 0x00603000, (i & 1) ? 1.0f : 0.0f, (i & 2) ? 1.0f : 0.0f);
    IDirect3DDevice7_DrawPrimitive(d, D3DPT_TRIANGLESTRIP, FVF, v, 4, 0);
    return 1;
}

static int
k_texaddr(side *s, IDirect3DDevice7 *d, const char **why)
{
    static const DWORD modes[3] = { D3DTADDRESS_WRAP, D3DTADDRESS_MIRROR, D3DTADDRESS_CLAMP };
    static const DWORD caps[3]  = { D3DPTADDRESSCAPS_WRAP, D3DPTADDRESSCAPS_MIRROR, D3DPTADDRESSCAPS_CLAMP };
    int                i;

    if (!use_tex(s, d, 0, why))
        return 0;
    for (i = 0; i < 3; i++) {
        if (!(hw.caps.dpcTriCaps.dwTextureAddressCaps & caps[i]))
            continue;
        ts(d, 0, D3DTSS_ADDRESS, modes[i]);
        quad(d, i * 85, 0, i * 85 + 84, 256, 0xffffffff, -1, -1, 2, 2);
    }
    return 1;
}

static int
k_texops(side *s, IDirect3DDevice7 *d, const char **why)
{
    static const DWORD ops[4] = { D3DTOP_SELECTARG1, D3DTOP_MODULATE, D3DTOP_ADD, D3DTOP_MODULATE2X };
    static const DWORD opc[4] = { D3DTEXOPCAPS_SELECTARG1, D3DTEXOPCAPS_MODULATE, D3DTEXOPCAPS_ADD, D3DTEXOPCAPS_MODULATE2X };
    int                i;

    if (!use_tex(s, d, 0, why))
        return 0;
    for (i = 0; i < 4; i++) {
        if (!(hw.caps.dwTextureOpCaps & opc[i]))
            continue;
        ts(d, 0, D3DTSS_COLOROP, ops[i]);
        quad(d, (i & 1) * 128, (i >> 1) * 128, (i & 1) * 128 + 128, (i >> 1) * 128 + 128, 0xff4080c0, 0, 0, 1, 1);
    }
    return 1;
}

static int
k_mipmap(side *s, IDirect3DDevice7 *d, const char **why)
{
    if (!(hw.caps.dpcTriCaps.dwTextureFilterCaps & (D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPNEAREST))) {
        *why = "no mipmapping in caps";
        return 0;
    }
    if (!use_tex(s, d, 3, why))
        return 0;
    ts(d, 0, D3DTSS_MIPFILTER, D3DTFP_POINT);
    floor_quad(d, 0xffffffff, 8);
    return 1;
}

static int
k_trilinear(side *s, IDirect3DDevice7 *d, const char **why)
{
    if (!(hw.caps.dpcTriCaps.dwTextureFilterCaps & (D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_LINEARMIPLINEAR))) {
        *why = "no trilinear in caps";
        return 0;
    }
    if (!use_tex(s, d, 3, why))
        return 0;
    ts(d, 0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
    ts(d, 0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    ts(d, 0, D3DTSS_MIPFILTER, D3DTFP_LINEAR);
    floor_quad(d, 0xffffffff, 8);
    return 1;
}

static int
k_multitex(side *s, IDirect3DDevice7 *d, const char **why)
{
    if (hw.caps.wMaxSimultaneousTextures < 2) {
        *why = "one texture per pass";
        return 0;
    }
    if (!use_tex(s, d, 0, why))
        return 0;
    if (!s->tex[4]) {
        *why = "no second texture";
        return 0;
    }
    IDirect3DDevice7_SetTexture(d, 1, s->tex[4]);
    ts(d, 1, D3DTSS_TEXCOORDINDEX, 1);
    ts(d, 1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    ts(d, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    ts(d, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    ts(d, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    ts(d, 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
    quad(d, 0, 0, 256, 256, 0xffffffff, 0, 0, 1, 1);
    return 1;
}

/* tol = per-channel tolerance, radius = position slack in pixels (edge
   rules, half-texel offsets), warn/fail = per-mille of bad pixels. */
static const struct {
    const char *name;
    d3dcase     fn;
    int         tol, radius, warn, fail;
} cases[] = {
    { "FLATGOUR", k_flat, 24, 1, 5, 30 },       { "ZBUFFER", k_zbuf, 24, 1, 5, 30 },
    { "ZFUNC", k_zfunc, 24, 1, 5, 30 },         { "TEXPOINT", k_texpoint, 24, 1, 5, 30 },
    { "TEXBILIN", k_texbilin, 32, 2, 5, 30 },   { "PERSP", k_persp, 40, 2, 10, 50 },
    { "ALPHABLD", k_alphablend, 24, 1, 5, 30 }, { "BLENDMOD", k_blendmodes, 24, 1, 5, 30 },
    { "ALPHATST", k_alphatest, 24, 1, 5, 30 },  { "COLORKEY", k_ckey, 24, 1, 5, 30 },
    { "FOG", k_fog, 32, 2, 10, 50 },            { "SPECULAR", k_specular, 32, 1, 5, 30 },
    { "TEXADDR", k_texaddr, 24, 1, 5, 30 },     { "TEXOPS", k_texops, 24, 1, 5, 30 },
    { "MIPMAP", k_mipmap, 40, 2, 20, 100 },     { "TRILIN", k_trilinear, 48, 2, 20, 100 },
    { "MULTITEX", k_multitex, 32, 1, 5, 30 },
};

static void
read_rt(IDirectDrawSurface7 *rt, DWORD *out)
{
    DDSURFACEDESC2 d;
    int            x, y, bpp;

    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    if (IDirectDrawSurface7_Lock(rt, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL) != DD_OK)
        return;
    bpp = d.ddpfPixelFormat.dwRGBBitCount / 8;
    for (y = 0; y < TH; y++)
        for (x = 0; x < TW; x++) {
            DWORD p = 0;

            memcpy(&p, (BYTE *) d.lpSurface + y * d.lPitch + x * bpp, bpp);
            out[y * TW + x] = (expand(p, d.ddpfPixelFormat.dwRBitMask) << 16) |
                              (expand(p, d.ddpfPixelFormat.dwGBitMask) << 8) | expand(p, d.ddpfPixelFormat.dwBBitMask);
        }
    IDirectDrawSurface7_Unlock(rt, NULL);
}

static int
draw(side *s, int i, const char **why)
{
    IDirect3DDevice7 *d = s->dev;
    int               ok;

    IDirect3DDevice7_Clear(d, 0, NULL, D3DCLEAR_TARGET | (s->z ? D3DCLEAR_ZBUFFER : 0), 0xff000000, 1.0f, 0);
    if (IDirect3DDevice7_BeginScene(d) != D3D_OK) {
        *why = "BeginScene failed";
        return 0;
    }
    defaults(d);
    ok = cases[i].fn(s, d, why);
    IDirect3DDevice7_EndScene(d);
    return ok;
}

static void
show(void)
{
    RECT a, b;

    SetRect(&a, 0, 0, TW, TH);
    SetRect(&b, 0, 0, TW, TH);
    IDirectDrawSurface7_Blt(prim, &b, hw.rt, &a, DDBLT_WAIT, NULL);
    OffsetRect(&b, TW + 8, 0);
    IDirectDrawSurface7_Blt(prim, &b, sw.rt, &a, DDBLT_WAIT, NULL);
}

void __stdcall
entry(void)
{
    DDSURFACEDESC2       d;
    const DDPIXELFORMAT *f, *g;
    const char          *why;
    int                  i, k;
    static const struct {
        int   bits;
        DWORD a, r, g, b;
    } want[5] = {
        { 16, 0, 0xf800, 0x07e0, 0x001f },       /* 0 checker  RGB565   */
        { 16, 0xf000, 0x0f00, 0x00f0, 0x000f },  /* 1 alpha    ARGB4444 */
        { 16, 0, 0xf800, 0x07e0, 0x001f },       /* 2 ckey     RGB565   */
        { 16, 0, 0xf800, 0x07e0, 0x001f },       /* 3 mips     RGB565   */
        { 16, 0x8000, 0x7c00, 0x03e0, 0x001f },  /* 4 stripes  ARGB1555 */
    };

    ct_open("D3DCHK");
    quick  = ct_flag("/Q");
    vidtex = ct_flag("/VIDTEX");
    /* Looked up at run time so the program still starts (and reports a
       skip) where DirectX 7 is missing. */
    pDDCreateEx = (HRESULT(WINAPI *)(GUID *, LPVOID *, REFIID, IUnknown *)) GetProcAddress(
        LoadLibraryA("DDRAW.DLL"), "DirectDrawCreateEx");
    if (!pDDCreateEx || pDDCreateEx(NULL, (void **) &dd, &IID_IDirectDraw7, NULL) != DD_OK ||
        IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **) &d3d) != DD_OK) {
        ct_skip("ALL", "DirectX 7 is not installed");
        ct_close();
        ExitProcess(2);
    }
    IDirect3D7_EnumDevices(d3d, dev_cb, NULL);
    win = ct_window(TW * 2 + 8, TH);
    IDirectDraw7_SetCooperativeLevel(dd, win, DDSCL_NORMAL);
    memset(&d, 0, sizeof(d));
    d.dwSize         = sizeof(d);
    d.dwFlags        = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDraw7_CreateSurface(dd, &d, &prim, NULL);
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.dwSize = sizeof(pdesc);
    IDirectDrawSurface7_GetSurfaceDesc(prim, &pdesc);
    ct_log("INFO  primary %d bpp", pdesc.ddpfPixelFormat.dwRGBBitCount);
    if (pdesc.ddpfPixelFormat.dwRGBBitCount < 16) {
        ct_skip("ALL", "desktop must be 16 bpp or more");
        goto done;
    }
    if (!(devfound & 1) || !make_side(&hw, &IID_IDirect3DHALDevice, DDSCAPS_VIDEOMEMORY)) {
        ct_skip("ALL", "no HAL device at this colour depth");
        goto done;
    }
    if (!make_side(&sw, &IID_IDirect3DRGBDevice, DDSCAPS_SYSTEMMEMORY)) {
        ct_skip("ALL", "cannot create the RGB reference device");
        goto done;
    }
    ct_log("INFO  card textures: %s", vidtex ? "video memory, loaded by Blt (/VIDTEX)" : "DirectX 7 texture manager");
    ct_log("INFO  z-buffer: card %d bits, reference %d bits", hw.havez ? hw.zf.dwZBufferBitDepth : 0,
           sw.havez ? sw.zf.dwZBufferBitDepth : 0);
    for (k = 0; k < 5; k++) {
        f = find_tf(&hw, want[k].bits, want[k].a, want[k].r, want[k].g, want[k].b);
        g = find_tf(&sw, want[k].bits, want[k].a, want[k].r, want[k].g, want[k].b);
        if (f && g) {
            hw.tex[k] = make_tex(&hw, f, k, k == 3 ? 7 : 0);
            sw.tex[k] = make_tex(&sw, g, k, k == 3 ? 7 : 0);
            if (!hw.tex[k] || !sw.tex[k])
                hw.tex[k] = sw.tex[k] = NULL;
        }
        ct_log("INFO  texture %d: %s", k, hw.tex[k] ? "ok" : "format not offered by the card");
    }
    buf_s = (DWORD *) GlobalAlloc(GPTR, TW * TH * 4);
    buf_r = (DWORD *) GlobalAlloc(GPTR, TW * TH * 4);
    for (i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++) {
        why = "";
        if (!draw(&hw, i, &why)) {
            ct_skip(cases[i].name, why);
            continue;
        }
        draw(&sw, i, &why);
        read_rt(hw.rt, buf_s);
        read_rt(sw.rt, buf_r);
        show();
        ct_grade_r(cases[i].name, buf_s, buf_r, TW, TH, cases[i].tol, cases[i].radius, cases[i].warn, cases[i].fail, "");
        ct_pump(quick ? 50 : 700);
    }
done:
    ct_close();
    ExitProcess(ct_nfail ? 1 : 0);
}
