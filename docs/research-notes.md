# Research notes

Findings that shaped the first code, with how sure we are of each. Update this
file as things get confirmed or disproved on real games.

## How a game finds the Oculus runtime

- Games don't link `LibOVRRT64_1.dll` directly. They statically link Meta's
  small LibOVR loader (the "CAPI shim"), which finds the installed runtime and
  calls `LoadLibrary` on it. In Unity games that loader is inside `OVRPlugin.dll`
  in the game folder; native/Unreal games have it in the executable or plugin.
- The install location comes from the registry: `HKLM\Software\Oculus VR,
  LLC\Oculus` (32-bit view), value `Base`; the runtime is under
  `<Base>\Support\oculus-runtime\`. Revive's injector reads the same key
  (`ReviveInjector/main.cpp`, `GetOculusBasePath`). `ovrl prefix-setup` writes
  this key so the loader has a path to try.
- LibOVR defines `ovrError_LibSignCheck` (-3021, "LibOVRRT signature check
  failure"), so the loader can check the runtime DLL's signature. Our DLL is
  unsigned. **Unconfirmed** whether, when, and how strictly this is enforced
  (and how Wine's `WinVerifyTrust` behaves here).
- Revive works around all of this by injecting its DLL into the game at startup
  (`DetourCreateProcessWithDlls`) and hooking `LoadLibraryW`/`LoadLibraryExW`
  so any request for `LibOVRRT64_1.dll` returns the Revive module. It also
  hooks `OpenEventW` for the "HMD connected" event (a Unity performance fix),
  and `CreateDXGIFactory` to force DXGI 1.1 for shared textures. We'll likely
  need both in Milestone 3.
- We do the same with `ovrinject.exe` + an import-table hook in the shim
  (`src/libovr/iat_hook.c`). Tested under Wine 9 with a test program that
  imitates the loader; **not yet tested on a real game or under Proton**.
  Known gap: callers that get `LoadLibrary*` through `GetProcAddress` are
  not redirected.

## Platform SDK (`LibOVRPlatform64_1.dll`)

- Ships inside each game's folder, so the stub can just replace it there
  (`ovrl stubs install`).
- Unity games call it through the C# `Oculus.Platform` wrapper (`DllImport`,
  bound lazily per function), so a missing export only fails when that
  function is called. Native games link an import library, so a missing export
  stops the game from loading at all. Use `ovrl exports <real.dll> <stub.dll>`
  to list what a given game's DLL exports that ours doesn't.
- Native games may load the Platform DLL through a loader that checks its
  signature (`ovrPlatformInitialize_SignatureInvalid` exists). **Unconfirmed.**
- **Message type IDs are unverified.** They're hash-like constants from
  `OVR_MessageType.h`. Meta's reference site and package registry are blocked
  from the environment the first code was written in, so the defaults in
  `src/platform/platform_types.h` come from memory. The async-init type is
  unknown and defaults to 0. To check them, decompile
  `<Game>_Data/Managed/Oculus.Platform.dll` (e.g. `ilspycmd`) and read the
  `MessageType` enum, then fix the header or set `OVRSTUB_MSGTYPE_*` at runtime.

## LibOVR ABI

- Struct layouts in `src/libovr/ovr_types.h` were cross-checked against the
  `ovr-sys` 0.3.0 Rust bindings (LibOVR 1.15) and pinned with `static_assert`s.
- By-value struct returns matter on Win64: 1/2/4/8-byte structs come back in
  RAX, anything else goes through a hidden pointer. `ovrSizei`,
  `ovrDetectResult` and `ovrHmdColorDesc` are 8 bytes; the rest use a hidden
  pointer.
- `ovr_GetRenderDesc` (before 1.17, eye offset as a vector) and
  `ovr_GetRenderDesc2` (1.17 and later, full eye pose) both have to be exported;
  Revive does the same. Expect more renamed variants like this across SDK
  versions (Milestone 5).
- Revive's complete runtime export list (from `Revive/REV_CAPI*.cpp`) is the
  checklist for Milestone 3. The no-HMD shim exports all of the core and D3D/GL
  entry points. It leaves out the audio GUID functions, `ovr_Lookup`,
  `ovr_GetExternalCameras`/`SetExternalCameraProperties`, the Vulkan functions,
  `ovr_GetTrackingStateWithSensorData`, `ovr_Get/SetEnabledCaps` and
  `ovr_GetTrackingCaps`.

## Sources

- Revive: https://github.com/LibreVR/Revive (MIT)
- ovr-sys bindings: https://crates.io/crates/ovr-sys
- OpenComposite: https://gitlab.com/znixian/OpenOVR (expects the LibOVR SDK to be
  supplied separately; it doesn't vendor the headers)
