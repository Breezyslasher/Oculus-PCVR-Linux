"""Tests for ovrl's store and download code against a local fake store/CDN.

Run from launcher/:  python3 -m unittest discover -s tests
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import tempfile
import threading
import unittest
import urllib.parse
import zipfile
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from ovrl import cli, download, store

TOKEN = "FRLtesttoken123"
BUILD = "9001"
SEG = 1000


def _segments(data: bytes) -> list[bytes]:
    return [data[i:i + SEG] for i in range(0, len(data), SEG)] or [b""]


class FakeMeta:
    """In-memory store: one user, one owned Rift app, one build."""

    def __init__(self, files: dict[str, bytes]):
        self.files = files
        self.blobs: dict[str, bytes] = {}
        self.corrupt_once: set[str] = set()
        self.segment_requests = 0
        manifest_files = {}
        for name, data in files.items():
            segs = []
            for i, chunk in enumerate(_segments(data)):
                sha = hashlib.sha256(chunk).hexdigest()
                self.blobs[sha] = chunk
                segs.append([i * SEG, sha, len(chunk)])
            manifest_files[name] = {"sha256": hashlib.sha256(data).hexdigest(),
                                    "size": len(data), "segmentSize": SEG,
                                    "segments": segs}
        self.manifest = {"canonicalName": "test-game", "launchFile": "Game.exe",
                         "launchParameters": "", "files": manifest_files}

    def graphql(self, doc_id: str, variables: dict) -> dict:
        if doc_id == store.DOC_CURRENT_USER:
            return {"data": {"viewer": {"user": {"id": "1", "alias": "tester"}}}}
        if doc_id == store.DOC_ENTITLEMENTS:
            return {"data": {"viewer": {"user": {"active_entitlements": {"nodes": [
                {"active_state": "ACTIVE", "item": {"id": "111", "display_name": "Test Game"}},
            ]}}}}}
        if doc_id == store.DOC_APP_VERSIONS and variables.get("applicationID") == "111":
            return {"data": {"node": {"display_name": "Test Game", "primary_binaries": {"nodes": [
                {"id": "8000", "version": "1.0", "version_code": 1, "created_date": 0,
                 "binary_release_channels": {"nodes": []}},
                {"id": BUILD, "version": "1.1", "version_code": 2, "created_date": 1700000000,
                 "binary_release_channels": {"nodes": [{"channel_name": "LIVE"}]}},
            ]}}}}
        return {"data": {"node": None}}

    def handler(self):
        meta = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, code: int, body: bytes, ctype="application/octet-stream"):
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                url = urllib.parse.urlparse(self.path)
                q = dict(urllib.parse.parse_qsl(url.query))
                if q.get("access_token") != TOKEN:
                    err = {"error": {"type": "OAuthException", "message": "bad token"}}
                    return self._send(400, json.dumps(err).encode(), "application/json")
                body = meta.graphql(q["doc_id"], json.loads(q["variables"]))
                self._send(200, json.dumps(body).encode(), "application/json")

            def do_GET(self):
                url = urllib.parse.urlparse(self.path)
                q = dict(urllib.parse.parse_qsl(url.query))
                if q.get("access_token") != TOKEN:
                    return self._send(403, b"forbidden")
                if url.path == "/binaries/download/" and q.get("get_manifest") == "1":
                    buf = io.BytesIO()
                    with zipfile.ZipFile(buf, "w") as z:
                        z.writestr("manifest.json", json.dumps(meta.manifest))
                    return self._send(200, buf.getvalue())
                if url.path == "/binaries/segment/":
                    meta.segment_requests += 1
                    sha = q["segment_sha256"]
                    data = meta.blobs[sha]
                    if sha in meta.corrupt_once:
                        meta.corrupt_once.discard(sha)
                        data = data + b"garbage"
                    return self._send(200, zlib.compress(data))
                self._send(404, b"")

        return Handler


class StoreTest(unittest.TestCase):
    def setUp(self):
        payload = os.urandom(3500)
        self.files = {"Game.exe": payload, "Data/level0.bin": os.urandom(1200),
                      "Data/empty.txt": b""}
        self.meta = FakeMeta(self.files)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), self.meta.handler())
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        base = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.patches = [(store, "GRAPH_URL", base + "/graphql"),
                        (download, "CDN_URL", base)]
        self.saved = [(m, n, getattr(m, n)) for m, n, _ in self.patches]
        for m, n, v in self.patches:
            setattr(m, n, v)
        self.tmp = tempfile.TemporaryDirectory()
        self.env = {"XDG_CONFIG_HOME": self.tmp.name, "OVRL_TOKEN": ""}
        self.old_env = {k: os.environ.get(k) for k in self.env}
        os.environ.update(self.env)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        for m, n, v in self.saved:
            setattr(m, n, v)
        for k, v in self.old_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.tmp.cleanup()

    def run_cli(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            rc = cli.main(list(argv))
        return rc, out.getvalue()

    def test_token_checks(self):
        self.assertIsNone(store.token_problem(TOKEN))
        self.assertIsNotNone(store.token_problem("abc"))
        self.assertIsNotNone(store.token_problem("OC|123|abc"))
        self.assertIsNotNone(store.token_problem("FRL%7Cabc"))

    def test_login_saves_token_privately(self):
        rc, out = self.run_cli("login", "--token", TOKEN)
        self.assertEqual(rc, 0)
        self.assertIn("tester", out)
        path = Path(self.tmp.name) / "ovrl" / "token"
        self.assertEqual(path.read_text().strip(), TOKEN)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_login_rejects_bad_token(self):
        rc, _ = self.run_cli("login", "--token", "FRLwrong")
        self.assertEqual(rc, 1)
        self.assertFalse((Path(self.tmp.name) / "ovrl" / "token").exists())

    def test_library_and_versions(self):
        store.save_token(TOKEN)
        rc, out = self.run_cli("library")
        self.assertEqual(rc, 0)
        self.assertIn("111  Test Game", out)
        rc, out = self.run_cli("versions", "111")
        self.assertEqual(rc, 0)
        self.assertLess(out.index(f"build {BUILD}"), out.index("build 8000"))
        self.assertIn("[LIVE]", out)

    def test_download_picks_live_build_and_resumes(self):
        store.save_token(TOKEN)
        dest = Path(self.tmp.name) / "game"
        rc, out = self.run_cli("download", "111", "--dest", str(dest), "--jobs", "2")
        self.assertEqual(rc, 0, out)
        for name, data in self.files.items():
            self.assertEqual((dest / name).read_bytes(), data)
        self.assertIn("Game.exe", out)
        self.assertTrue((dest / "manifest.json").is_file())
        self.assertFalse(list(dest.rglob("*.part")))

        # Re-running downloads nothing; damaging one file re-fetches only it.
        before = self.meta.segment_requests
        self.run_cli("download", "111", "--dest", str(dest))
        self.assertEqual(self.meta.segment_requests, before)
        (dest / "Data/level0.bin").write_bytes(b"broken")
        rc, out = self.run_cli("download", "111", "--dest", str(dest))
        self.assertEqual(rc, 0)
        self.assertIn("2 of 3 files already present", out)
        self.assertEqual(self.meta.segment_requests, before + 2)
        self.assertEqual((dest / "Data/level0.bin").read_bytes(), self.files["Data/level0.bin"])

    def test_corrupt_segment_is_retried(self):
        first = self.meta.manifest["files"]["Game.exe"]["segments"][1][1]
        self.meta.corrupt_once.add(first)
        dest = Path(self.tmp.name) / "game"
        download.download_build(TOKEN, BUILD, dest, workers=2)
        self.assertEqual((dest / "Game.exe").read_bytes(), self.files["Game.exe"])

    def test_bad_token_on_cdn(self):
        with self.assertRaisesRegex(store.StoreError, "refused"):
            download.download_build("FRLwrong", BUILD, Path(self.tmp.name) / "g")

    def test_manifest_path_traversal_refused(self):
        self.meta.manifest["files"]["../evil.txt"] = self.meta.manifest["files"]["Game.exe"]
        with self.assertRaisesRegex(store.StoreError, "unsafe path"):
            download.download_build(TOKEN, BUILD, Path(self.tmp.name) / "g")
        self.assertFalse((Path(self.tmp.name) / "evil.txt").exists())


if __name__ == "__main__":
    unittest.main()
