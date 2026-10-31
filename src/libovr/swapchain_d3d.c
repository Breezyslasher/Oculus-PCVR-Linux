#include "swapchain_d3d.h"

#define COBJMACROS
#include <d3d11.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../common/log.h"

#define LOG(...) stub_log("swapchain", __VA_ARGS__)
#define MAX_CHAIN_LENGTH 3
#define MAX_LIVE_CHAINS 64

struct ovrTextureSwapChainData {
    ID3D11Texture2D *textures[MAX_CHAIN_LENGTH];
    int gl;                             /* an OpenGL chain (gl_tex, no textures) */
    unsigned gl_tex[MAX_CHAIN_LENGTH];  /* GL texture names */
    int length;
    int index;          /* next buffer the game will render into */
    int committed;      /* last index committed, or -1 */
    int width, height;
    DXGI_FORMAT format;
};

/* Map a LibOVR texture format to a DXGI format. With the Typeless misc flag
 * the game wants a typeless texture it will view as it chooses; otherwise use
 * the exact (possibly sRGB) format. */
static DXGI_FORMAT dxgi_format(ovrTextureFormat fmt, unsigned misc)
{
    int typeless = (misc & ovrTextureMisc_DX_Typeless) != 0;
    switch (fmt) {
    case OVR_FORMAT_R8G8B8A8_UNORM:
    case OVR_FORMAT_R8G8B8A8_UNORM_SRGB:
        return typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS
             : fmt == OVR_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
             : DXGI_FORMAT_R8G8B8A8_UNORM;
    case OVR_FORMAT_B8G8R8A8_UNORM:
    case OVR_FORMAT_B8G8R8A8_UNORM_SRGB:
        return typeless ? DXGI_FORMAT_B8G8R8A8_TYPELESS
             : fmt == OVR_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
             : DXGI_FORMAT_B8G8R8A8_UNORM;
    case OVR_FORMAT_B8G8R8X8_UNORM:
    case OVR_FORMAT_B8G8R8X8_UNORM_SRGB:
        return typeless ? DXGI_FORMAT_B8G8R8X8_TYPELESS
             : fmt == OVR_FORMAT_B8G8R8X8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB
             : DXGI_FORMAT_B8G8R8X8_UNORM;
    case OVR_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case OVR_FORMAT_R11G11B10_FLOAT:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    /* Depth chains (Unreal and newer Unity ask for one per eye buffer). A
     * typeless one lets the game view it both as depth and as a shader
     * input; otherwise it gets the exact depth format. */
    case OVR_FORMAT_D16_UNORM:
        return typeless ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_D16_UNORM;
    case OVR_FORMAT_D24_UNORM_S8_UINT:
        return typeless ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
    case OVR_FORMAT_D32_FLOAT:
        return typeless ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
    case OVR_FORMAT_D32_FLOAT_S8X24_UINT:
        return typeless ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
        return typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static UINT bind_flags(unsigned ovr_bind)
{
    UINT f = 0;
    if (ovr_bind & ovrTextureBind_DX_RenderTarget)
        f |= D3D11_BIND_RENDER_TARGET;
    if (ovr_bind & ovrTextureBind_DX_UnorderedAccess)
        f |= D3D11_BIND_UNORDERED_ACCESS;
    if (ovr_bind & ovrTextureBind_DX_DepthStencil)
        f |= D3D11_BIND_DEPTH_STENCIL;
    /* Games expect to sample the submitted image, and the compositor reads it. */
    f |= D3D11_BIND_SHADER_RESOURCE;
    return f;
}

static void *g_live[MAX_LIVE_CHAINS];
static SRWLOCK g_live_lock = SRWLOCK_INIT;

static void live_set(void *chain, void *value)
{
    AcquireSRWLockExclusive(&g_live_lock);
    for (int i = 0; i < MAX_LIVE_CHAINS; i++)
        if (g_live[i] == chain) {
            g_live[i] = value;
            break;
        }
    ReleaseSRWLockExclusive(&g_live_lock);
}

int swapchain_d3d_known(ovrTextureSwapChain chain)
{
    int found = 0;
    if (!chain)
        return 0;
    AcquireSRWLockShared(&g_live_lock);
    for (int i = 0; i < MAX_LIVE_CHAINS && !found; i++)
        found = g_live[i] == (void *)chain;
    ReleaseSRWLockShared(&g_live_lock);
    return found;
}

ovrTextureSwapChain swapchain_d3d_create(void *d3d_device,
                                         const ovrTextureSwapChainDesc *desc)
{
    ID3D11Device *dev = (ID3D11Device *)d3d_device;
    if (!dev || !desc || desc->Width <= 0 || desc->Height <= 0)
        return NULL;

    struct ovrTextureSwapChainData *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->length = desc->StaticImage ? 1 : 2;
    if (c->length > MAX_CHAIN_LENGTH)
        c->length = MAX_CHAIN_LENGTH;
    c->index = 0;
    c->committed = -1;
    c->width = desc->Width;
    c->height = desc->Height;

    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width = desc->Width;
    td.Height = desc->Height;
    td.MipLevels = desc->MipLevels > 0 ? desc->MipLevels : 1;
    td.ArraySize = desc->ArraySize > 0 ? desc->ArraySize : 1;
    td.Format = dxgi_format(desc->Format, desc->MiscFlags);
    td.SampleDesc.Count = desc->SampleCount > 0 ? desc->SampleCount : 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind_flags(desc->BindFlags);
    /* A typed depth format can't also be a shader resource. */
    if ((desc->BindFlags & ovrTextureBind_DX_DepthStencil) &&
        !(desc->MiscFlags & ovrTextureMisc_DX_Typeless))
        td.BindFlags &= ~(UINT)D3D11_BIND_SHADER_RESOURCE;
    /* Shared so SteamVR's out-of-process compositor can open the texture. */
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    /* Under Proton the compositor reads textures in-process through DXVK, so
     * sharing isn't needed; OVRSTUB_NO_SHARED=1 drops it for testing. */
    const char *ns = getenv("OVRSTUB_NO_SHARED");
    if (ns && *ns == '1')
        td.MiscFlags = 0;
    c->format = td.Format;

    for (int i = 0; i < c->length; i++) {
        HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &c->textures[i]);
        if (FAILED(hr) && (td.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) {
            /* Some formats/flags reject SHARED; retry without it. */
            td.MiscFlags = 0;
            hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &c->textures[i]);
        }
        if (FAILED(hr)) {
            LOG("CreateTexture2D failed (hr 0x%08lx, %dx%d fmt %d)",
                (unsigned long)hr, desc->Width, desc->Height, (int)td.Format);
            swapchain_d3d_destroy(c);
            return NULL;
        }
    }
    live_set(NULL, c);  /* claim a free slot */
    LOG("created %d-texture swap chain %dx%d fmt %d (shared=%d)",
        c->length, desc->Width, desc->Height, (int)td.Format,
        td.MiscFlags == D3D11_RESOURCE_MISC_SHARED);
    return (ovrTextureSwapChain)c;
}

/* ---- OpenGL chains --------------------------------------------------------
 * Same ring and bookkeeping, holding GL texture names instead. The game makes
 * its GL context current before creating, committing and destroying chains,
 * so the textures live in its context. GL entry points come from the game's
 * opengl32.dll at run time, so D3D games never load it. */
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_FLOAT_32_UNSIGNED_INT_24_8_REV 0x8DAD
#define GL_UNSIGNED_SHORT 0x1403
#define GL_RGB 0x1907
#define GL_UNSIGNED_INT_10F_11F_11F_REV 0x8C3B
#define GL_RGBA8 0x8058
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_RGBA16F 0x881A
#define GL_R11F_G11F_B10F 0x8C3A
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH32F_STENCIL8 0x8CAD

static struct {
    int loaded;
    void (WINAPI *GenTextures)(int, unsigned *);
    void (WINAPI *DeleteTextures)(int, const unsigned *);
    void (WINAPI *BindTexture)(unsigned, unsigned);
    void (WINAPI *TexParameteri)(unsigned, unsigned, int);
    void (WINAPI *TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned,
                              const void *);
    void (WINAPI *GetIntegerv)(unsigned, int *);
    void (WINAPI *TexStorage2D)(unsigned, int, unsigned, int, int);
} gl;

static int gl_load(void)
{
    if (gl.loaded)
        return gl.loaded > 0;
    HMODULE m = LoadLibraryW(L"opengl32.dll");
    PROC (WINAPI *get_proc)(LPCSTR) = m ? (void *)GetProcAddress(m, "wglGetProcAddress") : NULL;
#define GLFN(field, name) gl.field = m ? (void *)GetProcAddress(m, name) : NULL
    GLFN(GenTextures, "glGenTextures");
    GLFN(DeleteTextures, "glDeleteTextures");
    GLFN(BindTexture, "glBindTexture");
    GLFN(TexParameteri, "glTexParameteri");
    GLFN(TexImage2D, "glTexImage2D");
    GLFN(GetIntegerv, "glGetIntegerv");
#undef GLFN
    /* GL 4.2 / ARB_texture_storage: only through wglGetProcAddress, which
     * needs the game's context current (it is, when it creates a chain). */
    gl.TexStorage2D = get_proc ? (void *)get_proc("glTexStorage2D") : NULL;
    gl.loaded = gl.GenTextures && gl.DeleteTextures && gl.BindTexture &&
                gl.TexParameteri && gl.TexImage2D && gl.GetIntegerv ? 1 : -1;
    if (gl.loaded < 0)
        LOG("OpenGL entry points not found in opengl32.dll");
    return gl.loaded > 0;
}

/* GL sized internal format, plus the format/type pair glTexImage2D wants. */
static unsigned gl_internal_format(ovrTextureFormat f, unsigned *fmt, unsigned *type)
{
    *fmt = GL_RGBA;
    *type = GL_UNSIGNED_BYTE;
    switch (f) {
    case OVR_FORMAT_R8G8B8A8_UNORM_SRGB:
    case OVR_FORMAT_B8G8R8A8_UNORM_SRGB:
    case OVR_FORMAT_B8G8R8X8_UNORM_SRGB:
        return GL_SRGB8_ALPHA8;
    case OVR_FORMAT_R16G16B16A16_FLOAT:
        *type = GL_HALF_FLOAT;
        return GL_RGBA16F;
    case OVR_FORMAT_R11G11B10_FLOAT:
        *fmt = GL_RGB;
        *type = GL_UNSIGNED_INT_10F_11F_11F_REV;
        return GL_R11F_G11F_B10F;
    case OVR_FORMAT_D16_UNORM:
        *fmt = GL_DEPTH_COMPONENT;
        *type = GL_UNSIGNED_SHORT;
        return GL_DEPTH_COMPONENT16;
    case OVR_FORMAT_D24_UNORM_S8_UINT:
        *fmt = GL_DEPTH_STENCIL;
        *type = GL_UNSIGNED_INT_24_8;
        return GL_DEPTH24_STENCIL8;
    case OVR_FORMAT_D32_FLOAT:
        *fmt = GL_DEPTH_COMPONENT;
        *type = GL_FLOAT;
        return GL_DEPTH_COMPONENT32F;
    case OVR_FORMAT_D32_FLOAT_S8X24_UINT:
        *fmt = GL_DEPTH_STENCIL;
        *type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
        return GL_DEPTH32F_STENCIL8;
    default:   /* RGBA8 / BGRA8 / BGRX8 unorm: GL keeps no BGRA internal format */
        return GL_RGBA8;
    }
}

ovrTextureSwapChain swapchain_gl_create(const ovrTextureSwapChainDesc *desc)
{
    if (!desc || desc->Width <= 0 || desc->Height <= 0 || !gl_load())
        return NULL;
    if (desc->ArraySize > 1 || desc->SampleCount > 1)
        LOG("GL swap chain: array size %d / %d samples not supported; making a "
            "plain 2D texture", desc->ArraySize, desc->SampleCount);
    struct ovrTextureSwapChainData *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->gl = 1;
    c->length = desc->StaticImage ? 1 : 2;
    c->committed = -1;
    c->width = desc->Width;
    c->height = desc->Height;
    int levels = desc->MipLevels > 0 ? desc->MipLevels : 1;
    unsigned fmt, type, internal = gl_internal_format(desc->Format, &fmt, &type);

    int prev = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    gl.GenTextures(c->length, c->gl_tex);
    for (int i = 0; i < c->length; i++) {
        gl.BindTexture(GL_TEXTURE_2D, c->gl_tex[i]);
        if (gl.TexStorage2D) {
            gl.TexStorage2D(GL_TEXTURE_2D, levels, internal, desc->Width, desc->Height);
        } else {
            gl.TexImage2D(GL_TEXTURE_2D, 0, (int)internal, desc->Width, desc->Height, 0,
                          fmt, type, NULL);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gl.BindTexture(GL_TEXTURE_2D, (unsigned)prev);
    if (!c->gl_tex[0]) {
        LOG("glGenTextures gave no texture (no current GL context?)");
        free(c);
        return NULL;
    }
    live_set(NULL, c);
    LOG("created %d-texture GL swap chain %dx%d internal format 0x%x (textures %u..%u, %s)",
        c->length, desc->Width, desc->Height, internal, c->gl_tex[0],
        c->gl_tex[c->length - 1], gl.TexStorage2D ? "glTexStorage2D" : "glTexImage2D");
    return (ovrTextureSwapChain)c;
}

int swapchain_is_gl(ovrTextureSwapChain chain)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    return c && c->gl;
}

unsigned swapchain_gl_buffer(ovrTextureSwapChain chain, int index)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c || !c->gl || index < 0 || index >= c->length)
        return 0;
    return c->gl_tex[index];
}

void swapchain_d3d_destroy(ovrTextureSwapChain chain)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c)
        return;
    live_set(c, NULL);
    if (c->gl && gl.DeleteTextures)
        gl.DeleteTextures(c->length, c->gl_tex);
    for (int i = 0; i < MAX_CHAIN_LENGTH; i++)
        if (c->textures[i])
            ID3D11Texture2D_Release(c->textures[i]);
    free(c);
}

int swapchain_d3d_length(ovrTextureSwapChain chain)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    return c ? c->length : 0;
}

int swapchain_d3d_current_index(ovrTextureSwapChain chain)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    return c ? c->index : 0;
}

void swapchain_d3d_commit(ovrTextureSwapChain chain)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c)
        return;
    c->committed = c->index;
    c->index = (c->index + 1) % c->length;
}

void *swapchain_d3d_buffer(ovrTextureSwapChain chain, int index, const void *iid)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c || index < 0 || index >= c->length || !c->textures[index])
        return NULL;
    void *out = NULL;
    if (iid) {
        if (FAILED(ID3D11Texture2D_QueryInterface(c->textures[index],
                                                  (REFIID)iid, &out)))
            return NULL;
        return out;  /* caller owns this reference */
    }
    ID3D11Texture2D_AddRef(c->textures[index]);
    return c->textures[index];
}

void *swapchain_d3d_submit_texture(ovrTextureSwapChain chain)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c)
        return NULL;
    int i = c->committed >= 0 ? c->committed : 0;
    if (c->gl)   /* OpenVR takes a GL texture name as its handle */
        return (void *)(uintptr_t)c->gl_tex[i];
    return c->textures[i];
}

static DXGI_FORMAT typed_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return f;
    }
}

static void clear_texture(struct ovrTextureSwapChainData *c, ID3D11Texture2D *tex,
                          const float color[4]);

void swapchain_d3d_fill_test_pattern(ovrTextureSwapChain chain, int eye)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c || c->gl)   /* D3D-only debug aid */
        return;
    /* Magenta for the left eye, cyan for the right. */
    const float colors[2][4] = {{1, 0, 1, 1}, {0, 1, 1, 1}};
    clear_texture(c, swapchain_d3d_submit_texture(chain), colors[eye & 1]);
}

void swapchain_d3d_preclear_next(ovrTextureSwapChain chain)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c || c->gl)   /* D3D-only debug aid */
        return;
    const float marker[4] = {0.0f, 0.25f, 0.0f, 1.0f};  /* dark green */
    clear_texture(c, c->textures[c->index], marker);
}

static void clear_texture(struct ovrTextureSwapChainData *c, ID3D11Texture2D *tex,
                          const float color[4])
{
    if (!tex)
        return;
    ID3D11Device *dev = NULL;
    ID3D11Texture2D_GetDevice(tex, &dev);
    if (!dev)
        return;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Format = typed_format(c->format);
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView *rtv = NULL;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, &rd, &rtv);
    if (SUCCEEDED(hr) && ctx) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color);
        ID3D11DeviceContext_Flush(ctx);
        ID3D11RenderTargetView_Release(rtv);
    } else {
        static int warned;
        if (!warned++)
            LOG("clear: CreateRenderTargetView failed (hr 0x%08lx)",
                (unsigned long)hr);
    }
    if (ctx)
        ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
}

static unsigned read_pixel(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                           ID3D11Texture2D *src, DXGI_FORMAT fmt, int x, int y)
{
    D3D11_TEXTURE2D_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Width = sd.Height = 1;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = fmt;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &staging)))
        return 0xdeadbeef;
    D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)x + 1, (UINT)y + 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)staging, 0, 0, 0, 0,
                                              (ID3D11Resource *)src, 0, &box);
    unsigned v = 0xdeadbeef;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        v = *(const unsigned *)m.pData;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
    return v;
}

void swapchain_d3d_probe(ovrTextureSwapChain chain, long frame)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c || !c->textures[0])
        return;
    ID3D11Device *dev = NULL;
    ID3D11Texture2D_GetDevice(c->textures[0], &dev);
    if (!dev)
        return;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    if (ctx) {
        for (int i = 0; i < c->length; i++) {
            /* A 3x3 grid in each eye's half; report how many samples are
             * non-zero and the first non-zero value, plus the centre. */
            for (int eye = 0; eye < 2; eye++) {
                int nonzero = 0;
                unsigned first = 0, centre = 0;
                for (int gy = 1; gy <= 3; gy++)
                    for (int gx = 1; gx <= 3; gx++) {
                        int x = eye * c->width / 2 + gx * c->width / 8;
                        int y = gy * c->height / 4;
                        unsigned v = read_pixel(dev, ctx, c->textures[i], c->format, x, y);
                        if (gx == 2 && gy == 2)
                            centre = v;
                        if (v) {
                            if (!nonzero)
                                first = v;
                            nonzero++;
                        }
                    }
                LOG("frame %ld: chain %p buffer %d%s eye %c: %d/9 samples non-zero "
                    "(first %08x, centre %08x)", frame, (void *)c, i,
                    i == c->committed ? " (submitted)" : "", eye ? 'R' : 'L',
                    nonzero, first, centre);
            }
        }
        ID3D11DeviceContext_Release(ctx);
    }
    ID3D11Device_Release(dev);
}

void swapchain_d3d_size(ovrTextureSwapChain chain, int *width, int *height)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (c) {
        *width = c->width;
        *height = c->height;
    }
}
