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
| Launcher (`ovrl`) | `launcher/` | Store login, owned-app list, build list and resumable Rift downloads (untested against the live store). Stub install/restore, prefix setup, launch, DLL export diff. |

## Build

You need `mingw-w64` (Debian/Ubuntu/Mint: `sudo apt install gcc-mingw-w64-x86-64`).

```sh
make            # -> build/LibOVRPlatform64_1.dll, build/LibOVRRT64_1.dll, build/ovrinject.exe
make check      # smoke tests under Wine (WINE=/path/to/wine64 to choose a binary)
cd launcher && python3 -m unittest discover -s tests   # launcher tests (no network needed)
```

## Download a game you own (Milestone 1)

`ovrl` can download Rift builds straight from Meta's CDN using your own
account's access token, the same way
[Oculus Downgrader](https://github.com/ComputerElite/Oculus-downgrader) does.
This has only been tested against a local fake of the store so far. If it
fails against the real one, please open an issue with the error message.

**1. Get your access token.** These steps follow
[ComputerElite's token guides](https://github.com/ComputerElite/ComputerElite.github.io/tree/main/tools/Oculus).
The token is a password for your account: anyone who has it can act as you,
so never share it.

*Without Windows (browser cookie, the guide's older method):*
1. In Chrome or Firefox, open <https://developer.oculus.com/manage> and log in
   with your Meta account. It may redirect to developers.meta.com; that's
   fine. You don't need a developer account, only to be logged in.
2. Press F12. In Chrome go to **Application → Cookies**; in Firefox go to
   **Storage → Cookies**. Select the site you're on.
3. Find the cookie named **`oc_ac_at`** and copy its **value** (it starts
   with `FRL` or `OC`).

Meta may no longer accept this kind of token for downloads; the downgrader
switched to the method below. `ovrl login` checks the token right away, and
`ovrl versions` / `ovrl download` will show whether it is accepted.

*With the Oculus PC app on Windows (the guide's current method):* with the
Oculus app open and logged in, press Ctrl+Shift+I. In **Network**, filter for
`graph`, click the first request, and copy the `FRL…` token shown at the
bottom of its **Headers** tab. The token keeps working on Linux after that,
until it expires.

**2. Log in and find the game.**

```sh
cd launcher
python3 -m ovrl login          # paste the token; it's checked, then saved to ~/.config/ovrl/token (mode 600)
python3 -m ovrl library        # app IDs and names you own
python3 -m ovrl versions <id>  # builds of a Rift app, newest first
```

Quest and Rift versions of a game have different app IDs. Only the Rift one
has a PC build.

**3. Download.**

```sh
python3 -m ovrl download <app-id> --dest ~/Games/MyGame
```

By default it takes the newest build on the `LIVE` channel; `--build-id`
picks a specific one. Every segment and file is checked against the
manifest's SHA-256. Re-running the same command resumes and skips files
that are already complete. The game's `manifest.json` is saved alongside it
and names the launch file.

Tokens expire. If you get an "expired or invalid" error, run `ovrl login`
again. `OVRL_TOKEN=... ovrl ...` also works without saving the token.

## Try it on a game (Milestone 2: boot to menu)

Assumes the game is already downloaded (see above) and has a Proton prefix,
e.g. after adding the exe to Steam as a non-Steam game and launching it once,
or by creating one as below.

```sh
GAME=/path/to/Game/Binaries/Win64        # the folder with the real game exe
P=~/.local/share/ovrl/prefixes/mygame    # a Proton compatdata dir
PROTON=~/.steam/steam/steamapps/common/"Proton 10.0"

cd launcher
# Put both stubs next to the game exe (keeps backups as *.ovrl-orig). The
# LibOVR loader searches the exe folder for a bare LibOVRRT64_1.dll.
python3 -m ovrl stubs install "$GAME"

# Install the shim into the prefix and register a fake Oculus install
python3 -m ovrl prefix-setup "$P" --wine "$PROTON/files/bin/wine"

# Launch through the injector. Unreal games also need -hmd=OculusRift so the
# engine skips its built-in SteamVR plugin (whose openvr_api.dll is absent).
python3 -m ovrl launch "$P" --proton "$PROTON" "$GAME/Game.exe" -hmd=OculusRift

# Undo the stubs
python3 -m ovrl stubs restore "$GAME"
```

Logs go to stderr and to `ovrstub.log` next to the game exe (or wherever
`OVRSTUB_LOG` points). The `[libovr-trace]` lines show which Oculus files,
events and env vars the game looks for, which is the first place to look when
a new game doesn't reach its menu.

## Layout

```
src/common/     logging shared by the DLLs
src/platform/   LibOVRPlatform64_1.dll stub
src/libovr/     LibOVRRT64_1.dll shim: LoadLibrary redirect, GetProcAddress hook, WinVerifyTrust (sigcheck.c)
src/injector/   ovrinject.exe
tests/          Windows-side smoke tests (run under Wine by `make check`)
launcher/ovrl/  Python launcher CLI (store.py: Meta API, download.py: CDN downloader)
launcher/tests/ launcher tests against a local fake store/CDN
tools/          debugging helpers (minidump_info.py: summarize a crash dump)
docs/           plan and research notes
```

## Notes

- Only use this with games you own. Stubbing entitlement checks is a ToS gray
  area (see the plan's risk section).
- No Oculus SDK headers are included. Types are written from the public API,
  cross-checked against the `ovr-sys` bindings.
