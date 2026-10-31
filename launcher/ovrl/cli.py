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
import re
import shutil
import subprocess
import sys
from pathlib import Path

from . import pe, steam, store
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
    # The injector and shim are ours alone (no game ships them), so a
    # previous copy is never an original worth keeping.
    ours = target.name in (INJECTOR, SHIM_INJECT_NAME)
    if target.exists() and not backup.exists() and not ours and not _is_our_stub(target):
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


def pe_file_version(path: Path) -> tuple[int, int, int, int]:
    """A PE file's version from its VS_FIXEDFILEINFO, or (0, 0, 0, 0)."""
    try:
        data = path.read_bytes()
    except OSError:
        return (0, 0, 0, 0)
    i = data.find(b"\xbd\x04\xef\xfe")     # VS_FIXEDFILEINFO signature
    if i < 0 or i + 16 > len(data):
        return (0, 0, 0, 0)
    ms = int.from_bytes(data[i + 8:i + 12], "little")
    ls = int.from_bytes(data[i + 12:i + 16], "little")
    return (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)


def _find_openvr_api(override: str | None) -> Path | None:
    """Locate the newest 64-bit Windows openvr_api.dll on this machine.

    SteamVR on Linux may not ship a Windows copy, so games' copies count too.
    They range from years-old OpenVR 1.0 builds (bundled with Unreal 4.1x)
    to current ones, and an old one can connect to SteamVR yet fail to hand
    out the interfaces our shim asks for. So take the highest version.
    """
    if override:
        p = Path(override).expanduser()
        return p if p.is_file() else None
    steam_roots = [
        Path.home() / ".steam/steam",
        Path.home() / ".steam/debian-installation",  # Debian/Mint packaging
        Path.home() / ".steam/root",
        Path.home() / ".local/share/Steam",
        Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam",  # Flatpak
        Path.home() / "snap/steam/common/.local/share/Steam",
    ]
    seen, found = set(), []
    for root in (r for r in steam_roots if r.is_dir()):
        for pattern in ("steamapps/common/SteamVR/bin/win64/openvr_api.dll",
                        "steamapps/common/*/bin/win64/openvr_api.dll",
                        "steamapps/common/*/*/Binaries/ThirdParty/OpenVR/*/Win64/openvr_api.dll",
                        "steamapps/common/*/*_Data/Plugins/x86_64/openvr_api.dll",
                        "steamapps/common/*/*_Data/Plugins/openvr_api.dll"):
            for cand in root.glob(pattern):
                real = cand.resolve()
                if real in seen or not cand.is_file() or not pe_is_x64(cand):
                    continue
                seen.add(real)
                found.append((_openvr_rank(cand), cand))
    if not found:
        return None
    (version, system, _), best = max(found, key=lambda rc: rc[0])
    print(f"using {best} (file version {'.'.join(map(str, version))}, "
          f"IVRSystem_{system:03d})", file=sys.stderr)
    return best


def _openvr_rank(path: Path) -> tuple:
    """Sort key for openvr_api.dll copies: newer sorts higher.

    The file version when the DLL has one; then the newest IVRSystem_NNN
    interface it names (always present, rises with each OpenVR SDK); then
    size as a last tie-break.
    """
    try:
        data = path.read_bytes()
    except OSError:
        return ((0, 0, 0, 0), 0, 0)
    systems = [int(m) for m in re.findall(rb"IVRSystem_(\d{3})", data)]
    return (pe_file_version(path), max(systems, default=0), len(data))


def pe_is_x64(path: Path) -> bool:
    """True for a 64-bit (x86-64) PE file."""
    try:
        data = path.read_bytes()[:4096]
        off = int.from_bytes(data[0x3C:0x40], "little")
        return data[off:off + 4] == b"PE\0\0" and \
            int.from_bytes(data[off + 4:off + 6], "little") == 0x8664
    except (OSError, ValueError):
        return False


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
        exe = Path(args.game_exe).resolve()
        env.setdefault("OVRSTUB_LOG", _to_windows_path(exe.parent / log_name(exe)))
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
    exe_dir = exe.parent
    stage_game(exe, Path(args.build), args.real_runtime, args.real_platform,
               args.openvr, log=lambda m: print(m, file=sys.stderr))

    # Steam runs the launch options as a shell command with %command% expanded
    # to `proton run <target.exe>`. A leading VAR=val sets an env var that
    # reaches the Wine process; everything after %command% is passed to the
    # injector. Relative names resolve against the shortcut's "Start In" dir.
    launch_opts = _steam_launch_options(exe, _auto_hmd(exe, args.hmd))
    target = exe_dir / INJECTOR
    print("\nAdd the game to Steam as a non-Steam game, then in its Properties:")
    print(f"  Target / exe : {target}")
    print(f"  Start In     : {exe_dir}")
    print(f"  Launch opts  : {launch_opts}")
    print("  Compatibility: force Proton (the same version you use elsewhere)")
    print("\nOr let `ovrl add-to-steam` write the shortcut for you (close Steam first).")
    print("Then start SteamVR and launch the entry from your Steam library; "
          "Steam wires up the VR bridge for you.")
    return 0


def log_name(exe: Path) -> str:
    """The shim's default log file name for a game exe (see src/common/log.c)."""
    return f"ovrstub-{exe.stem}.log"


def _steam_launch_options(exe: Path, hmd: str | None,
                          prefix: Path | None = None) -> str:
    """The Steam launch-options line that runs the game through the injector.

    Steam runs the launch options as a shell command with %command% expanded
    to `proton run <target.exe>`. A leading VAR=val sets an env var that
    reaches the Wine process; everything after %command% is passed to the
    injector. Relative names resolve against the shortcut's "Start In" dir.
    """
    # No OVRSTUB_LOG: the shim, staged beside the exe, logs to
    # ovrstub-<exe name>.log there by default (see log_name).
    env_prefix = "OVRSTUB_INSTALL_ROOT_CA=1"
    if prefix:
        # Steam would otherwise give the shortcut a fresh compatdata prefix;
        # overriding it reuses one already prepared with `ovrl prefix-setup`.
        env_prefix = f"STEAM_COMPAT_DATA_PATH={prefix} " + env_prefix
    hmd_arg = f" -hmd={hmd}" if hmd else ""
    return (f'{env_prefix} %command% --wait {SHIM_INJECT_NAME} '
            f'-- "{exe.name}"{hmd_arg}')


def _steam_root() -> Path | None:
    """The Steam install that holds userdata/ and config/."""
    for cand in (Path.home() / ".steam/steam",
                 Path.home() / ".steam/root",
                 Path.home() / ".steam/debian-installation",
                 Path.home() / ".local/share/Steam",
                 Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
                 Path.home() / "snap/steam/common/.local/share/Steam"):
        if (cand / "userdata").is_dir() and (cand / "config").is_dir():
            return cand.resolve()
    return None


def _steam_running() -> bool:
    try:
        return subprocess.run(["pgrep", "-x", "steam"], capture_output=True).returncode == 0
    except OSError:
        return False


def _steam_libraries(root: Path) -> list[Path]:
    """Every Steam library folder (steamapps/libraryfolders.vdf), root first."""
    libs = [root]
    vdf = root / "steamapps/libraryfolders.vdf"
    try:
        data = steam.parse_text_vdf(vdf.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return libs
    folders = next(iter(data.values()), {}) if data else {}
    for entry in (folders.values() if isinstance(folders, dict) else []):
        if isinstance(entry, dict) and entry.get("path"):
            lib = Path(entry["path"])
            if lib.is_dir() and lib.resolve() != root.resolve():
                libs.append(lib)
    return libs


def newest_proton_tool(root: Path) -> str | None:
    """Steam's name (proton_N) for the newest numbered Proton installed.

    Proton's VR bridge only offers the OpenVR interface versions that
    existed when it was built; older ones connect to SteamVR but hand our
    shim no IVRSystem/IVRCompositor. A newer Proton also upgrades a prefix
    an older one made, so the newest installed is the safe choice.
    """
    best = None
    for lib in _steam_libraries(root):
        for d in (lib / "steamapps/common").glob("Proton [0-9]*"):
            m = re.match(r"Proton (\d+)", d.name)
            if m and (d / "proton").is_file():
                best = max(best or 0, int(m.group(1)))
    return f"proton_{best}" if best else None


def _prefix_proton_tool(prefix: Path | None, cfg: dict) -> str | None:
    """The compat tool matching the Proton that created `prefix`, if known.

    Proton writes its version to <compatdata>/version (e.g. "10.0-201").
    Running a prefix with an older Proton than made it can break it (Proton
    9 fails the game's signature check in a Proton 10 prefix), so prefer
    the same major version when Steam already knows a tool by that name.
    """
    if not prefix:
        return None
    try:
        version = (prefix / "version").read_text().strip()
        major = int(version.split(".")[0].split("-")[0])
    except (OSError, ValueError):
        return None
    want = f"proton_{major}"
    known = {m.get("name") for m in steam.compat_mappings(cfg)}
    if want in known:
        return want
    print(f"ovrl: note: {prefix} was made by Proton {version}; if the game misbehaves, "
          f"set its Compatibility tool to that version in Steam (or pass --proton-tool)",
          file=sys.stderr)
    return None


def steam_user_dir(steam_root: str | None, steam_user: str | None) -> tuple[Path, Path]:
    """(Steam install root, userdata/<id>) to edit; exits with a reason if unclear."""
    root = Path(steam_root).expanduser() if steam_root else _steam_root()
    if not root or not (root / "userdata").is_dir():
        sys.exit("ovrl: Steam install not found; pass --steam-root")
    users = sorted(p for p in (root / "userdata").iterdir()
                   if p.name.isdigit() and p.name != "0")
    if steam_user:
        user_dir = root / "userdata" / steam_user
    elif len(users) == 1:
        user_dir = users[0]
    else:
        sys.exit("ovrl: several Steam users in userdata; pass --steam-user "
                 "with one of: " + ", ".join(u.name for u in users))
    if not user_dir.is_dir():
        sys.exit(f"ovrl: {user_dir} not found")
    return root, user_dir


def add_shortcuts(games: list[dict], prefix: str | None = None,
                  proton_tool: str | None = None, steam_root: str | None = None,
                  steam_user: str | None = None, log=print) -> None:
    """Add (or update) non-Steam shortcuts for staged games, as Heroic does.

    Each game is {"exe": path, "name": str, "hmd": str}. Edits
    userdata/<id>/config/shortcuts.vdf (binary) and config/config.vdf (text)
    directly, reading and writing each file once for the whole batch. Steam
    rewrites both on exit, so it must be closed. Each file is backed up and
    must round-trip through our parser before we change it. Exits with a
    reason on any problem, before writing anything.
    """
    root, user_dir = steam_user_dir(steam_root, steam_user)
    if _steam_running():
        sys.exit("ovrl: Steam is running; quit it fully first (it overwrites its "
                 "config on exit, which would discard this change)")
    pfx = None
    if prefix:
        pfx = Path(prefix).expanduser().resolve()
        if not (_wine_prefix(pfx) / "drive_c").is_dir():
            sys.exit(f"ovrl: {pfx} is not an initialized Proton prefix")
        if " " in str(pfx):
            sys.exit(f"ovrl: {pfx} contains a space, which Steam launch "
                     "options can't carry in a VAR=val prefix")

    planned = []
    for g in games:
        exe = Path(g["exe"]).resolve()
        exe_dir = exe.parent
        for name in (INJECTOR, SHIM_INJECT_NAME):
            if not (exe_dir / name).is_file():
                sys.exit(f"ovrl: {exe_dir / name} missing; run `ovrl steam-setup` first")
        name = g.get("name") or exe.stem.replace("-Win64-Shipping", "")
        exe_field = f'"{exe_dir / INJECTOR}"'
        appid = steam.shortcut_appid(exe_field, name)
        entry = steam.make_entry(appid, name, exe_field, f'"{exe_dir}/"',
                                 _steam_launch_options(exe, g.get("hmd"), pfx),
                                 openvr=True)
        planned.append((name, exe_field, appid, entry))

    # --- shortcuts.vdf ---
    shortcuts = user_dir / "config" / "shortcuts.vdf"
    if shortcuts.is_file():
        orig = shortcuts.read_bytes()
        top = steam.parse_shortcuts(orig)
        if steam.dump_shortcuts(top) != orig:
            sys.exit(f"ovrl: refusing to edit {shortcuts}: it doesn't round-trip "
                     "through our parser, so writing could lose entries")
    else:
        top = [[0x00, "shortcuts", []]]

    # --- config.vdf: force Proton for each shortcut ---
    config = root / "config" / "config.vdf"
    cfg = None
    tool = None
    if config.is_file():
        cfg = steam.parse_text_vdf(config.read_text(encoding="utf-8"))
        if steam.parse_text_vdf(steam.dump_text_vdf(cfg)) != cfg:
            sys.exit(f"ovrl: refusing to edit {config}: it doesn't round-trip "
                     "through our parser")
        tool = proton_tool or newest_proton_tool(root) \
            or _prefix_proton_tool(pfx, cfg) or steam.detect_proton_tool(cfg)

    if shortcuts.is_file():
        shutil.copy2(shortcuts, shortcuts.with_suffix(".vdf.ovrl-bak"))
    else:
        shortcuts.parent.mkdir(parents=True, exist_ok=True)
    for name, exe_field, appid, entry in planned:
        action = steam.upsert_shortcut(top, name, exe_field, entry)
        log(f"{action} shortcut \"{name}\" (appid {appid})")
        if cfg is not None and tool:
            steam.set_compat_tool(cfg, appid, tool)
    shortcuts.write_bytes(steam.dump_shortcuts(top))
    log(f"wrote {shortcuts}")
    if ".var/app/com.valvesoftware.Steam" in str(root):
        dirs = sorted({str(Path(g["exe"]).resolve().parent) for g in games})
        log("note: Flatpak Steam only sees folders it is allowed to; if a game "
            "won't start, run: flatpak override --user " +
            " ".join(f"--filesystem={d}" for d in dirs) + " com.valvesoftware.Steam")

    if cfg is not None and tool:
        shutil.copy2(config, config.with_suffix(".vdf.ovrl-bak"))
        config.write_text(steam.dump_text_vdf(cfg), encoding="utf-8")
        log(f"set compatibility tool to {tool} in {config}")
    else:
        log("ovrl: no Proton mapping set; after starting Steam, open each entry's "
            "Properties > Compatibility and force Proton (or pass --proton-tool)")


def cmd_add_to_steam(args: argparse.Namespace) -> int:
    """Write the non-Steam shortcut and Proton mapping for one staged game."""
    add_shortcuts([{"exe": args.game_exe, "name": args.name,
                    "hmd": _auto_hmd(Path(args.game_exe), args.hmd)}],
                  prefix=args.prefix, proton_tool=args.proton_tool,
                  steam_root=args.steam_root, steam_user=args.steam_user)
    name = args.name or Path(args.game_exe).stem.replace("-Win64-Shipping", "")
    print(f"\nStart Steam: \"{name}\" is in your library. Start SteamVR, then Play.")
    return 0


def unreal_hmd_name(game_root: Path) -> str:
    """The -hmd= value that selects an Unreal game's Oculus plugin.

    Unreal starts only the VR plugin whose name matches -hmd=. Older engines
    (4.1x) ship Engine/Plugins/Runtime/OculusRift, named "OculusRift"; from
    4.18 the plugin lives in Engine/Plugins/Runtime/Oculus/OculusVR and its
    module is "OculusHMD", so -hmd=OculusRift would match nothing there.
    """
    if (game_root / "Engine/Plugins/Runtime/Oculus/OculusVR").is_dir():
        return "OculusHMD"
    return "OculusRift"


def _auto_hmd(exe: Path, hmd: str | None) -> str | None:
    """Resolve --hmd auto from the game folder (Unreal only; else none)."""
    if hmd != "auto":
        return hmd
    for parent in exe.resolve().parents:
        if (parent / "Engine").is_dir():
            return unreal_hmd_name(parent)
    return ""


def stage_game(exe: Path, build: Path, real_runtime: str | None,
               real_platform: str | None, openvr: str | None, log=print) -> None:
    """Put the genuine runtime, Platform stub, shim, injector and OpenVR beside
    a game's exe so Steam can launch it through the injector."""
    if not exe.is_file():
        sys.exit(f"ovrl: {exe} not found")
    exe_dir = exe.parent
    # Genuine runtime + Platform stub next to the exe for the signature check.
    _install_stubs(exe_dir, build, real_runtime, real_platform)
    # Our shim (distinct name), the injector and OpenVR beside the exe.
    _install_file(_build_file(build, RUNTIME_DLL), exe_dir / SHIM_INJECT_NAME, "shim")
    _install_file(_build_file(build, INJECTOR), exe_dir / INJECTOR, "injector")
    found = _find_openvr_api(openvr)
    if found:
        _install_file(found, exe_dir / OPENVR_DLL, OPENVR_DLL)
    else:
        log(f"ovrl: warning: {OPENVR_DLL} not found; VR output will be "
            "unavailable until it is present. Pass --openvr <path> or "
            "install SteamVR.")


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
    token = store.load_token()
    apps = store.entitlements(token)
    if not apps:
        print("no owned apps returned")
        return 0
    width = max(len(a.id) for a in apps)
    if args.check:
        from . import library
        library.refresh_library(token)
        pc = 0
        for a in apps:
            info = library.classify(token, a.id)
            label = {"pc": "PC", "quest": "Quest"}.get(info["kind"], "-")
            pc += info["kind"] == "pc"
            print(f"{a.id:<{width}}  {label:<5}  {a.name}", flush=True)
        print(f"\n{len(apps)} apps, {pc} PC games. Install one with "
              "`python3 -m ovrl install <id>`.")
        return 0
    for a in apps:
        print(f"{a.id:<{width}}  {a.name}")
    print(f"\n{len(apps)} apps. Quest and Rift versions of a game have different "
          "IDs; `ovrl library --check` marks which are PC games.")
    return 0


def cmd_setup(args: argparse.Namespace) -> int:
    from . import library
    updates = {k: getattr(args, k) for k in library.CONFIG_KEYS
               if getattr(args, k) is not None}
    try:
        cfg = library.save_config(updates) if updates else library.load_config()
    except library.LibraryError as e:
        sys.exit(f"ovrl: {e}")
    for k in library.CONFIG_KEYS:
        print(f"{k:<14} {cfg.get(k, '')}")
    return 0


def _library_call(fn, *a, **kw):
    from . import library
    try:
        return fn(*a, **kw)
    except library.LibraryError as e:
        sys.exit(f"ovrl: {e}")


def cmd_install(args: argparse.Namespace) -> int:
    from . import library
    game = _library_call(library.install, args.app_id)
    print(f"\n{game['name']} installed in {game['dir']}")
    return 0


def cmd_add_queued(args: argparse.Namespace) -> int:
    from . import library
    _library_call(library.add_to_steam)
    return 0


def cmd_update_shims(args: argparse.Namespace) -> int:
    from . import library
    _library_call(library.update_shims)
    return 0


def cmd_gui(args: argparse.Namespace) -> int:
    from . import gui
    gui.serve(args.port, open_browser=not args.no_browser)
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
    if args.dest:
        dest = Path(args.dest)
    else:
        # Name the folder after the game, in your games folder (as install does).
        from . import library
        name = name if name != args.app_id else ""
        name = name or library.library_name(args.app_id, token)
        dest = Path(library.load_config()["games_dir"]).expanduser() / \
            library._folder_name(name, args.app_id)
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
    s.add_argument("--hmd", default="auto",
                   help="value for the game's -hmd arg (default: auto: "
                        "OculusRift or OculusHMD for Unreal games by engine "
                        "version, none otherwise; pass empty to omit)")
    s.set_defaults(func=cmd_steam_setup)

    s = sub.add_parser("add-to-steam",
                       help="write the non-Steam shortcut + Proton mapping into "
                            "Steam's config (run steam-setup first; Steam closed)")
    s.add_argument("game_exe", help="the game's .exe (as given to steam-setup)")
    s.add_argument("--name", help="library name (default: derived from the exe)")
    s.add_argument("--hmd", default="auto",
                   help="value for the game's -hmd arg (default: auto, as "
                        "for steam-setup)")
    s.add_argument("--proton-tool", metavar="NAME",
                   help="compat tool name, e.g. proton_11 (default: the newest "
                        "numbered Proton installed)")
    s.add_argument("--prefix", metavar="COMPATDATA",
                   help="Proton prefix (compatdata dir) to run in instead of the "
                        "fresh one Steam would create, e.g. one set up with "
                        "`ovrl prefix-setup`")
    s.add_argument("--steam-root", help="Steam install dir (auto-detected)")
    s.add_argument("--steam-user", metavar="ID",
                   help="userdata folder to use if you have several Steam users")
    s.set_defaults(func=cmd_add_to_steam)

    s = sub.add_parser("exports", help="list or diff DLL exports")
    s.add_argument("files", nargs="+")
    s.set_defaults(func=cmd_exports)

    s = sub.add_parser("login", help="save your Meta access token (see README)")
    s.add_argument("--token", help="token to save; prompted for (hidden) if omitted")
    s.set_defaults(func=cmd_login)

    s = sub.add_parser("library", help="list apps your account owns")
    s.add_argument("--check", action="store_true",
                   help="also mark which apps are PC games (one store lookup each)")
    s.set_defaults(func=cmd_library)

    s = sub.add_parser("setup", help="save settings used by install/gui "
                                     "(no options: show them)")
    s.add_argument("--real-runtime", metavar="LibOVRRT64_1.dll",
                   help="genuine Meta-signed runtime for the signature check")
    s.add_argument("--real-platform", metavar="LibOVRPlatform64_1.dll")
    s.add_argument("--prefix", metavar="COMPATDATA",
                   help="Proton prefix all games run in")
    s.add_argument("--games-dir", help="where games are downloaded (default ~/Games)")
    s.add_argument("--proton-tool", metavar="NAME", help="e.g. proton_11")
    s.add_argument("--openvr", metavar="openvr_api.dll")
    s.add_argument("--steam-root")
    s.add_argument("--steam-user", metavar="ID")
    s.set_defaults(func=cmd_setup)

    s = sub.add_parser("install", help="download a PC game, stage it and add it "
                                       "to Steam in one step")
    s.add_argument("app_id")
    s.set_defaults(func=cmd_install)

    s = sub.add_parser("add-queued", help="add installed games not yet in Steam "
                                          "(Steam closed)")
    s.set_defaults(func=cmd_add_queued)

    s = sub.add_parser("update-shims", help="copy the current build's shim into "
                                            "every installed game")
    s.set_defaults(func=cmd_update_shims)

    s = sub.add_parser("gui", help="open the library UI in your browser")
    s.add_argument("--port", type=int, default=8765)
    s.add_argument("--no-browser", action="store_true")
    s.set_defaults(func=cmd_gui)

    s = sub.add_parser("versions", help="list a Rift app's builds")
    s.add_argument("app_id")
    s.set_defaults(func=cmd_versions)

    s = sub.add_parser("download", help="download a Rift build (resumable)")
    s.add_argument("app_id")
    s.add_argument("--build-id", help="specific build (default: newest LIVE build)")
    s.add_argument("--dest", help="target directory (default: ~/Games/<GameName>, "
                                  "or your games_dir setting)")
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
