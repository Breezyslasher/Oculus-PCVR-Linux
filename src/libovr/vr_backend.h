/*
 * OpenVR backend for the LibOVR shim (Milestone 3).
 *
 * Wraps the small part of OpenVR the shim needs so the rest of the code deals
 * in LibOVR types. It dynamically loads openvr_api.dll (provided by SteamVR /
 * Proton's bridge) and uses OpenVR's C "FnTable" interfaces. When SteamVR is
 * not available every call fails cleanly, and the shim falls back to its
 * no-HMD behavior.
 *
 * Both OpenVR and LibOVR use a right-handed, Y-up, -Z-forward metric space, so
 * poses map across with no axis conversion (Revive relies on the same).
 */
#ifndef OVRSTUB_VR_BACKEND_H
#define OVRSTUB_VR_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#include "ovr_types.h"

/* Bring up OpenVR as a Scene application. Safe to call more than once;
 * returns true once the system and compositor interfaces are available. */
bool vr_backend_init(void);
void vr_backend_shutdown(void);
bool vr_backend_active(void);

/* Per-eye recommended render target size (same for both eyes). */
void vr_backend_render_target_size(int *width, int *height);

/* Symmetric-or-not tangent-half-angle FOV for an eye, in LibOVR's convention. */
ovrFovPort vr_backend_fov(ovrEyeType eye);

/* Eye pose relative to the head (HmdToEyePose), as a LibOVR pose. */
ovrPosef vr_backend_eye_pose(ovrEyeType eye);

/* Latest head pose (orientation + position). Identity if unavailable. */
ovrPoseStatef vr_backend_head_pose(void);

/* True while a hand controller of the given OpenVR class is connected. */
bool vr_backend_hmd_present(void);

#endif
