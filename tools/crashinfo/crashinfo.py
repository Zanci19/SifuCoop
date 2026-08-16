"""Answer one question about a UE4 minidump: which module was executing?

Sifu's crash reports arrive with an unsymbolised call stack -- often a single
module name, sometimes nothing at all -- which is not enough to tell whether a
crash belongs to the mod or to the game. The minidump itself always carries the
faulting instruction address and the loaded module ranges, and that is enough to
attribute it.

Usage:
    python crashinfo.py <UE4Minidump.dmp>
"""

import struct
import sys

STREAM_MODULE_LIST = 4
STREAM_EXCEPTION = 6

EXCEPTION_NAMES = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000094: "INTEGER_DIVIDE_BY_ZERO",
    0xC00000FD: "STACK_OVERFLOW",
    0x80000003: "BREAKPOINT",
}


def read_minidump_string(data, rva):
    (length,) = struct.unpack_from("<I", data, rva)
    raw = data[rva + 4: rva + 4 + length]
    return raw.decode("utf-16-le", "replace")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    with open(sys.argv[1], "rb") as fh:
        data = fh.read()

    signature, _version, stream_count, directory_rva = struct.unpack_from("<IIII", data, 0)
    if signature != 0x504D444D:
        print("not a minidump")
        return 1

    streams = {}
    for i in range(stream_count):
        stream_type, size, rva = struct.unpack_from("<III", data, directory_rva + i * 12)
        streams[stream_type] = (size, rva)

    exception_address = None
    if STREAM_EXCEPTION in streams:
        _size, rva = streams[STREAM_EXCEPTION]

        code, flags = struct.unpack_from("<II", data, rva + 8)
        (exception_address,) = struct.unpack_from("<Q", data, rva + 24)
        (param_count,) = struct.unpack_from("<I", data, rva + 32)
        params = struct.unpack_from(f"<{max(param_count, 0)}Q", data, rva + 40) \
            if 0 < param_count <= 15 else ()
        name = EXCEPTION_NAMES.get(code, f"0x{code:08X}")
        print(f"exception        {name} (0x{code:08X}) flags=0x{flags:X}")
        print(f"faulting address 0x{exception_address:016X}")
        if len(params) >= 2:
            kind = {0: "reading", 1: "writing", 8: "executing"}.get(params[0], str(params[0]))
            print(f"operation        {kind} 0x{params[1]:016X}")

    modules = []
    if STREAM_MODULE_LIST in streams:
        _size, rva = streams[STREAM_MODULE_LIST]
        (count,) = struct.unpack_from("<I", data, rva)
        for i in range(count):
            entry = rva + 4 + i * 108
            base, size, _checksum, _stamp, name_rva = struct.unpack_from("<QIIII", data, entry)
            modules.append((base, size, read_minidump_string(data, name_rva)))

    modules.sort()
    print(f"modules          {len(modules)} loaded")

    if exception_address is None:
        return 0

    owner = None
    for base, size, name in modules:
        if base <= exception_address < base + size:
            owner = (base, size, name)
            break

    print()
    if owner:
        base, size, name = owner
        offset = exception_address - base
        print(f"FAULTING MODULE  {name}")
        print(f"                 base 0x{base:X}  size 0x{size:X}  offset +0x{offset:X}")
    else:
        print("FAULTING MODULE  none -- the address is not inside any loaded module")
        print("                 (a jump through a bad pointer, or freed/JIT memory)")




    for base, size, name in modules:
        if "dsound" in name.lower():
            print(f"\nmod              {name}")
            print(f"                 base 0x{base:X}  size 0x{size:X}  "
                  f"(ends 0x{base + size:X})")
            inside = base <= exception_address < base + size
            print(f"                 faulting address inside it: "
                  f"{'YES' if inside else 'no'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
