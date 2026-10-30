# Oculus PCVR on Linux

Run Oculus/Meta Rift Store PCVR games on Linux through Proton and SteamVR,
without the Oculus PC app. See [docs/PLAN.md](docs/PLAN.md) for the full plan
and [docs/research-notes.md](docs/research-notes.md) for what has been
confirmed so far.

**Status: early, in active bring-up.** Milestones 1-2 work on real games
(download, injection, stubs, signature handling). Milestone 3 (OpenVR-backed
rendering to the headset) is newly implemented and untested on hardware.

| Piece | Path | State |
|-------|------|-------|
| Platform SDK stub (`LibOVRPlatform64_1.dll`) | `src/platform` | Init, entitlement (always passes), logged-in user, message queue. Message type IDs unverified. |
| LibOVR shim (`LibOVRRT64_1.dll`) | `src/libovr` | Bridges LibOVR to OpenVR (SteamVR): session, HMD description, tracking, D3D11 swap chains and frame submission. Falls back to "no HMD" when SteamVR is absent. Redirects the game's runtime load; answers the signature check. |
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
# Put the stubs next to the game exe (keeps backups as *.ovrl-orig). The
# LibOVR loader searches the exe folder for a bare LibOVRRT64_1.dll.
#
# Many games verify the runtime is signed by Meta before loading it. For
# those, pass Meta's own genuine signed LibOVRRT64_1.dll (from a Meta PC
# runtime install) so the check passes against a real file; the injected
# shim still swaps our implementation in at load, so no Meta service runs:
python3 -m ovrl stubs install "$GAME" --real-runtime /path/to/real/LibOVRRT64_1.dll
# If the game also checks the Platform SDK's signature, add its real file too:
#   ... --real-platform /path/to/real/LibOVRPlatform64_1.dll
# Games without the check work with just:
#   python3 -m ovrl stubs install "$GAME"

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

## See it in the headset (Milestone 3: launch through Steam)

**Status:** *The Eastern Edge* (Unity 2017.4, Oculus SDK 1.37) is playable this
way: it renders in the headset with head tracking and Touch-style controller
input, on Linux Mint with Proton 11 and a Quest 3 via SteamVR.
*Spider-Man: Homecoming VR* (Unreal 4.14, SDK 1.8) gets a VR session and
submits frames, but renders black (even flat with `-nohmd`), so that one is
blocked on the game itself rather than the shim.

Use a Proton at least as new as the one that created the prefix. In the
tests, Proton 9 failed the game's signature check in a prefix made by Proton
10; Proton 10 and 11 both pass. `add-to-steam --prefix` picks the version
recorded in the prefix when Steam has a tool by that name.

`ovrl launch` runs the game under a bare `proton run`, which is enough to
reach a menu but leaves Proton's OpenVR bridge (`vrclient`) without the Steam
runtime environment that points it at the native Linux SteamVR server — so
VR init fails and you get a black screen. Steam sets that environment up for
anything it launches. The simplest fix is therefore to let Steam drive the
launch: add the game as a non-Steam game and point its shortcut at our
injector.

```sh
GAME=/path/to/Game/Binaries/Win64/Game.exe   # the real game exe

cd launcher
# Stage everything next to the exe: the genuine runtime for the signature
# check, our shim (as ovrstub_shim.dll), the injector, and openvr_api.dll.
python3 -m ovrl steam-setup "$GAME" --real-runtime /path/to/real/LibOVRRT64_1.dll
# --real-platform, --openvr and --hmd work as for the commands above.
```

Then write the Steam shortcut the way Heroic does — straight into Steam's
config, no dialogs. **Quit Steam fully first** (it rewrites its config on
exit, which would discard the change):

```sh
python3 -m ovrl add-to-steam "$GAME" --name "Spider-Man Homecoming VR" \
    --prefix ~/.local/share/ovrl/prefixes/mygame
```

`--prefix` runs the game in a prefix you already prepared with
`ovrl prefix-setup` (above) instead of the fresh one Steam would create for
the shortcut. On Spider-Man: Homecoming this is what got the game past its
runtime check *and* into a SteamVR session.

This adds the entry to `userdata/<you>/config/shortcuts.vdf` (flagged as a VR
app) and forces Proton for it in `config/config.vdf`, reusing whichever
Proton your other games already map to (`--proton-tool` overrides). Both files
are backed up as `*.ovrl-bak` and must round-trip through the parser before
they are touched, so a surprise in Steam's format aborts rather than losing
your other shortcuts. Start Steam and the game is in your library.

If you'd rather use the Steam UI, `steam-setup` printed the exact settings:
**Add a Non-Steam Game**, browse to `ovrinject.exe` in the game folder, then
in its **Properties**:

- **Target** stays `ovrinject.exe`, **Start In** the game folder (Steam fills
  this in when you browse to the exe).
- **Launch Options**: paste the printed line, e.g.
  `OVRSTUB_INSTALL_ROOT_CA=1 %command% --wait ovrstub_shim.dll -- "Game.exe" -hmd=OculusRift`
- **Compatibility**: force the same Proton version you use elsewhere.

Start SteamVR, then launch the entry from your library. Steam wires up the VR
bridge, Proton loads our shim through the injector, and the shim renders the
game through SteamVR. Undo with `ovrl stubs restore "$(dirname "$GAME")"`.

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
