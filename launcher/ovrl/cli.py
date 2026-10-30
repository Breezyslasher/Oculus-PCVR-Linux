"""ovrl: set up and launch Rift Store games under Proton with the stub DLLs.

Store side (Milestone 1): log in with your Meta access token, list owned apps
and their builds, and download a Rift build. Local side (Milestones 2-3):
install the stubs into a game folder, prepare a Proton prefix and launch
through the injector.
"""

from __future__ import annotations

import argparse
import datetime
import getpass
import os
import shutil
import subprocess
import sys
from pathlib import Path

from . import pe, store
from .download import download_build

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = REPO_ROOT / "build"

PLATFORM_DLL = "LibOVRPlatform64_1.dll"
RUNTIME_DLL = "LibOVRRT64_1.dll"
INJECTOR = "ovrinject.exe"
BACKUP_SUFFIX = ".ovrl-orig"
# The shim is injected under its own name so it never overwrites the genuine
# LibOVRRT64_1.dll that the game's signature check reads from disk.
SHIM_INJECT_NAME = "ovrstub_shim.dll"
OPENVR_DLL = "openvr_api.dll"

# Both stub DLLs embed this marker (see the stub sources), so we can tell our
# own build apart from a game's genuine file.
STUB_MARKER = b"OVRSTUB-BUILD-MARKER-v1"


def _is_our_stub(path: Path) -> bool:
    try:
        return STUB_MARKER in path.read_bytes()
    except OSError:
        return False


def _check_real_dll(path: Path, name: str) -> Path:
    """Validate a user-supplied genuine Meta DLL before installing it."""
    if not path.is_file():
        sys.exit(f"ovrl: {path} not found")
    data = path.read_bytes()
    if data[:2] != b"MZ":
        sys.exit(f"ovrl: {path} is not a Windows DLL")
    if STUB_MARKER in data:
        sys.exit(f"ovrl: {path} is an ovrstub build, not Meta's signed {name}; "
                 "supply the real file from a Meta PC runtime install")
    return path

# Where the fake Oculus install lives inside the prefix, as Windows and Unix paths.
STUB_BASE_WIN = "C:\\ovrstub\\"
STUB_BASE_REL = Path("drive_c/ovrstub")
OCULUS_KEY = r"HKLM\Software\Oculus VR, LLC\Oculus"


def _build_file(build: Path, name: str) -> Path:
    path = build / name
    if not path.is_file():
        sys.exit(f"ovrl: {path} not found; run `make` first")
    return path


def _wine_prefix(prefix: Path) -> Path:
    """Accept either a Proton compatdata dir or a plain Wine prefix."""
    return prefix / "pfx" if (prefix / "pfx").is_dir() else prefix


def _install_file(src: Path, target: Path, label: str) -> None:
    """Copy src to target, backing up any pre-existing non-ours file first."""
    backup = target.with_name(target.name + BACKUP_SUFFIX)
    if target.exists() and not backup.exists() and not _is_our_stub(target):
        shutil.copy2(target, backup)
        print(f"backed up original to {backup}")
    shutil.copy2(src, target)
    print(f"installed {label} {target}")


def _install_stubs(game_dir: Path, build: Path,
                   real_runtime: str | None, real_platform: str | None) -> None:
    # The game's LibOVR loader often verifies a DLL is Meta-signed before
    # loading it. With a real runtime/platform we place Meta's own genuine
    # signed DLL here so that check passes against a real file; the injected
    # shim's LoadLibrary redirect still swaps our implementation in at load,
    # so Meta's service code never runs. When no real file is given for a
    # slot, our stub is installed there.
    real = {RUNTIME_DLL: real_runtime, PLATFORM_DLL: real_platform}
    for name in (RUNTIME_DLL, PLATFORM_DLL):
        is_real = bool(real[name])
        src = _check_real_dll(Path(real[name]), name) if is_real \
            else _build_file(build, name)
        _install_file(src, game_dir / name, "real " + name if is_real else "stub")


def _restore_dir(game_dir: Path) -> bool:
    """Undo stubs/steam-setup in a game folder; return whether anything changed."""
    restored = False
    for name in (RUNTIME_DLL, PLATFORM_DLL, OPENVR_DLL):
        target = game_dir / name
        backup = game_dir / (name + BACKUP_SUFFIX)
        if backup.exists():
            shutil.move(str(backup), str(target))
            print(f"restored {target}")
            restored = True
        elif target.exists() and _is_our_stub(target):
            # Only remove a file we put here; leave a real runtime alone.
            target.unlink()
            print(f"removed stub {target} (no original to restore)")
            restored = True
    # The injector and the distinctly-named shim are ours alone; a game never
    # ships them, so remove them by name.
    for name in (SHIM_INJECT_NAME, INJECTOR):
        target = game_dir / name
        if target.is_file():
            target.unlink()
            print(f"removed {target}")
            restored = True
    return restored


def cmd_stubs(args: argparse.Namespace) -> int:
    # The LibOVR loader searches the exe's own folder for a bare
    # LibOVRRT64_1.dll (and the Platform SDK beside it), so both stubs go in
    # the game's Binaries directory. Any real file found there is backed up.
    game_dir = Path(args.game_dir)
    if not game_dir.is_dir():
        sys.exit(f"ovrl: {game_dir} is not a directory")

    if args.action == "install":
        _install_stubs(game_dir, Path(args.build),
                       args.real_runtime, args.real_platform)
    elif not _restore_dir(game_dir):
        print(f"ovrl: nothing to restore in {game_dir}", file=sys.stderr)
    return 0


def _find_openvr_api(override: str | None) -> Path | None:
    """Locate SteamVR's 64-bit Windows openvr_api.dll."""
    if override:
        p = Path(override).expanduser()
        return p if p.is_file() else None
    steam_roots = [
        Path.home() / ".steam/steam",
        Path.home() / ".steam/debian-installation",  # Debian/Mint packaging
        Path.home() / ".steam/root",
        Path.home() / ".local/share/Steam",
        Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam",  # Flatpak
    ]
    # Steam libraries can live elsewhere; also scan common library folders.
    rel = Path("steamapps/common/SteamVR/bin/win64/openvr_api.dll")
    for root in steam_roots:
        cand = root / rel
        if cand.is_file():
            return cand
    return None


def cmd_prefix_setup(args: argparse.Namespace) -> int:
    """Lay out a fake Oculus install in the prefix and register it.

    The game's LibOVR loader looks up the runtime through the Oculus "Base"
    registry value, so we point that at a directory holding our LibOVRRT shim.
    The injector + LoadLibrary redirect covers loaders that bypass this path.
    """
    build = Path(args.build)
    wine_prefix = _wine_prefix(Path(args.prefix))
    if not (wine_prefix / "drive_c").is_dir():
        sys.exit(f"ovrl: {wine_prefix} is not an initialized Wine prefix "
                 "(launch any game in it once first)")

    base = wine_prefix / STUB_BASE_REL
    runtime_dir = base / "Support" / "oculus-runtime"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(_build_file(build, RUNTIME_DLL), runtime_dir / RUNTIME_DLL)
    # Some engines (e.g. Unreal) load the Platform SDK from the Oculus install
    # rather than the game folder; the shim redirects those loads here.
    shutil.copy2(_build_file(build, PLATFORM_DLL), runtime_dir / PLATFORM_DLL)
    shutil.copy2(_build_file(build, INJECTOR), base / INJECTOR)
    print(f"copied shim, Platform stub and injector to {base}")

    # The shim renders through OpenVR, which needs openvr_api.dll. Proton only
    # puts it on the search path for Steam VR apps, so copy SteamVR's own
    # Windows build next to the shim; vr_backend loads it by full path.
    openvr = _find_openvr_api(args.openvr)
    if openvr:
        shutil.copy2(openvr, runtime_dir / "openvr_api.dll")
        print(f"copied openvr_api.dll from {openvr}")
    else:
        print("ovrl: warning: openvr_api.dll not found; VR output will be "
              "unavailable until it is present. Pass --openvr <path> or "
              "install SteamVR.", file=sys.stderr)

    env = dict(os.environ, WINEPREFIX=str(wine_prefix))
    for view in ("/reg:32", "/reg:64"):
        subprocess.run(
            [args.wine, "reg", "add", OCULUS_KEY, "/v", "Base", "/t", "REG_SZ",
             "/d", STUB_BASE_WIN, "/f", view],
            env=env, check=True)
    print(f"registered {OCULUS_KEY}\\Base = {STUB_BASE_WIN}")
    return 0


def _to_windows_path(path: str | Path) -> str:
    """Map a Unix path to Wine's Z: drive; leave Windows paths alone."""
    p = str(path)
    if _is_windows_path(p):
        return p
    return "Z:" + str(Path(p).resolve()).replace("/", "\\")


def _is_windows_path(p: str) -> bool:
    return (len(p) > 1 and p[1] == ":") or "\\" in p


def cmd_launch(args: argparse.Namespace) -> int:
    compat = Path(args.prefix)
    wine_prefix = _wine_prefix(compat)
    base = wine_prefix / STUB_BASE_REL
    injector = base / INJECTOR
    shim = base / "Support" / "oculus-runtime" / RUNTIME_DLL
    if not injector.is_file() or not shim.is_file():
        sys.exit("ovrl: prefix not set up; run `ovrl prefix-setup` first")

    env = dict(os.environ)
    # Meta's LibOVR loader (built into games) checks LIBOVR_DLL_DIR for the
    # runtime's folder; it prepends the value as-is, so keep the backslash.
    env.setdefault("LIBOVR_DLL_DIR", STUB_BASE_WIN + "Support\\oculus-runtime\\")
    if not _is_windows_path(args.game_exe):
        env.setdefault("OVRSTUB_LOG", _to_windows_path(
            Path(args.game_exe).resolve().parent / "ovrstub.log"))
    if args.proton:
        env["STEAM_COMPAT_DATA_PATH"] = str(compat)
        env.setdefault("STEAM_COMPAT_CLIENT_INSTALL_PATH",
                       str(Path.home() / ".steam" / "steam"))
        runner = [str(Path(args.proton) / "proton"), "run"]
    else:
        env["WINEPREFIX"] = str(wine_prefix)
        runner = [args.wine]

    # The injector runs inside Wine, so everything it opens needs a Windows path.
    cmd = runner + [str(injector), "--wait", _to_windows_path(shim), "--",
                    _to_windows_path(args.game_exe), *args.game_args]
    print("+ " + " ".join(cmd))
    return subprocess.call(cmd, env=env)


def cmd_steam_setup(args: argparse.Namespace) -> int:
    """Prepare a game folder to be launched through Steam as a non-Steam game.

    Launching under a bare `proton run` leaves Proton's OpenVR bridge
    (vrclient) without the Steam runtime environment that points it at the
    native Linux SteamVR server, so VR_InitInternal fails. Steam sets that up
    itself for any app it launches. So instead of a prefix we stage everything
    next to the game exe and let Steam drive the launch: the genuine runtime
    (for the signature check), our shim under its own name, the injector and
    openvr_api.dll all live in the exe's folder, and Steam's shortcut points at
    the injector, which starts the game with the shim injected.
    """
    exe = Path(args.game_exe)
    if not exe.is_file():
        sys.exit(f"ovrl: {exe} not found")
    exe_dir = exe.parent
    build = Path(args.build)

    # Genuine runtime + Platform stub next to the exe for the signature check.
    _install_stubs(exe_dir, build, args.real_runtime, args.real_platform)
    # Our shim (distinct name), the injector and OpenVR beside the exe.
    _install_file(_build_file(build, RUNTIME_DLL), exe_dir / SHIM_INJECT_NAME, "shim")
    _install_file(_build_file(build, INJECTOR), exe_dir / INJECTOR, "injector")

    openvr = _find_openvr_api(args.openvr)
    if openvr:
        _install_file(openvr, exe_dir / OPENVR_DLL, OPENVR_DLL)
    else:
        print(f"ovrl: warning: {OPENVR_DLL} not found; VR output will be "
              "unavailable until it is present. Pass --openvr <path> or "
              "install SteamVR.", file=sys.stderr)

    # Steam runs the launch options as a shell command with %command% expanded
    # to `proton run <target.exe>`. A leading VAR=val sets an env var that
    # reaches the Wine process; everything after %command% is passed to the
    # injector. Relative names resolve against the shortcut's "Start In" dir.
    hmd = f" -hmd={args.hmd}" if args.hmd else ""
    launch_opts = (f'OVRSTUB_INSTALL_ROOT_CA=1 %command% --wait {SHIM_INJECT_NAME} '
                   f'-- "{exe.name}"{hmd}')
    target = exe_dir / INJECTOR
    print("\nAdd the game to Steam as a non-Steam game, then in its Properties:")
    print(f"  Target / exe : {target}")
    print(f"  Start In     : {exe_dir}")
    print(f"  Launch opts  : {launch_opts}")
    print("  Compatibility: force Proton (the same version you use elsewhere)")
    print("\nStart SteamVR, then launch the entry from your Steam library. "
          "Steam wires up the VR bridge for you.")
    return 0


def cmd_exports(args: argparse.Namespace) -> int:
    return pe.main(args.files)


def cmd_login(args: argparse.Namespace) -> int:
    token = args.token or getpass.getpass("Meta access token (input hidden): ")
    token = store.clean_token(token)
    problem = store.token_problem(token)
    if problem:
        sys.exit(f"ovrl: that doesn't look like a user token: {problem}")
    user = store.current_user(token)
    path = store.save_token(token)
    print(f"logged in as {user.get('alias') or user.get('display_name') or user.get('id')}")
    print(f"token saved to {path} (readable only by you)")
    return 0


def cmd_library(args: argparse.Namespace) -> int:
    apps = store.entitlements(store.load_token())
    if not apps:
        print("no owned apps returned")
        return 0
    width = max(len(a.id) for a in apps)
    for a in apps:
        print(f"{a.id:<{width}}  {a.name}")
    print(f"\n{len(apps)} apps. Quest and Rift versions of a game have different "
          "IDs; `ovrl versions <id>` shows a Rift app's builds.")
    return 0


def _date(ts: int) -> str:
    return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%d") if ts else "?"


def cmd_versions(args: argparse.Namespace) -> int:
    name, builds = store.app_builds(store.load_token(), args.app_id)
    print(f"{name or 'App'} ({args.app_id})")
    for b in builds:
        channels = f"  [{', '.join(c for c in b.channels if c)}]" if any(b.channels) else ""
        print(f"  build {b.id}  version {b.version} (code {b.version_code})"
              f"  {_date(b.created)}{channels}")
    return 0


def cmd_download(args: argparse.Namespace) -> int:
    token = store.load_token()
    build_id = args.build_id
    name = args.app_id
    if not build_id:
        name, builds = store.app_builds(token, args.app_id)
        if not builds:
            sys.exit(f"ovrl: {name or args.app_id} has no downloadable builds")
        live = [b for b in builds if any(c.upper() == "LIVE" for c in b.channels)]
        chosen = (live or builds)[0]
        build_id = chosen.id
        print(f"{name or args.app_id}: build {chosen.id}, version {chosen.version}")
    dest = Path(args.dest) if args.dest else Path.cwd() / f"{args.app_id}-{build_id}"
    manifest = download_build(token, build_id, dest, workers=args.jobs)
    print(f"done: {dest}")
    launch = manifest.get("launchFile")
    if launch:
        params = manifest.get("launchParameters") or ""
        print(f"launch file: {dest / launch}" + (f"  (args: {params})" if params else ""))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="ovrl", description=__doc__.splitlines()[0])
    p.add_argument("--build", default=str(DEFAULT_BUILD),
                   help="directory with the built DLLs (default: %(default)s)")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("stubs", help="install/restore the stubs in a game's exe folder")
    s.add_argument("action", choices=["install", "restore"])
    s.add_argument("game_dir")
    s.add_argument("--real-runtime", metavar="LibOVRRT64_1.dll",
                   help="genuine Meta-signed runtime to place for the signature "
                        "check; the shim still provides the implementation")
    s.add_argument("--real-platform", metavar="LibOVRPlatform64_1.dll",
                   help="genuine Meta-signed Platform SDK, for games that also "
                        "check its signature (the stub still provides it)")
    s.set_defaults(func=cmd_stubs)

    s = sub.add_parser("prefix-setup", help="install the LibOVR shim into a Wine/Proton prefix")
    s.add_argument("prefix", help="Proton compatdata dir or Wine prefix")
    s.add_argument("--wine", default="wine", help="wine binary to run `reg` with")
    s.add_argument("--openvr", metavar="openvr_api.dll",
                   help="path to SteamVR's openvr_api.dll (auto-detected if omitted)")
    s.set_defaults(func=cmd_prefix_setup)

    s = sub.add_parser("launch", help="launch a game through the injector")
    s.add_argument("prefix", help="Proton compatdata dir or Wine prefix")
    s.add_argument("game_exe", help="game executable (Windows or Unix path)")
    s.add_argument("game_args", nargs=argparse.REMAINDER)
    s.add_argument("--proton", help="Proton install dir; plain Wine is used if omitted")
    s.add_argument("--wine", default="wine")
    s.set_defaults(func=cmd_launch)

    s = sub.add_parser("steam-setup",
                       help="stage a game folder for launch through Steam as a "
                            "non-Steam game (lets Proton wire up VR)")
    s.add_argument("game_exe", help="the game's .exe (stubs install beside it)")
    s.add_argument("--real-runtime", metavar="LibOVRRT64_1.dll",
                   help="genuine Meta-signed runtime for the signature check")
    s.add_argument("--real-platform", metavar="LibOVRPlatform64_1.dll",
                   help="genuine Meta-signed Platform SDK (the stub is used otherwise)")
    s.add_argument("--openvr", metavar="openvr_api.dll",
                   help="path to SteamVR's openvr_api.dll (auto-detected if omitted)")
    s.add_argument("--hmd", default="OculusRift",
                   help="value for the game's -hmd arg (default: %(default)s; "
                        "pass empty to omit)")
    s.set_defaults(func=cmd_steam_setup)

    s = sub.add_parser("exports", help="list or diff DLL exports")
    s.add_argument("files", nargs="+")
    s.set_defaults(func=cmd_exports)

    s = sub.add_parser("login", help="save your Meta access token (see README)")
    s.add_argument("--token", help="token to save; prompted for (hidden) if omitted")
    s.set_defaults(func=cmd_login)

    s = sub.add_parser("library", help="list apps your account owns")
    s.set_defaults(func=cmd_library)

    s = sub.add_parser("versions", help="list a Rift app's builds")
    s.add_argument("app_id")
    s.set_defaults(func=cmd_versions)

    s = sub.add_parser("download", help="download a Rift build (resumable)")
    s.add_argument("app_id")
    s.add_argument("--build-id", help="specific build (default: newest LIVE build)")
    s.add_argument("--dest", help="target directory (default: ./<app>-<build>)")
    s.add_argument("--jobs", type=int, default=8,
                   help="parallel segment downloads (default: %(default)s)")
    s.set_defaults(func=cmd_download)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except store.StoreError as e:
        print(f"ovrl: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted; re-run the same command to resume", file=sys.stderr)
        return 130
