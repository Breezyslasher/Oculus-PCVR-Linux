"""Read the export table of a Windows PE file (DLL/EXE) without extra deps.

Used to compare our stub DLLs with the real LibOVRRT / LibOVRPlatform DLLs
and find exports a game might need that we do not provide yet:

    python3 -m ovrl.pe real/LibOVRPlatform64_1.dll build/LibOVRPlatform64_1.dll
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path


class PEError(ValueError):
    pass


def _sections(data: bytes, pe_off: int) -> list[tuple[int, int, int]]:
    """Return (virtual_address, virtual_size, raw_offset) per section."""
    num_sections, = struct.unpack_from("<H", data, pe_off + 6)
    opt_size, = struct.unpack_from("<H", data, pe_off + 20)
    table = pe_off + 24 + opt_size
    out = []
    for i in range(num_sections):
        off = table + i * 40
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        out.append((vaddr, max(vsize, raw_size), raw_ptr))
    return out


def _rva_to_offset(rva: int, sections: list[tuple[int, int, int]]) -> int:
    for vaddr, size, raw in sections:
        if vaddr <= rva < vaddr + size:
            return rva - vaddr + raw
    raise PEError(f"RVA {rva:#x} is outside every section")


def _cstr(data: bytes, off: int) -> str:
    end = data.index(b"\0", off)
    return data[off:end].decode("ascii", "replace")


def exports(path: str | Path) -> list[str]:
    """Return the exported symbol names of a PE file, sorted."""
    data = Path(path).read_bytes()
    if data[:2] != b"MZ":
        raise PEError(f"{path}: not a PE file")
    pe_off, = struct.unpack_from("<I", data, 0x3C)
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise PEError(f"{path}: bad PE signature")

    opt = pe_off + 24
    magic, = struct.unpack_from("<H", data, opt)
    dir_off = opt + {0x10B: 96, 0x20B: 112}.get(magic, -1)
    if dir_off < opt:
        raise PEError(f"{path}: unknown optional header magic {magic:#x}")

    export_rva, export_size = struct.unpack_from("<II", data, dir_off)
    if not export_rva:
        return []
    sections = _sections(data, pe_off)
    exp = _rva_to_offset(export_rva, sections)
    num_names, = struct.unpack_from("<I", data, exp + 24)
    names_rva, = struct.unpack_from("<I", data, exp + 32)
    names = _rva_to_offset(names_rva, sections)

    result = []
    for i in range(num_names):
        name_rva, = struct.unpack_from("<I", data, names + 4 * i)
        result.append(_cstr(data, _rva_to_offset(name_rva, sections)))
    return sorted(result)


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__.strip())
        return 2
    if len(argv) == 2:
        # Two files: show what the second (ours) is missing from the first.
        reference, ours = (set(exports(p)) for p in argv)
        missing = sorted(reference - ours)
        print(f"{len(missing)} of {len(reference)} exports in {argv[0]} "
              f"are missing from {argv[1]}:")
        for name in missing:
            print(f"  {name}")
        return 0
    for p in argv:
        names = exports(p)
        print(f"{p}: {len(names)} exports")
        for name in names:
            print(f"  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
