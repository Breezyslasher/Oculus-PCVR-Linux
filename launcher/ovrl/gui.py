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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import cli, library, store


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.jobs: dict[str, dict] = {}     # app_id -> install job
        self.log: list[str] = []
        self.busy: str | None = None        # what the single worker is doing
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


def _install(app_id: str) -> None:
    job = S.jobs[app_id]

    def progress(done, total):
        job.update(done=done, total=total)

    job["status"] = "downloading"
    try:
        library.install(app_id, on_progress=progress, log=S.say)
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
    installed = library.games()
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
    }


# Settings that hold a path, and whether Browse picks a folder or a file.
PICK = {"games_dir": "dir", "prefix": "dir", "steam_root": "dir",
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


def _post(path: str, body: dict) -> dict:
    if path == "/api/pick":
        key = str(body.get("key", ""))
        if key not in PICK:
            raise library.LibraryError("bad setting")
        return {"path": pick_path(PICK[key], str(body.get("start", "")),
                                  f"Choose {key.replace('_', ' ')}")}
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
        S.jobs[app_id] = {"status": "queued", "done": 0, "total": 0}
        S.installs.submit(_install, app_id)
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
input[type=text]{flex:1;min-width:160px;padding:6px 8px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit}
table{width:100%;border-collapse:collapse} td{padding:7px 6px;border-top:1px solid var(--line);vertical-align:middle}
.badge{font-size:12px;padding:1px 7px;border-radius:9px;border:1px solid currentColor;white-space:nowrap}
.pc{color:var(--ok)} .quest{color:var(--mute)} .other{color:var(--mute)} .unk{color:var(--warn)}
.mute{color:var(--mute);font-size:12px} .err{color:var(--bad)}
progress{width:140px} pre{max-height:220px;overflow:auto;font-size:12px;margin:0;white-space:pre-wrap}
.cfg{display:grid;grid-template-columns:150px 1fr;gap:6px 10px;align-items:center}
@media (max-width:600px){.cfg{grid-template-columns:1fr}}
</style></head><body><main>
<h1>Oculus PC games &rarr; Steam</h1>
<div class="card" id="status"></div>
<div class="card"><div class="bar" style="margin-bottom:8px">
<input type="text" id="q" placeholder="Search your library">
<label><input type="checkbox" id="pconly" checked> PC games only</label>
<button class="sec" onclick="post('/api/refresh')">Refresh library</button>
<button class="sec" onclick="post('/api/check')">Re-check unknown</button></div>
<table id="apps"></table></div>
<div class="card"><h2>Installed games</h2><table id="games"></table>
<div class="bar" style="margin-top:10px">
<button id="addq" onclick="post('/api/add-to-steam')">Add queued games to Steam</button>
<button class="sec" onclick="post('/api/update-shims')">Update shim in all games</button></div>
<p class="mute">Update copies the newly built shim (rebuild first with <code>git pull &amp;&amp; make</code>) into every installed game. Meta's genuine DLLs are left alone.</p></div>
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
function draw(){const s=st;
 document.getElementById("status").innerHTML=`<div class="bar">
 ${s.logged_in?"":'<span class="err">Not logged in: run <code>python3 -m ovrl login</code></span>'}
 ${s.config.real_runtime?"":'<span class="err">Set the genuine LibOVRRT64_1.dll path in Settings below.</span>'}
 <span>Steam: ${s.steam_running?'<b class="unk">running</b> (games get queued; quit Steam to add them)':'<b class="pc">closed</b>'}</span>
 ${s.queued.length?`<span>Queued for Steam: ${esc(s.queued.join(", "))}</span>`:""}
 ${s.busy?`<span class="mute">${esc(s.busy)}…</span>`:""}</div>`;
 const q=document.getElementById("q").value.toLowerCase(),pc=document.getElementById("pconly").checked;
 let rows=s.apps.filter(a=>a.name.toLowerCase().includes(q)&&(!pc||a.kind=="pc"||!a.kind));
 document.getElementById("apps").innerHTML=rows.length?rows.map(a=>{const j=s.jobs[a.id],g=s.installed[a.id];let act;
  if(j&&(j.status=="downloading"||j.status=="queued"))act=j.total?`<progress value="${j.done}" max="${j.total}"></progress> <span class="mute">${mb(j.done)} / ${mb(j.total)}</span>`:`<span class="mute">${j.status}…</span>`;
  else if(a.kind=="pc")act=`<button onclick="post('/api/install',{app_id:'${a.id}'})">${g?"Reinstall":"Install"}</button>`;
  else act="";
  if(j&&j.status=="error")act+=` <span class="err">${esc(j.error)}</span>`;
  return `<tr><td>${esc(a.name)}<div class="mute">${a.id}${a.version?" · v"+esc(a.version):""}</div></td><td>${badge(a)}</td><td style="text-align:right">${act}</td></tr>`}).join("")
  :`<tr><td class="mute">${s.apps.length?"No matches.":"Press “Refresh library” to load the games you own."}</td></tr>`;
 const gs=Object.values(s.installed);
 document.getElementById("games").innerHTML=gs.length?gs.map(g=>`<tr><td>${esc(g.name)}<div class="mute">${esc(g.engine)} · ${esc(g.exe)}</div></td><td>${g.in_steam?'<span class="badge pc">in Steam</span>':'<span class="badge unk">queued</span>'}</td></tr>`).join(""):'<tr><td class="mute">None yet.</td></tr>';
 document.getElementById("addq").disabled=!s.queued.length||s.steam_running;
 document.getElementById("log").textContent=s.log.join("\n");
 if(!cfgDrawn){cfgDrawn=true;document.getElementById("cfg").innerHTML=Object.entries(s.config_keys).map(([k,h])=>
  `<label for="c_${k}">${k}</label><div><div class="bar"><input type="text" id="c_${k}" value="${esc(s.config[k])}">${s.pick[k]?`<button class="sec" onclick="browse('${k}')">Browse…</button>`:""}</div><div class="mute">${esc(h)}</div></div>`).join("")}}
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

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    url = f"http://127.0.0.1:{port}/"
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
