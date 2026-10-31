"""Download a Rift build from Meta's CDN, with resume and hash verification.

Protocol (as used by OculusGraphQLApiLib's GameDownloader):
  1. GET {CDN}/binaries/download/?id=<build>&access_token=..&get_manifest=1
     returns a zip holding a JSON manifest: launch file plus, per file, its
     size, SHA-256 and an ordered list of segments [_, segment_sha256, ...].
  2. GET {CDN}/binaries/segment/?access_token=..&binary_id=<build>&segment_sha256=..
     returns one zlib-compressed segment; the SHA-256 is of the decompressed
     bytes. A file is its segments concatenated in manifest order.

Files that already exist with the right SHA-256 are skipped, so re-running
the same command resumes an interrupted download.
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib
from collections import deque
from concurrent.futures import Future, ThreadPoolExecutor
from pathlib import Path, PurePosixPath

from .store import TIMEOUT, USER_AGENT, StoreError

CDN_URL = os.environ.get("OVRL_CDN_URL", "https://securecdn.oculus.com")
RETRIES = 5
CHUNK = 1 << 20


def _get(url: str) -> bytes:
    """GET with retries. Never include the URL in errors: it holds the token."""
    last = ""
    for attempt in range(RETRIES):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                return resp.read()
        except urllib.error.HTTPError as e:
            last = f"HTTP {e.code}"
            if e.code in (401, 403):
                raise StoreError(f"CDN refused the request ({last}); the token may be "
                                 "expired, or this account may not own the build") from None
            if 400 <= e.code < 500 and e.code != 429:
                break
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            last = str(getattr(e, "reason", e))
        time.sleep(min(2 ** attempt, 16))
    raise StoreError(f"download failed after {RETRIES} attempts: {last}")


def fetch_manifest(token: str, build_id: str) -> dict:
    q = urllib.parse.urlencode({"id": build_id, "access_token": token, "get_manifest": 1})
    data = _get(f"{CDN_URL}/binaries/download/?{q}")
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            name = next(n for n in z.namelist() if n.endswith(".json"))
            return json.loads(z.read(name))
    except (zipfile.BadZipFile, StopIteration, json.JSONDecodeError):
        raise StoreError("CDN did not return a valid manifest zip "
                         "(is this a Rift build ID?)") from None


def _fetch_segment(token: str, build_id: str, sha256: str) -> bytes:
    q = urllib.parse.urlencode({"access_token": token, "binary_id": build_id,
                                "segment_sha256": sha256})
    for _ in range(RETRIES):
        raw = _get(f"{CDN_URL}/binaries/segment/?{q}")
        try:
            data = zlib.decompress(raw)
        except zlib.error:
            continue
        if hashlib.sha256(data).hexdigest() == sha256.lower():
            return data
    raise StoreError(f"segment {sha256[:12]}… failed verification {RETRIES} times")


def _file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(CHUNK):
            h.update(chunk)
    return h.hexdigest()


def _safe_path(dest: Path, name: str) -> Path:
    """Map a manifest path into dest, refusing anything that escapes it."""
    rel = PurePosixPath(name.replace("\\", "/"))
    if rel.is_absolute() or ".." in rel.parts:
        raise StoreError(f"manifest contains an unsafe path: {name!r}")
    return dest.joinpath(*rel.parts)


def _fmt(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} GiB"


class _Progress:
    def __init__(self, total: int, on_progress=None):
        self.total, self.done, self.start, self.last = total, 0, time.monotonic(), 0.0
        self.on_progress = on_progress

    def add(self, n: int) -> None:
        self.done += n
        if self.on_progress:
            self.on_progress(self.done, self.total)
        now = time.monotonic()
        if now - self.last >= 0.5 or self.done >= self.total:
            self.last = now
            rate = self.done / max(now - self.start, 1e-6)
            pct = 100 * self.done / self.total if self.total else 100
            sys.stderr.write(f"\r  {pct:5.1f}%  {_fmt(self.done)} / {_fmt(self.total)}"
                             f"  {_fmt(rate)}/s   ")
            sys.stderr.flush()


def download_build(token: str, build_id: str, dest: Path, workers: int = 8,
                   on_progress=None) -> dict:
    """Download every file of a Rift build into dest. Returns the manifest.

    on_progress(done_bytes, total_bytes), if given, is called as data arrives
    (from the calling thread)."""
    dest.mkdir(parents=True, exist_ok=True)
    manifest = fetch_manifest(token, build_id)
    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2))

    files = manifest.get("files") or {}
    if not files:
        raise StoreError("manifest lists no files")
    todo = []
    for name, info in files.items():
        path = _safe_path(dest, name)
        if path.is_file() and path.stat().st_size == info.get("size") \
                and _file_sha256(path) == str(info.get("sha256", "")).lower():
            continue
        todo.append((name, path, info))

    total = sum(int(info.get("size", 0)) for _, _, info in todo)
    print(f"{len(files) - len(todo)} of {len(files)} files already present; "
          f"downloading {len(todo)} ({_fmt(total)})")
    progress = _Progress(total, on_progress)
    if on_progress:
        on_progress(0, total)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        for name, path, info in todo:
            path.parent.mkdir(parents=True, exist_ok=True)
            # Truncate now so files with no segments still get created.
            open(path.with_name(path.name + ".part"), "wb").close()

        # One window of segment requests spans file boundaries, so games made
        # of many small files still keep every worker busy. Segments come
        # back in submission order and are appended to their file's .part.
        jobs = ((i, str(s[1])) for i, (_, _, info) in enumerate(todo)
                for s in info.get("segments") or [])
        remaining = [len(info.get("segments") or []) for _, _, info in todo]
        window: deque[tuple[int, Future]] = deque()

        def submit_next() -> None:
            job = next(jobs, None)
            if job is not None:
                window.append((job[0], pool.submit(_fetch_segment, token, build_id, job[1])))

        for _ in range(workers * 2):
            submit_next()
        for i, (name, path, info) in enumerate(todo):
            part = path.with_name(path.name + ".part")
            with open(part, "ab") as out:
                while remaining[i]:
                    idx, fut = window.popleft()
                    data = fut.result()
                    submit_next()
                    out.write(data)
                    progress.add(len(data))
                    remaining[idx] -= 1
            if _file_sha256(part) != str(info.get("sha256", "")).lower():
                part.unlink()
                raise StoreError(f"{name}: assembled file failed its SHA-256 check")
            part.replace(path)

    if todo:
        sys.stderr.write("\n")
    return manifest
