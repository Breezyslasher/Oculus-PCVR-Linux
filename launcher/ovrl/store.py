"""Meta (Oculus) store API: account, owned apps and Rift build listing.

The endpoints and persisted query IDs follow ComputerElite's
OculusGraphQLApiLib (used by Oculus Downgrader), which was maintained as of
2026. Meta can change or remove any of them without notice; when that
happens, the doc IDs below are the first thing to check.

All requests use the user's own access token (see `ovrl login`). The token is
sent only to Meta's servers and is never logged.
"""

from __future__ import annotations

import json
import os
import re
import stat
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

GRAPH_URL = os.environ.get("OVRL_GRAPH_URL", "https://graph.oculus.com/graphql")
USER_AGENT = "ovrl/0.1"
TIMEOUT = 60

# Persisted GraphQL queries (doc IDs) from OculusGraphQLApiLib.
DOC_CURRENT_USER = "4149322231793299"
DOC_ENTITLEMENTS = "4850747515044496"
DOC_APP_VERSIONS = "2885322071572384"


class StoreError(RuntimeError):
    pass


def config_dir() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "ovrl"


def clean_token(raw: str) -> str:
    """Strip what tends to come along when copying a token from dev tools:
    a "name: value" or "name=value" label, an "OAuth"/"Bearer" scheme, quotes."""
    def unwrap(t: str) -> str:
        t = t.strip().strip('"\'').strip()
        for scheme in ("oauth ", "bearer "):
            if t.lower().startswith(scheme):
                t = t[len(scheme):].strip()
        return t

    token = unwrap(raw)
    for sep in (":", "="):
        _, found, tail = token.partition(sep)
        if found and unwrap(tail).startswith(("FRL", "OC")):
            token = unwrap(tail)
    return token


def token_problem(token: str) -> str | None:
    """Return why a pasted token looks wrong, or None if it looks usable."""
    if "%" in token:
        return "it looks URL-encoded; copy it from the request payload, not the URL"
    if not (token.startswith("FRL") or token.startswith("OC")):
        return "user tokens start with 'FRL' or 'OC'"
    if "|" in token:
        return "it contains '|', which marks an app token, not your user token"
    if re.fullmatch(r"OC\d{15}.*", token):
        return "it looks edited; copy the token exactly as it appears"
    return None


def save_token(token: str) -> Path:
    path = config_dir() / "token"
    path.parent.mkdir(parents=True, exist_ok=True)
    # Create with owner-only permissions before writing the secret.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, stat.S_IRUSR | stat.S_IWUSR)
    with os.fdopen(fd, "w") as f:
        f.write(token.strip() + "\n")
    return path


def load_token() -> str:
    token = os.environ.get("OVRL_TOKEN", "").strip()
    if token:
        return token
    path = config_dir() / "token"
    if path.is_file():
        return path.read_text().strip()
    raise StoreError("no access token; run `ovrl login` first (or set OVRL_TOKEN)")


def graphql(token: str, doc_id: str, variables: dict) -> dict:
    query = urllib.parse.urlencode({
        "access_token": token,
        "variables": json.dumps(variables),
        "doc_id": doc_id,
    })
    req = urllib.request.Request(f"{GRAPH_URL}?{query}", data=b"", method="POST",
                                 headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            body = json.load(resp)
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:300]
        if "OAuthException" in detail:
            raise StoreError("Meta rejected the access token (expired or invalid); "
                             "run `ovrl login` again") from None
        raise StoreError(f"store request failed: HTTP {e.code}: {detail}") from None
    except urllib.error.URLError as e:
        raise StoreError(f"could not reach the store: {e.reason}") from None
    if body.get("errors") and not body.get("data"):
        raise StoreError(f"store returned errors: {body['errors']}")
    return body.get("data") or {}


def current_user(token: str) -> dict:
    user = (graphql(token, DOC_CURRENT_USER, {}).get("viewer") or {}).get("user")
    if not user:
        raise StoreError("token did not resolve to a user (expired or invalid?)")
    return user


@dataclass
class OwnedApp:
    id: str
    name: str
    state: str


def entitlements(token: str) -> list[OwnedApp]:
    user = (graphql(token, DOC_ENTITLEMENTS, {}).get("viewer") or {}).get("user") or {}
    nodes = (user.get("active_entitlements") or {}).get("nodes") or []
    apps = []
    for n in nodes:
        item = n.get("item") or {}
        if item.get("id"):
            apps.append(OwnedApp(item["id"], item.get("display_name", ""),
                                 n.get("active_state", "")))
    return sorted(apps, key=lambda a: a.name.lower())


@dataclass
class Build:
    id: str
    version: str
    version_code: int
    created: int
    channels: list[str]


def app_builds(token: str, app_id: str) -> tuple[str, list[Build]]:
    """Return (app name, builds sorted newest first) for a Rift app ID."""
    node = graphql(token, DOC_APP_VERSIONS, {"applicationID": app_id}).get("node")
    if not node:
        raise StoreError(f"no app with ID {app_id} (or it isn't visible to this account)")
    builds = []
    for b in (node.get("primary_binaries") or {}).get("nodes") or []:
        channels = [c.get("channel_name", "")
                    for c in ((b.get("binary_release_channels") or {}).get("nodes") or [])]
        builds.append(Build(b["id"], b.get("version", ""), int(b.get("version_code") or 0),
                            int(b.get("created_date") or 0), channels))
    builds.sort(key=lambda b: b.version_code, reverse=True)
    return node.get("display_name", ""), builds
