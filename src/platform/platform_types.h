/*
 * Types and constants for the subset of the Oculus Platform SDK
 * (LibOVRPlatform64_1.dll) that the stub implements.
 *
 * These are written from the public API reference, not copied from the SDK
 * headers. Message type IDs in particular are UNVERIFIED: they are hash-like
 * constants from OVR_MessageType.h, and a wrong value means the game never
 * recognizes our reply. Each one can be overridden at runtime with an
 * environment variable (see message_type() in ovr_platform_stub.c), and should
 * be checked against a real copy of OVR_MessageType.h or the MessageType enum
 * in a Unity game's Managed/Oculus.Platform.dll (e.g. with ilspycmd).
 */
#ifndef OVRSTUB_PLATFORM_TYPES_H
#define OVRSTUB_PLATFORM_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t ovrID;
typedef uint64_t ovrRequest;
typedef uint32_t ovrMessageType;

#define invalidRequestID ((ovrRequest)0)

typedef struct stub_message *ovrMessageHandle;
typedef struct stub_user *ovrUserHandle;
typedef struct stub_error *ovrErrorHandle;
typedef struct stub_message *ovrPlatformInitializeHandle;

typedef enum ovrPlatformInitializeResult {
    ovrPlatformInitialize_Success = 0,
    ovrPlatformInitialize_Uninitialized = -1,
    ovrPlatformInitialize_PreLoaded = -2,
    ovrPlatformInitialize_FileInvalid = -3,
    ovrPlatformInitialize_SignatureInvalid = -4,
    ovrPlatformInitialize_UnableToVerify = -5,
    ovrPlatformInitialize_VersionMismatch = -6,
    ovrPlatformInitialize_Unknown = -7,
    ovrPlatformInitialize_InvalidCredentials = -8,
    ovrPlatformInitialize_NotEntitled = -9,
} ovrPlatformInitializeResult;

/* UNVERIFIED defaults; override with OVRSTUB_MSGTYPE_<NAME>=0x... */
#define MSGTYPE_ENTITLEMENT_GET_IS_VIEWER_ENTITLED   0x186B58B1u
#define MSGTYPE_USER_GET_LOGGED_IN_USER              0x436F345Du
#define MSGTYPE_USER_GET                             0x6BCF9E47u
#define MSGTYPE_PLATFORM_INITIALIZE_WINDOWS_ASYNC    0x00000000u /* unknown */
#define MSGTYPE_ACHIEVEMENTS_ADD_COUNT               0x03E76231u
#define MSGTYPE_ACHIEVEMENTS_ADD_FIELDS              0x14AA2129u
#define MSGTYPE_ACHIEVEMENTS_GET_ALL_DEFINITIONS     0x03D3458Du
#define MSGTYPE_ACHIEVEMENTS_GET_ALL_PROGRESS        0x4F9FDE1Du
#define MSGTYPE_ACHIEVEMENTS_GET_DEFINITIONS_BY_NAME 0x629101BCu
#define MSGTYPE_ACHIEVEMENTS_GET_NEXT_DEFINITION_PAGE 0x2A7DD255u
#define MSGTYPE_ACHIEVEMENTS_GET_NEXT_PROGRESS_PAGE  0x2F42E727u
#define MSGTYPE_ACHIEVEMENTS_GET_PROGRESS_BY_NAME    0x152663B1u
#define MSGTYPE_ACHIEVEMENTS_UNLOCK                  0x593CCBDDu

typedef void (*ovrLogCallback)(const char *tag, const char *message);

#endif
