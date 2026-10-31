"""One-step game management: download, stage, add to Steam, update shims.

Shared by the `install`, `add-queued`, `update-shims` and `setup` commands
and by the browser UI (gui.py). Settings live in ~/.config/ovrl/config.json,
installed games in games.json and the store library cache in library.json.
"""

from __future__ import annotations

import json
import re
import shutil
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


def _manifest_kind(m: dict) -> tuple[str, str]:
    """('pc' | 'quest' | 'other', detail) from a build's manifest."""
    launch = (m.get("launchFile") or "").lower()
    files = [f.lower() for f in (m.get("files") or {})]
    if launch.endswith(".exe") or (not launch and any(f.endswith(".exe") for f in files)):
        return "pc", m.get("launchFile") or ""
    if launch.endswith(".apk") or any(f.endswith(".apk") for f in files):
        return "quest", "Android APK"
    return "other", f"launch file {launch or 'missing'}"


def build_kind(token: str, build_id: str) -> str:
    """Whether one build is for PC ('pc'), Quest ('quest') or neither.

    Cross-buy apps list their Quest and PC builds under one app id, and the
    store's version list doesn't say which is which, so ask each build's
    manifest: a PC build launches a Windows .exe, a Quest build is an APK
    (the CDN often has no PC manifest for it at all). Builds never change,
    so answers are cached in build_kinds.json.
    """
    with _lock:
        cache = _read("build_kinds.json")
    if build_id in cache:
        return cache[build_id]
    try:
        kind, _ = _manifest_kind(fetch_manifest(token, build_id))
    except store.StoreError:
        kind = "quest"
    with _lock:
        cache = _read("build_kinds.json")
        cache[build_id] = kind
        _write("build_kinds.json", cache)
    return kind


def _newest_pc_build(token: str, builds: list) -> object | None:
    """The newest PC build: LIVE ones first, then any other channel."""
    live = [b for b in builds if any(c.upper() == "LIVE" for c in b.channels)]
    rest = [b for b in builds if b not in live]
    for b in live + rest:
        if build_kind(token, b.id) == "pc":
            return b
    return None


def classify(token: str, app_id: str) -> dict:
    """Decide whether an owned app is a PC (Rift) game we can install.

    The entitlement list doesn't say, Quest and Rift versions of a game often
    have separate IDs, and cross-buy apps mix both kinds of build under one
    ID. So look for the newest PC build; failing that, the app is Quest-only
    (or a system/bundle entry with no builds at all).
    """
    try:
        name, builds = store.app_builds(token, app_id)
    except store.StoreError as e:
        info = {"kind": "other", "reason": str(e)}
    else:
        if not builds:
            info = {"kind": "other", "reason": "no downloadable builds"}
        else:
            b = _newest_pc_build(token, builds)
            if b is not None:
                info = {"kind": "pc", "build_id": b.id, "version": b.version}
            else:
                newest = _newest_build(builds)
                info = {"kind": "quest", "build_id": newest.id, "version": newest.version,
                        "reason": "no PC build (Quest/Android only)"}
    _update_cached_app(app_id, **{"kind": None, "reason": None, **info})
    return info


# --- install ---

def library_name(app_id: str, token: str | None = None) -> str:
    """The app's name from your library: the build list sometimes has none."""
    for a in cached_library().get("apps", []):
        if a["id"] == app_id and a.get("name"):
            return a["name"]
    if token:
        for a in store.entitlements(token):
            if a.id == app_id:
                return a.name
    return ""


def _folder_name(name: str, app_id: str) -> str:
    words = re.findall(r"[A-Za-z0-9]+", name)
    return "".join(w[:1].upper() + w[1:] for w in words) or app_id


def detect_exe(dest: Path, manifest: dict) -> tuple[Path, str, str]:
    """(exe to run, -hmd value, engine) for a downloaded Rift build.

    Unreal ships a small bootstrap exe at the root that relaunches the real
    <Project>/Binaries/Win64/*-Shipping.exe; injecting into the bootstrap
    would miss the game, so pick the shipping exe and pass the -hmd= name
    its Oculus plugin answers to (see cli.unreal_hmd_name).
    Unity's exe sits beside <exe>_Data and takes no -hmd.
    """
    if (dest / "Engine").is_dir():
        shipping = sorted(p for p in dest.glob("*/Binaries/Win64/*-Shipping.exe")
                          if p.parts[-4] != "Engine")
        if shipping:
            return shipping[0], cli.unreal_hmd_name(dest), "Unreal"
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


def versions(app_id: str, token: str | None = None) -> list[dict]:
    """Every downloadable PC build of an app, newest first (Quest builds of
    cross-buy apps are left out)."""
    from concurrent.futures import ThreadPoolExecutor
    token = token or store.load_token()
    _, builds = store.app_builds(token, app_id)
    with ThreadPoolExecutor(max_workers=6) as pool:
        kinds = list(pool.map(lambda b: build_kind(token, b.id), builds))
    pc = [b for b, k in zip(builds, kinds) if k == "pc"]
    default = _newest_pc_build(token, pc)
    return [{"id": b.id, "version": b.version, "code": b.version_code,
             "created": b.created, "channels": [c for c in b.channels if c],
             "default": default is not None and b.id == default.id} for b in pc]


def install(app_id: str, token: str | None = None, on_progress=None,
            log=print, build_id: str | None = None) -> dict:
    """Download a build of a PC app (default: the newest LIVE one), stage it
    and add it to Steam.

    If Steam is running the game is queued; `add_queued()` (or the UI's
    "Add queued games to Steam") adds every queued game once Steam is closed.
    """
    token = token or store.load_token()
    cfg = load_config()
    if not cfg.get("real_runtime"):
        raise LibraryError("set real_runtime first (`ovrl setup --real-runtime "
                           "<path to genuine LibOVRRT64_1.dll>`)")
    name, builds = store.app_builds(token, app_id)
    name = name or library_name(app_id, token)
    if not builds:
        raise LibraryError(f"{name or app_id} has no downloadable builds")
    if build_id:
        build = next((b for b in builds if b.id == build_id), None)
        if build is None:
            raise LibraryError(f"{name or app_id} has no build {build_id}")
        if build_kind(token, build.id) != "pc":
            raise LibraryError(f"build {build_id} of {name or app_id} is a Quest "
                               "build, not a PC one; pick another version")
    else:
        build = _newest_pc_build(token, builds)
        if build is None:
            raise LibraryError(f"{name or app_id} has no PC build (it looks like a "
                               "Quest app); pick its Rift version instead")
    log(f"{name}: build {build.id}, version {build.version}")

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
        cli._install_file(platform, exe_dir / cli.PLATFORM_INJECT_NAME, "Platform stub")
        cli.write_identity(exe_dir)
        for name, src in ((cli.PLATFORM_DLL, platform), (cli.RUNTIME_DLL, shim)):
            target = exe_dir / name
            if target.is_file() and cli._is_our_stub(target):
                cli._install_file(src, target, "stub")
        updated.append(g["name"])
    log(f"updated {len(updated)} game(s)")
    return updated


# --- organize old downloads (folders named by store id) ---

def _owned_names(token: str | None) -> dict:
    """App id -> name from the cached library, refreshed once if needed."""
    names = {a["id"]: a.get("name", "") for a in cached_library().get("apps", [])}
    if not names and token:
        names = {a["id"]: a.get("name", "") for a in refresh_library(token).get("apps", [])}
    return names


def scan_numbered(root: str, token: str | None = None) -> list[dict]:
    """Game folders under `root` named by store id (e.g. 1016967501697367),
    with the name each would get and where it would move to."""
    root_p = Path(root).expanduser()
    if not root_p.is_dir():
        raise LibraryError(f"{root_p} is not a folder")
    cfg = load_config()
    names = _owned_names(token)
    found = []
    for d in sorted(root_p.iterdir()):
        if not (d.is_dir() and d.name.isdigit() and len(d.name) >= 9):
            continue
        name = names.get(d.name, "")
        if not name and token:
            try:
                name, _ = store.app_builds(token, d.name)
            except store.StoreError:
                name = ""
        entry = {"app_id": d.name, "path": str(d), "name": name, "target": "", "problem": ""}
        if not name:
            entry["problem"] = "name unknown (log in, or refresh the library)"
        else:
            target = Path(cfg["games_dir"]).expanduser() / _folder_name(name, d.name)
            entry["target"] = str(target)
            if target.exists():
                entry["problem"] = f"{target} already exists"
        found.append(entry)
    return found


def organize(root: str, token: str | None = None, log=print) -> list[str]:
    """Rename numbered game folders under `root` to game names, move them
    into games_dir, repoint their Steam shortcuts and register them.

    Steam must be closed (it rewrites shortcuts.vdf on exit). Shortcuts are
    checked before anything moves and written once after, keeping each
    entry's appid so its Proton setting survives.
    """
    if cli._steam_running():
        raise LibraryError("quit Steam fully first (Steam > Exit): its shortcuts "
                           "will be updated to the new folders")
    plan = [e for e in scan_numbered(root, token) if not e["problem"]]
    if not plan:
        log("nothing to move")
        return []
    cfg = load_config()
    Path(cfg["games_dir"]).expanduser().mkdir(parents=True, exist_ok=True)

    shortcuts = top = None
    try:
        _, user_dir = _call(cli.steam_user_dir, cfg.get("steam_root"), cfg.get("steam_user"))
        shortcuts = user_dir / "config" / "shortcuts.vdf"
    except LibraryError as e:
        log(f"Steam shortcuts won't be updated: {e}")
    if shortcuts and shortcuts.is_file():
        orig = shortcuts.read_bytes()
        top = cli.steam.parse_shortcuts(orig)
        if cli.steam.dump_shortcuts(top) != orig:
            raise LibraryError(f"refusing to edit {shortcuts}: it doesn't round-trip "
                               "through our parser")

    moved = []
    for e in plan:
        src, dst = Path(e["path"]), Path(e["target"])
        log(f"moving {src} -> {dst}")
        shutil.move(str(src), str(dst))
        in_steam = bool(top and cli.steam.rewrite_shortcut_paths(top, str(src), str(dst)))
        if in_steam:
            log(f"  updated its Steam shortcut")
        try:
            manifest = json.loads((dst / "manifest.json").read_text())
        except (OSError, ValueError):
            manifest = {}
        try:
            exe, hmd, engine = detect_exe(dst, manifest)
        except LibraryError as err:
            log(f"  not registered: {err}")
        else:
            _save_game(e["app_id"], {
                "app_id": e["app_id"], "name": e["name"], "build_id": "",
                "version": "", "dir": str(dst), "exe": str(exe), "hmd": hmd,
                "engine": engine, "in_steam": in_steam})
        moved.append(e["name"])

    if top is not None:
        shutil.copy2(shortcuts, shortcuts.with_suffix(".vdf.ovrl-bak"))
        shortcuts.write_bytes(cli.steam.dump_shortcuts(top))
        log(f"wrote {shortcuts}")
    log(f"moved {len(moved)} game(s)")
    return moved


def set_proton(tool: str | None = None, log=print) -> list[str]:
    """Set the Proton of every game added through ovrl (Steam closed)."""
    cfg = load_config()
    return _call(cli.set_proton_for_shortcuts, tool or cfg.get("proton_tool"),
                 cfg.get("steam_root"), cfg.get("steam_user"), log=log)


def apply_identity(token: str | None = None, log=print) -> dict:
    """Fetch your Meta identity and write it beside every game's shim: the
    installed games and any other folder under games_dir holding our shim."""
    try:
        token = token or store.load_token()
    except store.StoreError:
        token = None
    ident = cli.fetch_identity(token) if token else cli.cached_identity()
    if not ident:
        raise LibraryError("log in first, so your identity can be fetched")
    dirs = {Path(g["exe"]).parent for g in games().values()}
    root = Path(load_config()["games_dir"]).expanduser()
    if root.is_dir():
        dirs |= {p.parent for p in root.rglob(cli.SHIM_INJECT_NAME)}
    n = 0
    for d in sorted(dirs):
        if (d / cli.SHIM_INJECT_NAME).is_file() and cli.write_identity(d, ident):
            n += 1
    log(f"wrote your Meta identity ({ident['oculus_id']}) beside {n} game(s)")
    return ident
