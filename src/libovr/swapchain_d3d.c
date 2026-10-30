#include "swapchain_d3d.h"

#define COBJMACROS
#include <d3d11.h>
#include <stdlib.h>
#include <string.h>

#include "../common/log.h"

#define LOG(...) stub_log("swapchain", __VA_ARGS__)
#define MAX_CHAIN_LENGTH 3

struct ovrTextureSwapChainData {
    ID3D11Texture2D *textures[MAX_CHAIN_LENGTH];
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
    LOG("created %d-texture swap chain %dx%d fmt %d (shared=%d)",
        c->length, desc->Width, desc->Height, (int)td.Format,
        td.MiscFlags == D3D11_RESOURCE_MISC_SHARED);
    return (ovrTextureSwapChain)c;
}

void swapchain_d3d_destroy(ovrTextureSwapChain chain)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c)
        return;
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

void swapchain_d3d_fill_test_pattern(ovrTextureSwapChain chain, int eye)
{
    struct ovrTextureSwapChainData *c = (void *)chain;
    if (!c)
        return;
    ID3D11Texture2D *tex = swapchain_d3d_submit_texture(chain);
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
        /* Magenta for the left eye, cyan for the right. */
        const float colors[2][4] = {{1, 0, 1, 1}, {0, 1, 1, 1}};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, colors[eye & 1]);
        ID3D11DeviceContext_Flush(ctx);
        ID3D11RenderTargetView_Release(rtv);
    } else {
        static int warned;
        if (!warned++)
            LOG("test pattern: CreateRenderTargetView failed (hr 0x%08lx)",
                (unsigned long)hr);
    }
    if (ctx)
        ID3D11DeviceContext_Release(ctx);
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
