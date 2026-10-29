"""ovrl: set up and launch Rift Store games under Proton with the stub DLLs.

Only the local half of the launcher exists so far (Milestones 2-3): installing
the stubs into a game folder, preparing a Proton prefix and launching through
the injector. Store login, library and downloads (Milestones 1 and 6) are
placeholders until download feasibility is confirmed.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from . import pe

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = REPO_ROOT / "build"

PLATFORM_DLL = "LibOVRPlatform64_1.dll"
RUNTIME_DLL = "LibOVRRT64_1.dll"
INJECTOR = "ovrinject.exe"
BACKUP_SUFFIX = ".ovrl-orig"

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


def cmd_stubs(args: argparse.Namespace) -> int:
    game_dir = Path(args.game_dir)
    target = game_dir / PLATFORM_DLL
    backup = game_dir / (PLATFORM_DLL + BACKUP_SUFFIX)

    if args.action == "install":
        if not target.exists():
            print(f"ovrl: warning: {target} does not exist; this game may not "
                  "use the Platform SDK", file=sys.stderr)
        elif not backup.exists():
            shutil.copy2(target, backup)
            print(f"backed up original to {backup}")
        shutil.copy2(_build_file(Path(args.build), PLATFORM_DLL), target)
        print(f"installed stub {target}")
    else:
        if not backup.exists():
            sys.exit(f"ovrl: no backup at {backup}")
        shutil.move(backup, target)
        print(f"restored {target}")
    return 0


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
    shutil.copy2(_build_file(build, INJECTOR), base / INJECTOR)
    print(f"copied shim and injector to {base}")

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


def cmd_exports(args: argparse.Namespace) -> int:
    return pe.main(args.files)


def cmd_not_yet(args: argparse.Namespace) -> int:
    print(f"ovrl {args.command}: not implemented yet. Store login, library and "
          "downloads depend on Milestone 1 (download feasibility); see "
          "docs/PLAN.md.", file=sys.stderr)
    return 1


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="ovrl", description=__doc__.splitlines()[0])
    p.add_argument("--build", default=str(DEFAULT_BUILD),
                   help="directory with the built DLLs (default: %(default)s)")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("stubs", help="install/restore the Platform SDK stub in a game folder")
    s.add_argument("action", choices=["install", "restore"])
    s.add_argument("game_dir")
    s.set_defaults(func=cmd_stubs)

    s = sub.add_parser("prefix-setup", help="install the LibOVR shim into a Wine/Proton prefix")
    s.add_argument("prefix", help="Proton compatdata dir or Wine prefix")
    s.add_argument("--wine", default="wine", help="wine binary to run `reg` with")
    s.set_defaults(func=cmd_prefix_setup)

    s = sub.add_parser("launch", help="launch a game through the injector")
    s.add_argument("prefix", help="Proton compatdata dir or Wine prefix")
    s.add_argument("game_exe", help="game executable (Windows or Unix path)")
    s.add_argument("game_args", nargs=argparse.REMAINDER)
    s.add_argument("--proton", help="Proton install dir; plain Wine is used if omitted")
    s.add_argument("--wine", default="wine")
    s.set_defaults(func=cmd_launch)

    s = sub.add_parser("exports", help="list or diff DLL exports")
    s.add_argument("files", nargs="+")
    s.set_defaults(func=cmd_exports)

    for name in ("login", "library", "download"):
        s = sub.add_parser(name, help="(not implemented yet)")
        s.set_defaults(func=cmd_not_yet)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)
