"""Read and write Steam's config so a game can be added as a non-Steam game.

Steam keeps non-Steam shortcuts in a binary KeyValues file
(``userdata/<id>/config/shortcuts.vdf``) and the per-game Proton choice in a
text KeyValues file (``config/config.vdf``). This module parses both, edits
them in place and writes them back, which is what Heroic does under the hood.

Steam rewrites these files when it exits, so it must be closed while we edit.
Both writers round-trip the original bytes/text first and refuse to touch a
file they cannot reproduce, so a parser gap can never corrupt a user's
shortcuts.
"""

from __future__ import annotations

import zlib
from pathlib import Path

# Binary KeyValues type tags.
_MAP = 0x00
_STR = 0x01
_INT = 0x02
_END = 0x08


def shortcut_appid(exe: str, app_name: str) -> int:
    """The 32-bit id Steam uses to key a non-Steam shortcut's settings.

    Computed like Steam/Heroic do: CRC-32 of the (quoted) exe string followed
    by the app name, with the high bit set. We write the same value into the
    shortcut and the compat-tool mapping, so Steam always matches the two.
    """
    data = (exe + app_name).encode("utf-8")
    return (zlib.crc32(data) | 0x80000000) & 0xFFFFFFFF


# ---- binary shortcuts.vdf ------------------------------------------------
#
# A map is a list of [type, key, value] triples; value is a nested list for a
# map, str for a string, int for an int32. Keeping the triples in order (and
# ints distinct from strings) lets us re-emit the original byte-for-byte.


def _read_cstr(buf: bytes, i: int) -> tuple[str, int]:
    end = buf.index(0, i)
    return buf[i:end].decode("utf-8"), end + 1


def _read_map(buf: bytes, i: int) -> tuple[list, int]:
    items: list = []
    while True:
        if i >= len(buf):          # tolerate a missing final terminator
            return items, i
        tag = buf[i]
        i += 1
        if tag == _END:
            return items, i
        key, i = _read_cstr(buf, i)
        if tag == _MAP:
            val, i = _read_map(buf, i)
        elif tag == _STR:
            val, i = _read_cstr(buf, i)
        elif tag == _INT:
            val = int.from_bytes(buf[i:i + 4], "little", signed=True)
            i += 4
        else:
            raise ValueError(f"unknown binary VDF tag {tag:#x} at {i - 1}")
        items.append([tag, key, val])


def _write_map(items: list) -> bytes:
    out = bytearray()
    for tag, key, val in items:
        out.append(tag)
        out += key.encode("utf-8") + b"\x00"
        if tag == _MAP:
            out += _write_map(val)
        elif tag == _STR:
            out += val.encode("utf-8") + b"\x00"
        elif tag == _INT:
            out += int(val).to_bytes(4, "little", signed=True)
        else:
            raise ValueError(f"unknown binary VDF tag {tag:#x}")
    out.append(_END)
    return bytes(out)


def parse_shortcuts(data: bytes) -> list:
    """Parse shortcuts.vdf bytes into the top-level map's triples."""
    items, _ = _read_map(data, 0)
    return items


def dump_shortcuts(items: list) -> bytes:
    return _write_map(items)


def _find(items: list, key: str):
    """The value of the (case-insensitive) key in a map, or None."""
    for _, k, v in items:
        if k.lower() == key.lower():
            return v
    return None


def shortcuts_map(top: list) -> list:
    """The inner "shortcuts" map (list of numbered entry triples)."""
    inner = _find(top, "shortcuts")
    if inner is None:
        raise ValueError("no 'shortcuts' map in file")
    return inner


def make_entry(appid: int, name: str, exe: str, start_dir: str,
               launch_options: str, openvr: bool) -> list:
    """Build one shortcut entry (the triples of a numbered map)."""
    return [
        [_INT, "appid", appid - (1 << 32) if appid >> 31 else appid],
        [_STR, "AppName", name],
        [_STR, "Exe", exe],
        [_STR, "StartDir", start_dir],
        [_STR, "icon", ""],
        [_STR, "ShortcutPath", ""],
        [_STR, "LaunchOptions", launch_options],
        [_INT, "IsHidden", 0],
        [_INT, "AllowDesktopConfig", 1],
        [_INT, "AllowOverlay", 1],
        [_INT, "OpenVR", 1 if openvr else 0],
        [_INT, "Devkit", 0],
        [_STR, "DevkitGameID", ""],
        [_INT, "DevkitOverrideAppID", 0],
        [_INT, "LastPlayTime", 0],
        [_STR, "FlatpakAppID", ""],
        [_MAP, "tags", []],
    ]


def upsert_shortcut(top: list, name: str, exe: str, entry: list) -> str:
    """Add or replace the entry for this app. Returns 'added' or 'replaced'."""
    entries = shortcuts_map(top)
    for item in entries:
        sub = item[2]
        if _find(sub, "Exe") == exe or _find(sub, "AppName") == name:
            item[2] = entry
            return "replaced"
    # Numbered keys are just the index as a string.
    entries.append([_MAP, str(len(entries)), entry])
    return "added"


_PATH_FIELDS = ("Exe", "StartDir", "LaunchOptions", "icon", "ShortcutPath")


def rewrite_shortcut_paths(top: list, old: str, new: str) -> list[str]:
    """Point every shortcut that refers to directory `old` at `new` instead.

    Rewrites the path fields (exe, start dir, launch options, icon) wherever
    `old` appears as a whole path component, keeping each entry's appid so
    its Proton mapping and artwork stay attached. Returns the names of the
    entries changed.
    """
    import re
    pattern = re.compile(re.escape(old.rstrip("/")) + r"(?=[/\\\"' ]|$)")
    changed = []
    for item in shortcuts_map(top):
        hit = False
        for field in item[2]:
            if field[0] == _STR and field[1] in _PATH_FIELDS and pattern.search(field[2]):
                field[2] = pattern.sub(new.rstrip("/").replace("\\", "\\\\"), field[2])
                hit = True
        if hit:
            changed.append(_find(item[2], "AppName") or "?")
    return changed


# ---- text config.vdf -----------------------------------------------------


def parse_text_vdf(text: str) -> dict:
    """Parse a text KeyValues document into nested dicts (order preserved)."""
    i, n = 0, len(text)

    def skip_ws() -> None:
        nonlocal i
        while i < n:
            c = text[i]
            if c in " \t\r\n":
                i += 1
            elif text.startswith("//", i):
                while i < n and text[i] != "\n":
                    i += 1
            else:
                break

    def token() -> str:
        nonlocal i
        skip_ws()
        if i >= n:
            raise ValueError("unexpected end of VDF")
        if text[i] == '"':
            i += 1
            start = i
            out = []
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    nxt = text[i + 1]
                    out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"'}.get(nxt, nxt))
                    i += 2
                else:
                    out.append(text[i])
                    i += 1
            i += 1  # closing quote
            return "".join(out) if out or start != i else ""
        # bare token (brace or unquoted word)
        start = i
        while i < n and text[i] not in ' \t\r\n"{}':
            i += 1
        return text[start:i]

    def parse_map() -> dict:
        nonlocal i
        d: dict = {}
        while True:
            skip_ws()
            if i >= n or text[i] == "}":
                i += 1
                return d
            key = token()
            skip_ws()
            if i < n and text[i] == "{":
                i += 1
                d[key] = parse_map()
            else:
                d[key] = token()
        return d

    skip_ws()
    root: dict = {}
    while i < n:
        key = token()
        skip_ws()
        if i < n and text[i] == "{":
            i += 1
            root[key] = parse_map()
        elif key:
            root[key] = token()
        skip_ws()
    return root


def dump_text_vdf(root: dict) -> str:
    out: list[str] = []

    def esc(s: str) -> str:
        return s.replace("\\", "\\\\").replace('"', '\\"')

    def emit(d: dict, depth: int) -> None:
        pad = "\t" * depth
        for key, val in d.items():
            if isinstance(val, dict):
                out.append(f'{pad}"{esc(key)}"\n{pad}{{\n')
                emit(val, depth + 1)
                out.append(f"{pad}}}\n")
            else:
                out.append(f'{pad}"{esc(key)}"\t\t"{esc(val)}"\n')

    emit(root, 0)
    return "".join(out)


def _ci_get(d: dict, key: str):
    for k in d:
        if k.lower() == key.lower():
            return k, d[k]
    return None, None


_COMPAT_PATH = ("InstallConfigStore", "Software", "Valve", "Steam",
                "CompatToolMapping")


def _descend(node: dict, part: str, create: bool) -> dict | None:
    """Child map for part (case-insensitive), created if asked and missing."""
    _, child = _ci_get(node, part)
    if isinstance(child, dict):
        return child
    if not create:
        return None
    child = {}
    node[part] = child
    return child


def set_compat_tool(root: dict, appid: int, tool: str) -> None:
    """Point the given appid at a Proton (compat tool) name in config.vdf."""
    node = root
    for part in _COMPAT_PATH:
        node = _descend(node, part, create=True)
    node[str(appid)] = {"name": tool, "config": "", "priority": "250"}


def default_compat_tool(root: dict) -> str | None:
    """The Proton the user picked for "all other titles" in Steam's settings
    (Steam stores it as the mapping for app id 0)."""
    node = root
    for part in _COMPAT_PATH:
        node = _descend(node, part, create=False)
        if node is None:
            return None
    m = node.get("0")
    name = m.get("name") if isinstance(m, dict) else None
    return name or None


def compat_mappings(root: dict) -> list[dict]:
    """All per-app compat tool entries in config.vdf."""
    node = root
    for part in _COMPAT_PATH:
        node = _descend(node, part, create=False)
        if node is None:
            return []
    return [m for m in node.values() if isinstance(m, dict)]


def detect_proton_tool(root: dict) -> str | None:
    """Reuse whatever Proton other games are already mapped to, if any."""
    node = root
    for part in _COMPAT_PATH:
        node = _descend(node, part, create=False)
        if node is None:
            return None
    counts: dict[str, int] = {}
    for mapping in node.values():
        if isinstance(mapping, dict):
            name = mapping.get("name", "")
            if name.lower().startswith("proton"):
                counts[name] = counts.get(name, 0) + 1
    return max(counts, key=counts.get) if counts else None
