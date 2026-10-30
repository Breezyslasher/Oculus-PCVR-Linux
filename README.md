# Oculus PCVR on Linux

Run Oculus/Meta Rift Store PCVR games on Linux through Proton and SteamVR,
without the Oculus PC app. See [docs/PLAN.md](docs/PLAN.md) for the full plan
and [docs/research-notes.md](docs/research-notes.md) for what has been
confirmed so far.

**Status: early.** The Milestone 2 pieces build and pass smoke tests under
Wine. They have not been tried on a real game yet.

| Piece | Path | State |
|-------|------|-------|
| Platform SDK stub (`LibOVRPlatform64_1.dll`) | `src/platform` | Init, entitlement (always passes), logged-in user, message queue. Message type IDs unverified. |
| LibOVR shim (`LibOVRRT64_1.dll`) | `src/libovr` | "Runtime present, no HMD": `ovr_Create` returns `ovrError_NoHmd`. Redirects the game's runtime load to itself. |
| Injector (`ovrinject.exe`) | `src/injector` | Starts the game suspended and loads the shim before game code runs. |
| Launcher (`ovrl`) | `launcher/` | Stub install/restore, prefix setup, launch, DLL export diff. No store login or downloads yet. |

## Build

You need `mingw-w64` (Debian/Ubuntu/Mint: `sudo apt install gcc-mingw-w64-x86-64`).

```sh
make            # -> build/LibOVRPlatform64_1.dll, build/LibOVRRT64_1.dll, build/ovrinject.exe
make check      # smoke tests under Wine (WINE=/path/to/wine64 to choose a binary)
```

## Try it on a game (Milestone 2: boot to menu)

Assumes the game is already installed somewhere (Milestone 1 covers getting
the files) and has a Proton prefix, e.g. after adding the exe to Steam as a
non-Steam game and launching it once.

```sh
cd launcher
# Swap in the Platform stub (keeps a backup as *.ovrl-orig)
python3 -m ovrl stubs install "/path/to/Game"

# Put the LibOVR shim into the prefix and register a fake Oculus install
python3 -m ovrl prefix-setup ~/.steam/steam/steamapps/compatdata/<id> \
    --wine ~/.steam/steam/steamapps/common/"Proton - Experimental"/files/bin/wine64

# Launch through the injector
python3 -m ovrl launch ~/.steam/steam/steamapps/compatdata/<id> \
    --proton ~/.steam/steam/steamapps/common/"Proton - Experimental" \
    "/path/to/Game/Game.exe"

# Undo the Platform stub
python3 -m ovrl stubs restore "/path/to/Game"
```

Logs go to stderr and to `ovrstub.log` next to the game exe (or wherever
`OVRSTUB_LOG` points). If a game ignores the entitlement reply, the message
type IDs are the likely cause. See the research notes for how to check them.

### Stub environment variables

| Variable | Default | Effect |
|----------|---------|--------|
| `OVRSTUB_USER_ID` | `1` | User ID reported to the game |
| `OVRSTUB_USER_NAME` | `LinuxPlayer` | Oculus ID / display name |
| `OVRSTUB_LOCALE` | `en_US` | `ovr_GetLoggedInUserLocale` |
| `OVRSTUB_MSGTYPE_ENTITLEMENT`, `..._USER_GET_LOGGED_IN_USER`, `..._USER_GET`, `..._PLATFORM_INITIALIZE_WINDOWS_ASYNC` | see `platform_types.h` | Override a message type ID |
| `OVRSTUB_LOG` | unset | Also append logs to this file |

## Layout

```
src/common/     logging shared by the DLLs
src/platform/   LibOVRPlatform64_1.dll stub
src/libovr/     LibOVRRT64_1.dll shim + LoadLibrary redirect
src/injector/   ovrinject.exe
tests/          Windows-side smoke tests (run under Wine by `make check`)
launcher/ovrl/  Python launcher CLI
docs/           plan and research notes
```

## Notes

- Only use this with games you own. Stubbing entitlement checks is a ToS gray
  area (see the plan's risk section).
- No Oculus SDK headers are included. Types are written from the public API,
  cross-checked against the `ovr-sys` bindings.
