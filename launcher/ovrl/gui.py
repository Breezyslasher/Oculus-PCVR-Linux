"""`ovrl gui`: a small local web page for your Oculus library.

Lists the apps you own, marks which are PC (Rift) games, installs them in
one click (download, stage, add to Steam) and updates the shim in every
installed game. It serves only on 127.0.0.1 and every POST must carry a
per-run random token embedded in the page, so other websites open in your
browser can't drive it.
"""

from __future__ import annotations

import json
import os
import secrets
import shutil
import subprocess
import threading
import time
import webbrowser
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import cli, library, store


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.jobs: dict[str, dict] = {}     # app_id -> install job
        self.log: list[str] = []
        self.busy: str | None = None        # what the single worker is doing
        self.user: str | None = None
        self.server = None
        self.installs = ThreadPoolExecutor(max_workers=1)   # one download at a time
        self.misc = ThreadPoolExecutor(max_workers=1)
        self.checks = ThreadPoolExecutor(max_workers=4)

    def say(self, msg: str) -> None:
        print(msg, flush=True)
        with self.lock:
            self.log.append(f"{time.strftime('%H:%M:%S')} {msg}")
            del self.log[:-300]


S = State()


def _run(label: str, fn, *args):
    def task():
        S.busy = label
        try:
            fn(*args)
        except (library.LibraryError, store.StoreError, OSError) as e:
            S.say(f"error: {e}")
        except Exception as e:  # keep the server alive, but report it
            S.say(f"error: {type(e).__name__}: {e}")
        finally:
            S.busy = None
    S.misc.submit(task)


def _install(app_id: str, build_id: str | None = None) -> None:
    job = S.jobs[app_id]

    def progress(done, total):
        job.update(done=done, total=total)

    job["status"] = "downloading"
    try:
        library.install(app_id, on_progress=progress, log=S.say, build_id=build_id)
        job["status"] = "done"
    except (library.LibraryError, store.StoreError, OSError) as e:
        job.update(status="error", error=str(e))
        S.say(f"error: {e}")
    except Exception as e:
        job.update(status="error", error=f"{type(e).__name__}: {e}")
        S.say(f"error: {job['error']}")


def _check(app_id: str) -> None:
    try:
        library.classify(store.load_token(), app_id)
    except Exception as e:
        S.say(f"check {app_id} failed: {e}")


def _refresh() -> None:
    lib = library.refresh_library(store.load_token())
    S.say(f"library: {len(lib['apps'])} apps")
    _check_unknown()


def _check_unknown() -> None:
    for a in library.cached_library().get("apps", []):
        if not a.get("kind"):
            S.checks.submit(_check, a["id"])


def state() -> dict:
    lib = library.cached_library().get("apps", [])
    installed = {i: {**g, "log": str(Path(g["exe"]).parent / cli.log_name(Path(g["exe"])))}
                 for i, g in library.games().items()}
    return {
        "apps": lib,
        "installed": installed,
        "jobs": S.jobs,
        "log": S.log[-60:],
        "busy": S.busy,
        "steam_running": cli._steam_running(),
        "queued": [g["name"] for g in installed.values() if not g.get("in_steam")],
        "config": library.load_config(),
        "config_keys": library.CONFIG_KEYS,
        "pick": PICK,
        "logged_in": (store.config_dir() / "token").is_file(),
        "user": S.user,
        "has_git": (cli.REPO_ROOT / ".git").is_dir(),
        "has_desktop": (Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
                        / "applications/ovrl.desktop").is_file(),
    }


# Settings that hold a path, and whether Browse picks a folder or a file.
PICK = {"games_dir": "dir", "prefix": "dir", "steam_root": "dir", "organize_dir": "dir",
        "real_runtime": "file", "real_platform": "file", "openvr": "file"}


def pick_path(kind: str, start: str, title: str) -> str:
    """Open the desktop's native file dialog; "" if cancelled.

    A web page only ever sees file names, never real paths, so the dialog
    runs here, on the machine the paths belong to.
    """
    start = os.path.expanduser(start or "~")
    if kind == "dir" and not os.path.isdir(start):
        start = os.path.expanduser("~")
    if shutil.which("zenity"):
        cmd = ["zenity", "--file-selection", f"--title={title}"]
        if kind == "dir":
            cmd.append("--directory")
        cmd.append(f"--filename={start.rstrip('/')}/" if os.path.isdir(start) else f"--filename={start}")
    elif shutil.which("kdialog"):
        cmd = ["kdialog", "--title", title,
               "--getexistingdirectory" if kind == "dir" else "--getopenfilename", start]
    else:
        raise library.LibraryError("no file dialog found (install zenity); type the path instead")
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def _login(raw: str) -> None:
    token = store.clean_token(raw)
    problem = store.token_problem(token)
    if problem:
        raise library.LibraryError(f"that doesn't look like a user token: {problem}")
    user = store.current_user(token)    # checks it with Meta before saving
    store.save_token(token)
    S.user = user.get("alias") or user.get("display_name") or user.get("id")
    S.say(f"logged in as {S.user}")
    try:
        library.apply_identity(token, log=S.say)
    except library.LibraryError as e:
        S.say(f"identity not written: {e}")


def _shell(cmd: list[str]) -> None:
    S.say("$ " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=cli.REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    for line in proc.stdout:
        S.say(line.rstrip())
    if proc.wait():
        raise library.LibraryError(f"{cmd[0]} failed (exit {proc.returncode}); see the log")


def _update_from_github() -> None:
    """git pull + make, then push the new shim into every installed game."""
    if not (cli.REPO_ROOT / ".git").is_dir():
        raise library.LibraryError(f"{cli.REPO_ROOT} is not a git checkout")
    before = subprocess.run(["git", "rev-parse", "HEAD"], cwd=cli.REPO_ROOT,
                            capture_output=True, text=True).stdout.strip()
    _shell(["git", "pull", "--ff-only"])
    _shell(["make"])
    library.update_shims(log=S.say)
    after = subprocess.run(["git", "rev-parse", "HEAD"], cwd=cli.REPO_ROOT,
                           capture_output=True, text=True).stdout.strip()
    if before != after:
        S.say("updated; press Quit and reopen ovrl to load the new version of this page")


def desktop_entry() -> Path:
    """Add "Oculus PC games" to the desktop's app menu (no terminal needed)."""
    path = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") \
        / "applications" / "ovrl.desktop"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "[Desktop Entry]\nType=Application\nName=Oculus PC games (ovrl)\n"
        "Comment=Install your Oculus PC games and add them to Steam\n"
        f"Path={cli.REPO_ROOT / 'launcher'}\nExec=python3 -m ovrl gui\n"
        "Icon=applications-games\nTerminal=false\nCategories=Game;\n")
    return path


def _token_or_none() -> str | None:
    try:
        return store.load_token()
    except store.StoreError:
        return None


def _post(path: str, body: dict) -> dict:
    if path == "/api/open":
        game = library.games().get(str(body.get("app_id", "")))
        if not game:
            raise library.LibraryError("unknown game")
        exe_dir = Path(game["exe"]).parent
        target = exe_dir / cli.log_name(Path(game["exe"])) \
            if body.get("what") == "log" else exe_dir
        if not target.exists():
            raise library.LibraryError(f"no log yet at {target}: launch the game from "
                                       "Steam first (logs are written there each launch)")
        if not shutil.which("xdg-open"):
            raise library.LibraryError(f"open it yourself: {target}")
        subprocess.Popen(["xdg-open", str(target)], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
        return {"ok": True}
    if path == "/api/login":
        _login(str(body.get("token", "")))
        return {"ok": True}
    if path == "/api/update":
        _run("updating from GitHub", _update_from_github)
        return {"ok": True}
    if path == "/api/desktop":
        S.say(f"added to your app menu: {desktop_entry()}")
        return {"ok": True}
    if path == "/api/quit":
        S.say("quitting")
        threading.Thread(target=S.server.shutdown, daemon=True).start()
        return {"ok": True}
    if path == "/api/organize/scan":
        return {"found": library.scan_numbered(str(body.get("folder", "")), _token_or_none())}
    if path == "/api/organize/run":
        if cli._steam_running():
            raise library.LibraryError("quit Steam fully first (Steam > Exit), then try again")
        _run("organizing", library.organize, str(body.get("folder", "")),
             _token_or_none(), S.say)
        return {"ok": True}
    if path == "/api/get-runtime":
        _run("getting Meta's runtime", library.get_meta_runtime, None, True, S.say)
        return {"ok": True}
    if path == "/api/set-proton":
        if cli._steam_running():
            raise library.LibraryError("quit Steam fully first (Steam > Exit), then try again")
        _run("setting Proton", library.set_proton, None, S.say)
        return {"ok": True}
    if path == "/api/pick":
        key = str(body.get("key", ""))
        if key not in PICK:
            raise library.LibraryError("bad setting")
        return {"path": pick_path(PICK[key], str(body.get("start", "")),
                                  f"Choose {key.replace('_', ' ')}")}
    if path == "/api/versions":
        app_id = str(body.get("app_id", ""))
        if not app_id.isdigit():
            raise library.LibraryError("bad app id")
        return {"versions": library.versions(app_id)}
    if path == "/api/refresh":
        _run("refreshing library", _refresh)
    elif path == "/api/check":
        _check_unknown()
    elif path == "/api/install":
        app_id = str(body.get("app_id", ""))
        if not app_id.isdigit():
            raise library.LibraryError("bad app id")
        if S.jobs.get(app_id, {}).get("status") in ("queued", "downloading"):
            return {"ok": True}
        build_id = str(body.get("build_id") or "") or None
        if build_id and not build_id.isdigit():
            raise library.LibraryError("bad build id")
        S.jobs[app_id] = {"status": "queued", "done": 0, "total": 0}
        S.installs.submit(_install, app_id, build_id)
    elif path == "/api/add-to-steam":
        if cli._steam_running():
            raise library.LibraryError("quit Steam fully first (Steam > Exit), then try again")
        _run("adding to Steam", library.add_to_steam, None, S.say)
    elif path == "/api/update-shims":
        _run("updating shims", library.update_shims, S.say)
    elif path == "/api/config":
        library.save_config({k: body.get(k, "") for k in library.CONFIG_KEYS})
        S.say("settings saved")
    else:
        raise library.LibraryError("not found")
    return {"ok": True}


PAGE = r"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ovrl library</title><style>
:root{--bg:#f6f7f9;--card:#fff;--fg:#1d2330;--mute:#677086;--line:#dde1e8;--acc:#2f6fed;--ok:#1e8e4f;--warn:#b26b00;--bad:#c62f3a}
@media (prefers-color-scheme:dark){:root{--bg:#14171c;--card:#1d2128;--fg:#e6e9ef;--mute:#99a1b3;--line:#2d333d;--acc:#6b9bff;--ok:#4cc184;--warn:#e0a446;--bad:#ff6b76}}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}
main{max-width:980px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 12px} h2{font-size:15px;margin:0 0 8px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-bottom:14px}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
button{background:var(--acc);color:#fff;border:0;border-radius:6px;padding:6px 12px;font:inherit;cursor:pointer}
button.sec{background:transparent;color:var(--acc);border:1px solid var(--line)}
button:disabled{opacity:.45;cursor:default}
select{padding:5px 6px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit;max-width:320px}
input[type=text],input[type=password]{flex:1;min-width:160px;padding:6px 8px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit}
table{width:100%;border-collapse:collapse} td{padding:7px 6px;border-top:1px solid var(--line);vertical-align:middle}
.badge{font-size:12px;padding:1px 7px;border-radius:9px;border:1px solid currentColor;white-space:nowrap}
.pc{color:var(--ok)} .quest{color:var(--mute)} .other{color:var(--mute)} .unk{color:var(--warn)}
.mute{color:var(--mute);font-size:12px} .err{color:var(--bad)}
progress{width:140px} pre{max-height:220px;overflow:auto;font-size:12px;margin:0;white-space:pre-wrap}
.cfg{display:grid;grid-template-columns:150px 1fr;gap:6px 10px;align-items:center}
@media (max-width:600px){.cfg{grid-template-columns:1fr}}
</style></head><body><main>
<div class="bar" style="justify-content:space-between"><h1>Oculus PC games &rarr; Steam</h1>
<div class="bar"><button class="sec" id="desk" onclick="post('/api/desktop')">Add to app menu</button>
<button class="sec" onclick="if(confirm('Stop ovrl? Downloads in progress will stop (they resume next time).'))post('/api/quit').then(()=>document.body.innerHTML='<main><h1>ovrl stopped</h1><p>Open it again from your app menu.</p></main>')">Quit</button></div></div>
<div class="card" id="status"></div>
<div class="card" id="login" hidden><h2>Log in to your Meta account</h2>
<p class="mute">Paste your access token (it starts with FRL or OC). It is checked with Meta, then saved only on this computer, readable only by you. Treat it like a password: paste it here and nowhere else.
How to get it: see <a href="https://github.com/Breezyslasher/Oculus-PCVR-Linux#download-a-game-you-own-milestone-1" target="_blank" rel="noreferrer">step 1 in the README</a>.</p>
<div class="bar"><input type="password" id="tok" placeholder="Access token" autocomplete="off">
<button onclick="post('/api/login',{token:document.getElementById('tok').value}).then(j=>{if(j.ok){document.getElementById('tok').value='';post('/api/refresh')}})">Log in</button></div></div>
<div class="card"><div class="bar" style="margin-bottom:8px">
<input type="text" id="q" placeholder="Search your library">
<label><input type="checkbox" id="pconly" checked> PC games only</label>
<button class="sec" onclick="post('/api/refresh')">Refresh library</button>
<button class="sec" onclick="post('/api/check')">Re-check unknown</button></div>
<table id="apps"></table></div>
<div class="card"><h2>Installed games</h2><table id="games"></table>
<div class="bar" style="margin-top:10px">
<button id="addq" onclick="post('/api/add-to-steam')">Add queued games to Steam</button>
<button class="sec" id="upd" onclick="post('/api/update')">Get latest version &amp; update all games</button>
<button class="sec" onclick="post('/api/update-shims')">Re-copy shim into all games</button>
<button class="sec" onclick="post('/api/set-proton')">Set Proton for all games</button></div>
<p class="mute">“Get latest version” downloads the newest ovrl code from GitHub, rebuilds it, and copies the new shim into every installed game. Meta's genuine DLLs are left alone. “Set Proton for all games” sets every game added through ovrl to the Proton chosen in Steam's settings (or the proton_tool setting below); quit Steam first.</p></div>
<div class="card"><h2>Organize old downloads</h2>
<p class="mute">Finds game folders named by store number (like 1016967501697367), renames them to the game's name, moves them into your games folder and updates their Steam shortcuts. Quit Steam first.</p>
<div class="bar"><input type="text" id="c_organize_dir" placeholder="Folder to scan, e.g. ~/Games/VR Oculus">
<button class="sec" onclick="browse('organize_dir')">Browse…</button>
<button class="sec" onclick="scanOrg()">Scan</button>
<button id="orgrun" onclick="runOrg()" disabled>Rename &amp; move</button></div>
<table id="org" style="margin-top:8px"></table></div>
<div class="card"><h2>Meta runtime files</h2>
<p class="mute">Games check that Meta's genuine LibOVRRT64_1.dll (and for some, LibOVRPlatform64_1.dll) sits beside them. This downloads both straight from Meta with your login, as Meta's own PC installer does (only those two files), sets them in Settings and puts them beside your games. No Windows PC needed.</p>
<div class="bar"><button onclick="post('/api/get-runtime')">Get Meta runtime files</button></div></div>
<div class="card"><h2>Settings</h2><p class="mute">games_dir is where downloads go. Browse opens your desktop&rsquo;s file dialog; remember to Save.</p><div class="cfg" id="cfg"></div>
<div class="bar" style="margin-top:10px"><button onclick="saveCfg()">Save settings</button></div></div>
<div class="card"><h2>Log</h2><pre id="log"></pre></div>
</main><script>
const TOKEN="__TOKEN__";let st=null,cfgDrawn=false;
const esc=s=>String(s??"").replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
async function post(p,body){const r=await fetch(p,{method:"POST",headers:{"X-Ovrl-Token":TOKEN,"Content-Type":"application/json"},body:JSON.stringify(body||{})});
 const j=await r.json().catch(()=>({error:"bad reply"}));if(j.error)alert(j.error);poll();return j}
async function browse(k){const el=document.getElementById("c_"+k);const j=await post("/api/pick",{key:k,start:el.value});if(j.path)el.value=j.path}
function mb(n){return (n/1048576).toFixed(0)+" MB"}
function badge(a){if(!a.kind)return '<span class="badge unk">checking…</span>';
 if(a.kind=="pc")return '<span class="badge pc">PC</span>';
 return `<span class="badge ${a.kind}" title="${esc(a.reason)}">${a.kind=="quest"?"Quest":"not a game"}</span>`}
const vers={},sel={};
async function loadVers(id){const j=await post("/api/versions",{app_id:id});if(j.versions){vers[id]=j.versions;const d=j.versions.find(v=>v.default);if(d)sel[id]=d.id;draw()}}
function draw(){const s=st;
 document.getElementById("status").innerHTML=`<div class="bar">
 ${s.logged_in?(s.user?`<span>Logged in as <b>${esc(s.user)}</b> <button class="sec" onclick="document.getElementById('login').hidden=false">Change</button></span>`:`<span>Logged in <button class="sec" onclick="document.getElementById('login').hidden=false">Change</button></span>`):'<span class="err">Not logged in: paste your token below.</span>'}
 ${s.config.real_runtime?"":'<span class="err">Meta runtime files missing: press “Get Meta runtime files” below.</span>'}
 <span>Steam: ${s.steam_running?'<b class="unk">running</b> (games get queued; quit Steam to add them)':'<b class="pc">closed</b>'}</span>
 ${s.queued.length?`<span>Queued for Steam: ${esc(s.queued.join(", "))}</span>`:""}
 ${s.busy?`<span class="mute">${esc(s.busy)}…</span>`:""}</div>`;
 const q=document.getElementById("q").value.toLowerCase(),pc=document.getElementById("pconly").checked;
 let rows=s.apps.filter(a=>a.name.toLowerCase().includes(q)&&(!pc||a.kind=="pc"||!a.kind));
 const ae=document.activeElement;if(!(ae&&ae.tagName=="SELECT"&&ae.closest("#apps")))document.getElementById("apps").innerHTML=rows.length?rows.map(a=>{const j=s.jobs[a.id],g=s.installed[a.id];let act;
  if(j&&(j.status=="downloading"||j.status=="queued"))act=j.total?`<progress value="${j.done}" max="${j.total}"></progress> <span class="mute">${mb(j.done)} / ${mb(j.total)}</span>`:`<span class="mute">${j.status}…</span>`;
  else if(a.kind=="pc")act=(vers[a.id]?`<select onchange="sel['${a.id}']=this.value">${vers[a.id].map(v=>`<option value="${esc(v.id)}"${(sel[a.id]||"")==v.id||(!sel[a.id]&&v.default)?" selected":""}>v${esc(v.version)} · ${v.created?new Date(v.created*1000).toISOString().slice(0,10):"?"}${v.channels.length?" · "+esc(v.channels.join(", ")):""}${g&&g.build_id==v.id?" (installed)":""}</option>`).join("")}</select> `:`<button class="sec" onclick="loadVers('${a.id}')">Versions…</button> `)+`<button onclick="post('/api/install',{app_id:'${a.id}',build_id:sel['${a.id}']||''})">${g?"Reinstall":"Install"}</button>`;
  else act="";
  if(j&&j.status=="error")act+=` <span class="err">${esc(j.error)}</span>`;
  return `<tr><td>${esc(a.name)}<div class="mute">${a.id}${a.version?" · newest v"+esc(a.version):""}${g&&g.version?" · installed v"+esc(g.version):""}</div></td><td>${badge(a)}</td><td style="text-align:right">${act}</td></tr>`}).join("")
  :`<tr><td class="mute">${s.apps.length?"No matches.":"Press “Refresh library” to load the games you own."}</td></tr>`;
 const gs=Object.values(s.installed);
 document.getElementById("games").innerHTML=gs.length?gs.map(g=>`<tr><td>${esc(g.name)}<div class="mute">${esc(g.engine)} · ${esc(g.exe)}</div><div class="mute">Log: ${esc(g.log)}</div></td><td>${g.in_steam?'<span class="badge pc">in Steam</span>':'<span class="badge unk">queued</span>'}</td><td style="text-align:right;white-space:nowrap"><button class="sec" onclick="post('/api/open',{app_id:'${esc(g.app_id)}',what:'log'})">Open log</button> <button class="sec" onclick="post('/api/open',{app_id:'${esc(g.app_id)}',what:'folder'})">Open folder</button></td></tr>`).join(""):'<tr><td class="mute">None yet.</td></tr>';
 document.getElementById("addq").disabled=!s.queued.length||s.steam_running;
 if(!s.logged_in)document.getElementById("login").hidden=false;
 document.getElementById("upd").disabled=!s.has_git||!!s.busy;
 document.getElementById("desk").hidden=s.has_desktop;
 document.getElementById("log").textContent=s.log.join("\n");
 if(!cfgDrawn){cfgDrawn=true;document.getElementById("cfg").innerHTML=Object.entries(s.config_keys).map(([k,h])=>
  `<label for="c_${k}">${k}</label><div><div class="bar"><input type="text" id="c_${k}" value="${esc(s.config[k])}">${s.pick[k]?`<button class="sec" onclick="browse('${k}')">Browse…</button>`:""}</div><div class="mute">${esc(h)}</div></div>`).join("")}}
async function scanOrg(){const f=document.getElementById("c_organize_dir").value;const j=await post("/api/organize/scan",{folder:f});
 const rows=(j.found||[]);document.getElementById("orgrun").disabled=!rows.some(r=>!r.problem);
 document.getElementById("org").innerHTML=rows.length?rows.map(r=>`<tr><td>${esc(r.name||"?")}<div class="mute">${esc(r.path)}</div></td><td>${r.problem?`<span class="err">${esc(r.problem)}</span>`:`<span class="mute">→ ${esc(r.target)}</span>`}</td></tr>`).join(""):(j.found?'<tr><td class="mute">No folders named by store number here.</td></tr>':"")}
async function runOrg(){if(!confirm("Move these games? Steam must be closed."))return;const f=document.getElementById("c_organize_dir").value;await post("/api/organize/run",{folder:f});document.getElementById("orgrun").disabled=true;setTimeout(scanOrg,3000)}
function saveCfg(){const b={};for(const k in st.config_keys)b[k]=document.getElementById("c_"+k).value;post("/api/config",b)}
async function poll(){try{st=await (await fetch("/api/state")).json();draw()}catch(e){}}
document.getElementById("q").oninput=draw;document.getElementById("pconly").onchange=draw;
poll();setInterval(poll,1000);
</script></body></html>"""


def serve(port: int = 8765, open_browser: bool = True) -> None:
    token = secrets.token_urlsafe(24)
    page = PAGE.replace("__TOKEN__", token).encode()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body: bytes, ctype="application/json"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _host_ok(self) -> bool:
            # Refuse other hostnames (DNS rebinding).
            host = (self.headers.get("Host") or "").rsplit(":", 1)[0]
            return host in ("127.0.0.1", "localhost")

        def do_GET(self):
            if not self._host_ok():
                return self._send(403, b"{}")
            if self.path == "/":
                self._send(200, page, "text/html; charset=utf-8")
            elif self.path == "/api/state":
                self._send(200, json.dumps(state()).encode())
            else:
                self._send(404, b"{}")

        def do_POST(self):
            if not self._host_ok() or self.headers.get("X-Ovrl-Token") != token:
                return self._send(403, b'{"error":"forbidden"}')
            try:
                n = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(n) or b"{}")
                self._send(200, json.dumps(_post(self.path, body)).encode())
            except (library.LibraryError, store.StoreError, ValueError) as e:
                self._send(200, json.dumps({"error": str(e)}).encode())

    url = f"http://127.0.0.1:{port}/"
    try:
        server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    except OSError:
        # Already running (e.g. opened twice from the app menu): show that one.
        print(f"ovrl UI already running at {url}")
        if open_browser:
            webbrowser.open(url)
        return
    S.server = server
    print(f"ovrl UI at {url}  (Ctrl+C to stop)")
    if library.cached_library().get("apps"):
        _check_unknown()
    if open_browser:
        threading.Timer(0.5, webbrowser.open, (url,)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        server.server_close()
