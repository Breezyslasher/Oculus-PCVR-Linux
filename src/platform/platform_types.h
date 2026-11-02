/*
 * Types and constants for the subset of the Oculus Platform SDK
 * (LibOVRPlatform64_1.dll) that the stub implements.
 *
 * These are written from the public API reference, not copied from the SDK
 * headers. Message type IDs are hash-like constants; a wrong value means the
 * game never recognizes our reply. The ones below were checked against the
 * type-to-name table (ovrMessageType_ToString) in Meta's Platform runtime.
 * The Windows async-init type has no entry there; its value is the one
 * Meta's runtime posts alongside its other initialization replies. Each can
 * still be overridden at runtime with an environment variable (see
 * message_type() in ovr_platform_stub.c).
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

/* Override any of these with OVRSTUB_MSGTYPE_<NAME>=0x... */
#define MSGTYPE_ENTITLEMENT_GET_IS_VIEWER_ENTITLED   0x186B58B1u
#define MSGTYPE_USER_GET_LOGGED_IN_USER              0x436F345Du
#define MSGTYPE_USER_GET                             0x6BCF9E47u
#define MSGTYPE_PLATFORM_INITIALIZE_WINDOWS_ASYNC    0x6DA7BA8Fu
#define MSGTYPE_ACHIEVEMENTS_ADD_COUNT               0x03E76231u
#define MSGTYPE_ACHIEVEMENTS_ADD_FIELDS              0x14AA2129u
#define MSGTYPE_ACHIEVEMENTS_GET_ALL_DEFINITIONS     0x03D3458Du
#define MSGTYPE_ACHIEVEMENTS_GET_ALL_PROGRESS        0x4F9FDE1Du
#define MSGTYPE_ACHIEVEMENTS_GET_DEFINITIONS_BY_NAME 0x629101BCu
#define MSGTYPE_ACHIEVEMENTS_GET_NEXT_DEFINITION_PAGE 0x2A7DD255u
#define MSGTYPE_ACHIEVEMENTS_GET_NEXT_PROGRESS_PAGE  0x2F42E727u
#define MSGTYPE_ACHIEVEMENTS_GET_PROGRESS_BY_NAME    0x152663B1u
#define MSGTYPE_ACHIEVEMENTS_UNLOCK                  0x593CCBDDu
#define MSGTYPE_USER_GET_ACCESS_TOKEN                0x06A85ABEu
#define MSGTYPE_USER_GET_LOGGED_IN_USER_FRIENDS      0x587C2A8Du
#define MSGTYPE_USER_GET_NEXT_USER_ARRAY_PAGE        0x267CF743u
#define MSGTYPE_IAP_GET_VIEWER_PURCHASES             0x3A0F8419u
#define MSGTYPE_IAP_GET_VIEWER_PURCHASES_DURABLE     0x63599E2Bu
#define MSGTYPE_IAP_GET_NEXT_PURCHASE_ARRAY_PAGE     0x47570A95u

typedef void (*ovrLogCallback)(const char *tag, const char *message);

#endif
