# Oculus PCVR on Linux: Launcher + Translation Layer

Project plan for running Oculus/Meta Rift Store PCVR games on Linux via Proton and SteamVR, with a Heroic-style launcher for login and downloads.

Target setup: Linux Mint, NVIDIA RTX 2060 Super, Meta Quest 3 via ALVR (SteamVR). WiVRn as a later backend.

---

## 1. Problem

- Rift Store games are Windows x86 binaries built on Meta's LibOVR runtime.
- They require the Oculus PC app and its background services, which do not run under Wine/Proton.
- Revive translates LibOVR calls to OpenVR/OpenXR, but it still depends on the Oculus runtime being installed, so it is Windows-only. Last repo push: Feb 2024.
- Valve's Lepton is unrelated here: it runs Android/Quest APKs on SteamOS (Steam Frame), not Windows PCVR games.

Gap: no Linux-native LibOVR replacement exists, and there is no Linux tool to log in to the Meta store and download Rift builds.

---

## 2. Architecture

```
+--------------------------------------------------+
| Launcher (GUI or Heroic backend)                 |
|  auth -> library -> download -> launch           |
+-----------------------+--------------------------+
                        |
                        v
+--------------------------------------------------+
| Proton prefix                                    |
|  game.exe                                        |
|   |-- LibOVRRT64_1.dll        (shim: LibOVR -> OpenVR)
|   |-- LibOVRPlatform64_1.dll  (stub: entitlements, user)
|   '-- DXVK (D3D11 -> Vulkan)                     |
+-----------------------+--------------------------+
                        | Proton vrclient bridge
                        v
+--------------------------------------------------+
| SteamVR (Linux) -> ALVR -> Quest 3               |
| (later: OpenXR -> WiVRn / Monado)                |
+--------------------------------------------------+
```

### 2.1 Launcher (Heroic-style)

| Stage    | Approach |
|----------|----------|
| Login    | Meta web login, capture the account access token (same pattern as Legendary for Epic). |
| Library  | Query the store GraphQL API for owned entitlements and app IDs. |
| Download | Resolve the release channel, fetch the build manifest and files, with resume and hash verification. |
| Launch   | Create/manage a Proton prefix, drop in shim DLLs, set env vars, launch the game through Steam/Proton. |

Implementation options:
- Standalone CLI backend (like `legendary` / `gogdl` / `nile`) so it can plug into Heroic later.
- Simple GUI frontend on top, or a Heroic integration.

Prior art to study:
- Meta OVR Platform Utility (`ovr-platform-util`): official CLI, runs on Linux, has Rift build download commands. Needs app ID, build ID and a token. It is unconfirmed whether a normal user token can download purchased store builds; test this first.
- ComputerElite's Oculus Downgrader: downloaded Rift builds of owned games via account login.
- Related downgrader browser extension: reportedly broke in early 2024 after Meta changed things.

### 2.2 LibOVR shim (`LibOVRRT64_1.dll`)

- A Windows DLL built with mingw-w64 that loads inside the Proton prefix.
- Reimplements the LibOVR API (sessions, tracking, swap chains, input, frame submission).
- Translates calls to OpenVR (`openvr_api.dll`) so Proton's vrclient bridge carries them to Linux SteamVR.
- Uses Revive's open-source mapping code as the starting reference.
- D3D11 texture swap chains are passed to OpenVR, and Proton/DXVK handle the D3D11 -> Vulkan path.
- Must bypass Revive's dependency on the Oculus runtime/service entirely.

### 2.3 Platform SDK stub (`LibOVRPlatform64_1.dll`)

- Store games call it for entitlement checks, user ID/name, achievements and multiplayer.
- Most games quit if the entitlement check fails, so the stub must return success.
- Minimum viable: initialize, entitlement check, logged-in user, message queue pump.
- Online, multiplayer and social features are out of scope and will be no-ops or failures.

---

## 3. Milestones

1. **Download feasibility**
   - Test `ovr-platform-util` with a user token on one owned Rift game.
   - If blocked, review the Oculus Downgrader approach.
   - Fallback: install on Windows once (VM or other PC) and copy the game folder.
2. **Boot to menu**
   - Pick one simple single-player Unity Rift title.
   - Run it in Proton with a stubbed Platform DLL and a no-op LibOVR that reports "no HMD".
   - Goal: the game passes the entitlement check and reaches its menu on the flat screen.
3. **Head tracking + rendering**
   - Implement the LibOVR session, tracking and swap-chain submit -> OpenVR.
   - Goal: picture and head tracking on the Quest 3 via ALVR.
4. **Controllers**
   - Map Touch input (buttons, triggers, thumbsticks, capacitive touch) from OpenVR input.
5. **Compatibility pass**
   - Test Unreal titles and different LibOVR versions.
   - Start a compatibility list.
6. **Launcher**
   - Library + download + prefix management CLI, then a GUI or Heroic backend.
7. **OpenXR backend (optional)**
   - Second output path to OpenXR for WiVRn/Monado, bypassing SteamVR.

---

## 4. Risks

- **API churn:** Meta can change or close store/download endpoints at any time.
- **SDK versions:** LibOVR and the Platform SDK change across versions, so the shim has to cover several ABIs.
- **Extra DRM:** some games add protection beyond the Platform entitlement check.
- **ToS:** stubbing entitlements and using unofficial downloads is a gray area, even for owned games.
- **NVIDIA + SteamVR on Linux:** known instability (e.g. the 590+ driver crash). Keep the patched Vulkan layer and ALVR setup stable before debugging the shim.

---

## 5. References

- Revive: https://github.com/LibreVR/Revive
- Oculus Downgrader: https://github.com/ComputerElite/Oculus-downgrader
- Meta OVR Platform Utility: https://developers.meta.com/vr/resources/publish-reference-platform-command-line-utility/?device=RIFT
- TCRF Rift download guide: https://tcrf.net/Help:Contents/Finding_Content/Systems/Oculus_Rift
- ArchWiki VR: https://wiki.archlinux.org/title/Virtual_reality
- VR on Linux game DB: https://db.vronlinux.org
- WiVRn: https://github.com/WiVRn/WiVRn
- OpenComposite (design reference for API reimplementation): https://gitlab.com/znixian/OpenOVR
