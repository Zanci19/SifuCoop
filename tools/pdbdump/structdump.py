"""Recover USTRUCT/UCLASS member offsets from the shipped executable.

Unreal's code generator emits, for every reflected class and struct, a table of
`FPropertyParams` describing each property -- including its byte offset inside
the owning type. Those tables live in .rdata and the PDB names every one of
them, so member offsets can be read straight out of the exe instead of being
guessed by diffing hex dumps at runtime.

This is how the mod learns where `m_fHealth` sits inside UHealthComponent, or
what an FNetOrderStructAttack actually contains, without ever dereferencing an
unknown field in the live game.

Usage:
    python structdump.py <exe> <pdb> UHealthComponent FNetOrderStructAttack ...
    python structdump.py <exe> <pdb> --raw UHealthComponent     # hex, for layout work
"""

import re
import struct
import sys

from pdbdump import load_symbols


GEN_FLAG_NAMES = {
    0x00: "Byte", 0x01: "Int8", 0x02: "Int16", 0x03: "Int", 0x04: "Int64",
    0x05: "UInt16", 0x06: "UInt32", 0x07: "UInt64", 0x08: "UnsizedInt",
    0x09: "UnsizedUInt", 0x0A: "Float", 0x0B: "Double", 0x0C: "Bool",
    0x0D: "SoftClass", 0x0E: "WeakObject", 0x0F: "LazyObject",
    0x10: "SoftObject", 0x11: "Class", 0x12: "Object", 0x13: "Interface",
    0x14: "Name", 0x15: "Str", 0x16: "Array", 0x17: "Map", 0x18: "Set",
    0x19: "Struct", 0x1A: "Delegate", 0x1B: "InlineMulticastDelegate",
    0x1C: "SparseMulticastDelegate", 0x1D: "Text", 0x1E: "Enum",
    0x1F: "FieldPath",
}


class Image:
    """Just enough PE to turn an RVA into bytes."""

    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        assert self.data[pe:pe + 4] == b"PE\0\0", "not a PE file"
        num_sections = struct.unpack_from("<H", self.data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", self.data, pe + 20)[0]
        opt = pe + 24
        self.image_base = struct.unpack_from("<Q", self.data, opt + 24)[0]
        self.sections = []
        table = opt + opt_size
        for i in range(num_sections):
            off = table + i * 40
            virtual_size, virtual_address, raw_size, raw_ptr = struct.unpack_from(
                "<IIII", self.data, off + 8)
            self.sections.append((virtual_address, virtual_size, raw_size, raw_ptr))

    def read(self, rva, size):
        for va, vsize, raw_size, raw_ptr in self.sections:
            if va <= rva < va + max(vsize, raw_size):
                delta = rva - va
                if delta + size > raw_size:
                    size = max(0, raw_size - delta)
                return self.data[raw_ptr + delta: raw_ptr + delta + size]
        return b""

    def read_va(self, va, size):
        if va < self.image_base:
            return b""
        return self.read(va - self.image_base, size)

    def cstring_va(self, va, limit=256):
        raw = self.read_va(va, limit)
        end = raw.find(b"\0")
        return raw[:end if end >= 0 else len(raw)].decode("utf-8", "replace")










PROP_HEADER = 0x28


def parse_prop(image, rva):
    raw = image.read(rva, PROP_HEADER)
    if len(raw) < PROP_HEADER:
        return None
    name_va, notify_va, flags, gen_flags, obj_flags, array_dim = struct.unpack_from(
        "<QQQIII", raw, 0)
    offset = struct.unpack_from("<H", raw, 0x24)[0]

    kind = GEN_FLAG_NAMES.get(gen_flags & 0x1F, f"?{gen_flags & 0x1F:02X}")
    return {
        "name": image.cstring_va(name_va) if name_va else "",
        "flags": flags,
        "kind": kind,
        "array_dim": array_dim,
        "offset": offset,
        "raw": raw,
    }


def main():
    argv = sys.argv[1:]
    raw_mode = "--raw" in argv
    argv = [a for a in argv if a != "--raw"]
    if len(argv) < 3:
        print(__doc__)
        return 1

    exe_path, pdb_path = argv[0], argv[1]
    wanted = argv[2:]

    image = Image(exe_path)
    pdb, symbols = load_symbols(pdb_path, verbose=False)


    pattern = re.compile(
        r"^\?NewProp_(?P<member>[^@]+)@Z_Construct_U(?:ScriptStruct|Class|Function)_"
        r"(?P<type>[A-Za-z0-9_]+)_Statics@@2U(?P<params>F[A-Za-z]*PropertyParams)@")

    by_type = {}
    struct_params = {}
    for sym in symbols:
        match = pattern.match(sym.name)
        if match:
            by_type.setdefault(match.group("type"), []).append(
                (match.group("member"), match.group("params"), sym.rva))
            continue

        if sym.name.startswith("?ReturnStructParams@Z_Construct_UScriptStruct_"):
            type_name = sym.name.split("Z_Construct_UScriptStruct_")[1].split("_Statics")[0]
            struct_params[type_name] = sym.rva

    for want in wanted:
        props = by_type.get(want)
        print(f"\n=== {want} ===")
        if want in struct_params:
            raw = image.read(struct_params[want], 0x50)


            size_of = struct.unpack_from("<Q", raw, 0x20)[0]
            align_of = struct.unpack_from("<Q", raw, 0x28)[0]
            num_props = struct.unpack_from("<i", raw, 0x38)[0]
            print(f"    sizeof={size_of} (0x{size_of:X})  align={align_of}  "
                  f"reflected_props={num_props}")
            if raw_mode:
                print("    structparams raw: " + " ".join(f"{b:02X}" for b in raw))
        if not props:
            print("    (no reflected properties found)")
            continue
        parsed = []
        for member, params_type, rva in props:
            if member.endswith("_SetBit"):
                continue
            info = parse_prop(image, rva)
            if not info:
                continue
            info["member"] = member
            info["params_type"] = params_type
            parsed.append(info)
        parsed.sort(key=lambda p: p["offset"])
        for p in parsed:
            note = "" if p["array_dim"] == 1 else f" [{p['array_dim']}]"
            print(f"    +0x{p['offset']:04X}  {p['kind']:<12} {p['name']}{note}"
                  f"   ({p['params_type']})")
            if raw_mode:
                print("             raw: " + " ".join(f"{b:02X}" for b in p["raw"]))

    pdb.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
