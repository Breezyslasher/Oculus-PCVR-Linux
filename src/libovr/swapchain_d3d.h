/*
 * D3D11 texture swap chains for the LibOVR shim (Milestone 3b).
 *
 * A game renders each frame into one texture of a small ring (the swap
 * chain), commits it, then submits it through ovr_SubmitFrame. We create
 * real ID3D11Texture2D objects on the game's own device so the game can draw
 * into them, and on submit hand the committed one to OpenVR's compositor.
 */
#ifndef OVRSTUB_SWAPCHAIN_D3D_H
#define OVRSTUB_SWAPCHAIN_D3D_H

#include "ovr_types.h"

struct ovrTextureSwapChainData;  /* opaque; defined in swapchain_d3d.c */

/* Create a swap chain of D3D11 textures matching `desc` on `d3d_device`
 * (an ID3D11Device*). Returns NULL on failure. */
ovrTextureSwapChain swapchain_d3d_create(void *d3d_device,
                                         const ovrTextureSwapChainDesc *desc);
void swapchain_d3d_destroy(ovrTextureSwapChain chain);

int swapchain_d3d_length(ovrTextureSwapChain chain);
int swapchain_d3d_current_index(ovrTextureSwapChain chain);
void swapchain_d3d_commit(ovrTextureSwapChain chain);

/* The ID3D11Texture2D at `index`, as an IUnknown* queried to `iid`. */
void *swapchain_d3d_buffer(ovrTextureSwapChain chain, int index, const void *iid);

/* The texture most recently committed (what gets submitted), and the chain's
 * pixel dimensions. */
void *swapchain_d3d_submit_texture(ovrTextureSwapChain chain);
void swapchain_d3d_size(ovrTextureSwapChain chain, int *width, int *height);
/* Debug aid (OVRSTUB_TEST_PATTERN=1): clear the texture about to be submitted
 * to a solid color, so whether it shows in the headset separates "submission
 * is broken" from "the game's rendering doesn't reach our texture". */
void swapchain_d3d_fill_test_pattern(ovrTextureSwapChain chain, int eye);
/* Debug aid: log a pixel from the middle of each eye's half of every buffer
 * in the chain (marking the one we submit), to tell "the game draws into a
 * different buffer" from "the game draws nothing". */
void swapchain_d3d_probe(ovrTextureSwapChain chain, long frame);

#endif
