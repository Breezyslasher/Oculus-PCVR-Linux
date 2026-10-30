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

### First real game: Spider-Man: Homecoming VR (Unreal Engine 4, 2017)

- No Oculus DLLs in the game folder. The LibOVR loader is built into
  `SMHCVR/Binaries/Win64/SMHCVR-Win64-Shipping.exe`, and so is the Platform
  SDK loader. It formats `%lsLibOVRRT%hs_%d.dll` and
  `%lsLibOVRPlatform%hs_%d.dll` with a directory prefix, so **Unreal loads
  the Platform SDK from the Oculus install, not the game folder.**
- Injection under Proton 10 works: the shim loads in the game and installs
  its redirect, and DXVK starts. But the runtime was never requested, and
  the game exited with code 3.
- Cause: before touching LibOVR, Unreal's Oculus plugin opens the named event
  `OculusHMDConnected` (created by the Oculus service, signaled while a
  headset is attached). Without it, the plugin treats Oculus as not
  installed. The shim now creates that event at startup (signaled by
  default; `OVRSTUB_HMD_CONNECTED=0` to leave it unsignaled), the same idea
  as Revive's `OpenEventW` hook. The redirect also covers
  `LibOVRPlatform64_1.dll`, and `ovrl prefix-setup` installs the Platform
  stub next to the shim.

- After that, the game still crashed at startup, with or without the shim:
  `0xC06D007E`, delay-load of **`openvr_api.dll`** (`VR_InitInternal`),
  found with `tools/minidump_info.py`. UE 4.14 builds in the SteamVR plugin
  even for Oculus Store builds but doesn't ship `openvr_api.dll`. Under
  Proton SteamVR looks installed, so the plugin calls into the missing DLL.
  Launching with **`-hmd=OculusRift`** avoids it; the game then runs (black
  screen, since no HMD is reported) and exits cleanly.
- It still never loaded the runtime: a `+reg,+file` trace showed no read
  of the Oculus registry key and no attempt to open LibOVRRT. The exe
  contains the ASCII string **`LIBOVR_DLL_DIR`**: the loader takes the runtime
  folder from that environment variable (prefixed directly onto
  `LibOVRRT64_1.dll`, so it needs a trailing backslash). `ovrl launch` now
  sets it to the shim's folder. The wide string `Oculus VR, LLC` in the exe
  is probably the signer name for the runtime signature check; whether that
  check blocks our unsigned DLL is the next thing to find out.

- With `LIBOVR_DLL_DIR` set and the HMD event present, the game searched for
  a **bare `LibOVRRT64_1.dll`** on the standard DLL search path (exe folder,
  cwd, system dirs, PATH) and never looked in the `LIBOVR_DLL_DIR` folder.
  Putting the stubs **next to the game exe** got them found: the trace then
  showed the game `CreateFileW`-opening both stubs.
- But it opened the files without loading them: the loaders call
  **`WinVerifyTrust`** on the DLL first and only load it if signed. Our
  unsigned stubs returned `TRUST_E_NOSIGNATURE` (0x800B0100). The shim now
  hooks `WinVerifyTrust` (via a `GetProcAddress` hook, since the game resolves
  wintrust at run time) and reports success for our two stub file names only;
  all other files verify normally. `OVRSTUB_FAKE_SIGNATURE=0` disables it.
  Loaders that also compare the signer name/thumbprint against Meta's real
  cert would still reject it; the sigcheck trace shows what each game calls.
- Confirmed on Spider-Man: Homecoming VR that this is not enough. With the
  `WinVerifyTrust` hook in place the Platform stub loads, but the runtime is
  still only `CreateFileW`-opened, never `LoadLibrary`-ed: the game's LibOVR
  loader reads the file's embedded code-signing certificate and checks that
  the publisher is Meta (the `Oculus VR, LLC` string), which our stub cannot
  satisfy.

  **Design decision: do not fabricate Meta's signing identity.** Making the
  stub present a certificate that claims to be signed by "Oculus VR, LLC"
  would mean forging a vendor's code-signing credentials, which this project
  will not do, even for games the user owns. The supported path is Revive's:
  the user supplies Meta's own freely-distributed, genuinely signed
  `LibOVRRT64_1.dll` (from the Meta PC runtime). That real file sits on disk
  so the game's authenticity check passes against a legitimate Meta binary,
  and our existing `LoadLibrary` redirect swaps our implementation in at load
  time, so none of Meta's service-dependent code runs. This keeps the
  authenticity check meaningful (it is validating a real Meta file) instead
  of defeating it. `ovrl stubs install --real-runtime <LibOVRRT64_1.dll>`
  does this: it places the user-supplied genuine runtime next to the exe
  (our stubs carry an `OVRSTUB-BUILD-MARKER-v1` marker so a real file is
  never mistaken for one of ours), and the injected shim's redirect swaps
  our implementation in at load.

## Platform SDK (`LibOVRPlatform64_1.dll`)

- Unity games ship it in the game folder, so the stub can replace it there
  (`ovrl stubs install`). Unreal games load it from the Oculus install
  instead (see above), where `ovrl prefix-setup` puts the stub.
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

## Store and downloads (Milestone 1)

Based on ComputerElite's
[OculusGraphQLApiLib](https://github.com/ComputerElite/OculusGraphQLApiLib)
(last commit May 2026), the library behind Oculus Downgrader. Implemented in
`launcher/ovrl/store.py` and `download.py`. **Not yet tested against the live
store.**

- **Auth:** the user's own access token (`FRL…` or `OC…`) from a logged-in
  browser session. The library also embeds Meta's own app tokens as
  fallbacks; we deliberately don't use them.
- **GraphQL:** `POST https://graph.oculus.com/graphql?access_token=…&variables=…&doc_id=…`
  with persisted query IDs: current user `4149322231793299`, entitlements
  `4850747515044496`, an app's builds `2885322071572384`
  (`{"applicationID": …}` → `node.primary_binaries.nodes[]` with `id`,
  `version`, `version_code`, `binary_release_channels`).
- **Manifest:** `GET https://securecdn.oculus.com/binaries/download/?id=<build>&access_token=…&get_manifest=1`
  returns a zip containing one JSON file: `launchFile`, `launchParameters`,
  `files{path: {sha256, size, segmentSize, segments[[?, sha256, ?], …]}}`.
  Only index 1 (the SHA-256) of a segment entry is relied on. The others
  look like offset and size.
- **Segments:** `GET https://securecdn.oculus.com/binaries/segment/?access_token=…&binary_id=<build>&segment_sha256=…`
  returns a zlib stream. The SHA-256 is over the decompressed bytes. Files are
  their segments concatenated in manifest order.
- The original Milestone 1 idea, `ovr-platform-util`, isn't needed for this
  path. It stays a fallback if the CDN route stops working.

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
