"""One-step game management: download, stage, add to Steam, update shims.

Shared by the `install`, `add-queued`, `update-shims` and `setup` commands
and by the browser UI (gui.py). Settings live in ~/.config/ovrl/config.json,
installed games in games.json and the store library cache in library.json.
"""

from __future__ import annotations

import json
import re
import threading
from pathlib import Path

from . import cli, store
from .download import download_build, fetch_manifest

CONFIG_KEYS = {
    "real_runtime": "genuine Meta-signed LibOVRRT64_1.dll (for the signature check)",
    "real_platform": "genuine Meta-signed LibOVRPlatform64_1.dll (optional)",
    "prefix": "Proton prefix (compatdata dir) all games run in (optional)",
    "games_dir": "where games are downloaded",
    "proton_tool": "Steam compat tool, e.g. proton_11 (optional; auto-detected)",
    "openvr": "openvr_api.dll to copy beside games (optional; auto-detected)",
    "steam_root": "Steam install dir (optional; auto-detected)",
    "steam_user": "Steam userdata id if you have several users (optional)",
}
DEFAULTS = {"games_dir": str(Path.home() / "Games")}

_lock = threading.RLock()


class LibraryError(Exception):
    pass


def _call(fn, *args, **kwargs):
    """Run a cli helper, turning its sys.exit(msg) into a LibraryError."""
    try:
        return fn(*args, **kwargs)
    except SystemExit as e:
        raise LibraryError(str(e.code)) from None


def _read(name: str) -> dict:
    path = store.config_dir() / name
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {}


def _write(name: str, data: dict) -> None:
    path = store.config_dir() / name
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    tmp.replace(path)


# --- settings ---

def load_config() -> dict:
    cfg = dict(DEFAULTS)
    cfg.update({k: v for k, v in _read("config.json").items() if v not in (None, "")})
    return cfg


def save_config(updates: dict) -> dict:
    with _lock:
        cfg = _read("config.json")
        for k, v in updates.items():
            if k not in CONFIG_KEYS:
                raise LibraryError(f"unknown setting {k!r}")
            v = (v or "").strip()
            if v and k in ("real_runtime", "real_platform", "prefix", "games_dir",
                           "openvr", "steam_root"):
                v = str(Path(v).expanduser())
            if v:
                cfg[k] = v
            else:
                cfg.pop(k, None)
        for k in ("real_runtime", "real_platform", "openvr"):
            if cfg.get(k) and not Path(cfg[k]).is_file():
                raise LibraryError(f"{k}: {cfg[k]} not found")
        if cfg.get("real_runtime"):
            _call(cli._check_real_dll, Path(cfg["real_runtime"]), cli.RUNTIME_DLL)
        if cfg.get("real_platform"):
            _call(cli._check_real_dll, Path(cfg["real_platform"]), cli.PLATFORM_DLL)
        _write("config.json", cfg)
    return load_config()


# --- installed games ---

def games() -> dict:
    return _read("games.json")


def _save_game(app_id: str, game: dict) -> None:
    with _lock:
        all_games = _read("games.json")
        all_games[app_id] = game
        _write("games.json", all_games)


# --- store library, with a PC/Quest verdict per app ---

def cached_library() -> dict:
    """{"apps": [{id, name, kind?, ...}]} as last fetched, or empty."""
    return _read("library.json")


def refresh_library(token: str) -> dict:
    apps = store.entitlements(token)
    with _lock:
        old = {a["id"]: a for a in cached_library().get("apps", [])}
        merged = []
        for a in apps:
            entry = dict(old.get(a.id, {}))
            entry.update(id=a.id, name=a.name)
            merged.append(entry)
        lib = {"apps": merged}
        _write("library.json", lib)
    return lib


def _update_cached_app(app_id: str, **fields) -> None:
    with _lock:
        lib = cached_library()
        for a in lib.get("apps", []):
            if a["id"] == app_id:
                a.update(fields)
        _write("library.json", lib)


def _newest_build(builds: list) -> object | None:
    live = [b for b in builds if any(c.upper() == "LIVE" for c in b.channels)]
    return (live or builds or [None])[0]


def classify(token: str, app_id: str) -> dict:
    """Decide whether an owned app is a PC (Rift) game we can install.

    The entitlement list doesn't say, and Quest and Rift versions of a game
    have separate IDs. So look at the newest build: a Rift build's manifest
    names a Windows .exe to launch; a Quest build is an APK (the CDN serves
    no Rift manifest for it), and system/bundle entries have no builds.
    """
    try:
        name, builds = store.app_builds(token, app_id)
    except store.StoreError as e:
        info = {"kind": "other", "reason": str(e)}
    else:
        b = _newest_build(builds)
        if b is None:
            info = {"kind": "other", "reason": "no downloadable builds"}
        else:
            info = {"build_id": b.id, "version": b.version}
            try:
                m = fetch_manifest(token, b.id)
            except store.StoreError:
                info.update(kind="quest", reason="no PC manifest (Quest/Android build)")
            else:
                launch = (m.get("launchFile") or "").lower()
                files = [f.lower() for f in (m.get("files") or {})]
                if launch.endswith(".exe") or (not launch and any(f.endswith(".exe") for f in files)):
                    info.update(kind="pc", launch=m.get("launchFile") or "")
                elif launch.endswith(".apk") or any(f.endswith(".apk") for f in files):
                    info.update(kind="quest", reason="Android APK")
                else:
                    info.update(kind="other", reason=f"launch file {launch or 'missing'}")
    _update_cached_app(app_id, **{"kind": None, "reason": None, **info})
    return info


# --- install ---

def _folder_name(name: str, app_id: str) -> str:
    words = re.findall(r"[A-Za-z0-9]+", name)
    return "".join(w[:1].upper() + w[1:] for w in words) or app_id


def detect_exe(dest: Path, manifest: dict) -> tuple[Path, str, str]:
    """(exe to run, -hmd value, engine) for a downloaded Rift build.

    Unreal ships a small bootstrap exe at the root that relaunches the real
    <Project>/Binaries/Win64/*-Shipping.exe; injecting into the bootstrap
    would miss the game, so pick the shipping exe and pass -hmd=OculusRift.
    Unity's exe sits beside <exe>_Data and takes no -hmd.
    """
    if (dest / "Engine").is_dir():
        shipping = sorted(p for p in dest.glob("*/Binaries/Win64/*-Shipping.exe")
                          if p.parts[-4] != "Engine")
        if shipping:
            return shipping[0], "OculusRift", "Unreal"
    launch = manifest.get("launchFile") or ""
    exe = dest / launch if launch else None
    if not exe or not exe.is_file():
        exes = sorted(dest.glob("*.exe"))
        if not exes:
            raise LibraryError(f"no .exe found in {dest}")
        exe = exes[0]
    if exe.with_name(exe.stem + "_Data").is_dir():
        return exe, "", "Unity"
    return exe, "OculusRift", "unknown"


def install(app_id: str, token: str | None = None, on_progress=None,
            log=print) -> dict:
    """Download the newest build of a PC app, stage it and add it to Steam.

    If Steam is running the game is queued; `add_queued()` (or the UI's
    "Add queued games to Steam") adds every queued game once Steam is closed.
    """
    token = token or store.load_token()
    cfg = load_config()
    if not cfg.get("real_runtime"):
        raise LibraryError("set real_runtime first (`ovrl setup --real-runtime "
                           "<path to genuine LibOVRRT64_1.dll>`)")
    name, builds = store.app_builds(token, app_id)
    build = _newest_build(builds)
    if build is None:
        raise LibraryError(f"{name or app_id} has no downloadable builds")
    log(f"{name}: build {build.id}, version {build.version}")
    try:
        manifest_preview = fetch_manifest(token, build.id)
    except store.StoreError:
        raise LibraryError(f"{name or app_id} has no PC build (it looks like a "
                           "Quest app); pick its Rift version instead") from None
    if not (manifest_preview.get("launchFile") or "").lower().endswith(".exe"):
        raise LibraryError(f"{name or app_id} isn't a PC game (launch file "
                           f"{manifest_preview.get('launchFile')!r})")

    dest = Path(cfg["games_dir"]).expanduser() / _folder_name(name, app_id)
    log(f"downloading to {dest}")
    manifest = download_build(token, build.id, dest, on_progress=on_progress)

    exe, hmd, engine = detect_exe(dest, manifest)
    log(f"{engine} game, exe {exe}")
    _call(cli.stage_game, exe, cli.DEFAULT_BUILD, cfg.get("real_runtime"),
          cfg.get("real_platform"), cfg.get("openvr"), log=log)

    game = {"app_id": app_id, "name": name, "build_id": build.id,
            "version": build.version, "dir": str(dest), "exe": str(exe),
            "hmd": hmd, "engine": engine, "in_steam": False}
    _save_game(app_id, game)
    if cli._steam_running():
        log("Steam is running, so the game is queued; close Steam and run "
            "`python3 -m ovrl add-queued` (or use the UI button)")
    else:
        add_to_steam([app_id], log=log)
        game = games()[app_id]
    return game


def add_to_steam(app_ids: list[str] | None = None, log=print) -> list[str]:
    """Add installed games (default: all not yet in Steam) in one write."""
    cfg = load_config()
    all_games = games()
    ids = app_ids if app_ids is not None else \
        [i for i, g in all_games.items() if not g.get("in_steam")]
    ids = [i for i in ids if i in all_games]
    if not ids:
        log("nothing to add")
        return []
    _call(cli.add_shortcuts, [all_games[i] for i in ids], prefix=cfg.get("prefix"),
          proton_tool=cfg.get("proton_tool"), steam_root=cfg.get("steam_root"),
          steam_user=cfg.get("steam_user"), log=log)
    with _lock:
        all_games = games()
        for i in ids:
            all_games[i]["in_steam"] = True
        _write("games.json", all_games)
    log("start Steam: the games are in your library. Start SteamVR, then Play.")
    return ids


def update_shims(log=print) -> list[str]:
    """Copy the current build's shim, injector and stubs into every game.

    Only files that are ours are replaced: the genuine Meta DLLs placed for
    the signature check stay as they are.
    """
    build = cli.DEFAULT_BUILD
    shim = _call(cli._build_file, build, cli.RUNTIME_DLL)
    injector = _call(cli._build_file, build, cli.INJECTOR)
    platform = _call(cli._build_file, build, cli.PLATFORM_DLL)
    updated = []
    for app_id, g in games().items():
        exe_dir = Path(g["exe"]).parent
        if not exe_dir.is_dir():
            log(f"skipped {g['name']}: {exe_dir} is gone")
            continue
        cli._install_file(shim, exe_dir / cli.SHIM_INJECT_NAME, "shim")
        cli._install_file(injector, exe_dir / cli.INJECTOR, "injector")
        for name, src in ((cli.PLATFORM_DLL, platform), (cli.RUNTIME_DLL, shim)):
            target = exe_dir / name
            if target.is_file() and cli._is_our_stub(target):
                cli._install_file(src, target, "stub")
        updated.append(g["name"])
    log(f"updated {len(updated)} game(s)")
    return updated
