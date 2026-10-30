#!/usr/bin/env python3
"""Summarize a Windows x64 minidump (e.g. Unreal's UE4Minidump.dmp).

Prints the exception, the module it happened in, a heuristic call stack
(return addresses found on the crashing thread's stack, mapped to modules),
and the loaded modules. Standard library only.

    python3 tools/minidump_info.py path/to/UE4Minidump.dmp
"""

from __future__ import annotations

import os
import struct
import sys

EXCEPTION_NAMES = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000094: "INTEGER_DIVIDE_BY_ZERO",
    0xC00000FD: "STACK_OVERFLOW",
    0xC0000409: "STACK_BUFFER_OVERRUN (fail-fast)",
    0xC0000374: "HEAP_CORRUPTION",
    0xC000041D: "FATAL_USER_CALLBACK_EXCEPTION",
    0xC06D007E: "DELAYLOAD: module not found",
    0xC06D007F: "DELAYLOAD: procedure not found",
    0xE06D7363: "C++ exception (MSVC throw)",
    0x80000003: "BREAKPOINT",
    0x40010006: "DBG_PRINTEXCEPTION",
}

STREAM_THREADS, STREAM_MODULES, STREAM_MEMORY, STREAM_EXCEPTION = 3, 4, 5, 6


class Dump:
    def __init__(self, data: bytes):
        if data[:4] != b"MDMP":
            raise SystemExit("not a minidump (missing MDMP signature)")
        self.data = data
        nstreams, dir_rva = struct.unpack_from("<II", data, 8)
        self.streams = {}
        for i in range(nstreams):
            stype, size, rva = struct.unpack_from("<III", data, dir_rva + 12 * i)
            self.streams[stype] = (rva, size)

    def string(self, rva: int) -> str:
        (length,) = struct.unpack_from("<I", self.data, rva)
        return self.data[rva + 4:rva + 4 + length].decode("utf-16-le", "replace")

    def modules(self) -> list[tuple[int, int, str]]:
        if STREAM_MODULES not in self.streams:
            return []
        rva, _ = self.streams[STREAM_MODULES]
        (count,) = struct.unpack_from("<I", self.data, rva)
        mods = []
        for i in range(count):
            off = rva + 4 + 108 * i
            base, size = struct.unpack_from("<QI", self.data, off)
            (name_rva,) = struct.unpack_from("<I", self.data, off + 20)
            mods.append((base, size, self.string(name_rva)))
        return sorted(mods)

    def exception(self):
        if STREAM_EXCEPTION not in self.streams:
            return None
        rva, _ = self.streams[STREAM_EXCEPTION]
        (tid,) = struct.unpack_from("<I", self.data, rva)
        code, flags, _rec, addr, nparams = struct.unpack_from("<IIQQI", self.data, rva + 8)
        params = struct.unpack_from("<15Q", self.data, rva + 8 + 32)[:nparams]
        ctx_size, ctx_rva = struct.unpack_from("<II", self.data, rva + 8 + 32 + 120)
        return tid, code, addr, params, (ctx_rva, ctx_size)

    def read(self, addr: int, n: int) -> bytes | None:
        """Read process memory captured in the dump (stacks etc.), if present."""
        if STREAM_MEMORY not in self.streams:
            return None
        rva, _ = self.streams[STREAM_MEMORY]
        (count,) = struct.unpack_from("<I", self.data, rva)
        for i in range(count):
            start, size, mem_rva = struct.unpack_from("<QII", self.data, rva + 4 + 16 * i)
            if start <= addr and addr + n <= start + size:
                off = mem_rva + addr - start
                return self.data[off:off + n]
        return None

    def thread_stack(self, tid: int):
        if STREAM_THREADS not in self.streams:
            return None
        rva, _ = self.streams[STREAM_THREADS]
        (count,) = struct.unpack_from("<I", self.data, rva)
        for i in range(count):
            off = rva + 4 + 48 * i
            (thread_id,) = struct.unpack_from("<I", self.data, off)
            if thread_id == tid:
                start, size, mem_rva = struct.unpack_from("<QII", self.data, off + 24)
                return start, self.data[mem_rva:mem_rva + size]
        return None


def unix_path(win_path: str) -> str | None:
    """Map Wine's Z:\\ paths back to the Linux filesystem."""
    if win_path[:3].upper() == "Z:\\":
        return "/" + win_path[3:].replace("\\", "/")
    return None


def read_cstring_from_module(mods, addr: int) -> str | None:
    """Read a string that lives in a module's image, using the file on disk."""
    for base, size, name in mods:
        if not base <= addr < base + size:
            continue
        path = unix_path(name)
        if not path or not os.path.isfile(path):
            return None
        rva = addr - base
        with open(path, "rb") as f:
            pe = f.read(4096)
            (pe_off,) = struct.unpack_from("<I", pe, 0x3C)
            nsec, = struct.unpack_from("<H", pe, pe_off + 6)
            opt_size, = struct.unpack_from("<H", pe, pe_off + 20)
            sec = pe_off + 24 + opt_size
            for i in range(nsec):
                vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", pe, sec + 40 * i + 8)
                if vaddr <= rva < vaddr + max(vsize, raw_size):
                    f.seek(raw_ptr + rva - vaddr)
                    return f.read(256).split(b"\0")[0].decode("ascii", "replace")
    return None


def explain_delayload(d: "Dump", mods, info_addr: int) -> None:
    # DelayLoadInfo (x64): cb, pidd, ppfn, szDll @24, dlp {fImportByName @32,
    # szProcName/dwOrdinal @40}, hmodCur, pfnCur, dwLastError @64.
    raw = d.read(info_addr, 68)
    if not raw:
        print("  (DelayLoadInfo not captured in the dump)")
        return
    sz_dll, by_name, proc = struct.unpack_from("<QI4xQ", raw, 24)
    (last_error,) = struct.unpack_from("<I", raw, 64)
    dll = read_cstring_from_module(mods, sz_dll) or f"<name at 0x{sz_dll:x}, not readable>"
    if by_name:
        func = read_cstring_from_module(mods, proc) or f"<name at 0x{proc:x}>"
    else:
        func = f"ordinal {proc & 0xFFFF}"
    print(f"  missing DLL: {dll}")
    print(f"  wanted function: {func}  (last error {last_error})")


def locate(mods, addr: int) -> str:
    for base, size, name in mods:
        if base <= addr < base + size:
            return f"{name.split(chr(92))[-1]}+0x{addr - base:x}"
    return "?"


def main(path: str) -> int:
    d = Dump(open(path, "rb").read())
    mods = d.modules()
    exc = d.exception()
    if not exc:
        print("no exception stream in this dump")
    else:
        tid, code, addr, params, (ctx_rva, ctx_size) = exc
        print(f"exception: 0x{code:08X} {EXCEPTION_NAMES.get(code, '')}")
        print(f"  at 0x{addr:x}  ({locate(mods, addr)})  thread {tid}")
        if code == 0xC0000005 and len(params) >= 2:
            kind = {0: "read", 1: "write", 8: "execute"}.get(params[0], str(params[0]))
            print(f"  {kind} of address 0x{params[1]:x}")
        if code in (0xC06D007E, 0xC06D007F) and params:
            print("  (delay-load failure: a DLL or function the game expects is missing)")
            explain_delayload(d, mods, params[0])
        if ctx_size >= 0x100:
            rsp, = struct.unpack_from("<Q", d.data, ctx_rva + 0x98)
            rip, = struct.unpack_from("<Q", d.data, ctx_rva + 0xF8)
            print(f"  rip 0x{rip:x} ({locate(mods, rip)})  rsp 0x{rsp:x}")
        stack = d.thread_stack(tid)
        if stack:
            start, mem = stack
            print("\nlikely call stack (return addresses found on the stack, innermost first):")
            shown = 0
            for off in range(0, len(mem) - 7, 8):
                (val,) = struct.unpack_from("<Q", mem, off)
                where = locate(mods, val)
                if where != "?":
                    print(f"  [sp+0x{off:04x}] {where}")
                    shown += 1
                    if shown >= 40:
                        break
    print(f"\nmodules ({len(mods)}):")
    for base, size, name in mods:
        print(f"  0x{base:016x} {size:>10}  {name}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__.strip())
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
