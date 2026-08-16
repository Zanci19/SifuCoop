"""PDB symbol extraction: DBI stream -> public symbols -> RVAs.

Only the parts we need for offset generation are implemented.
"""

import struct

from msf import MsfFile

STREAM_DBI = 3


S_PUB32 = 0x110E
S_GPROC32 = 0x1110
S_LPROC32 = 0x110F
S_GDATA32 = 0x110D
S_LDATA32 = 0x110C


DBG_SECTION_HDR = 5
DBG_SECTION_HDR_ORIG = 10


class DbiHeader:
    SIZE = 64

    def __init__(self, data):
        (
            self.version_signature,
            self.version_header,
            self.age,
            self.global_stream,
            self.build_number,
            self.public_stream,
            self.pdb_dll_version,
            self.sym_record_stream,
            self.pdb_dll_rbld,
            self.mod_info_size,
            self.section_contribution_size,
            self.section_map_size,
            self.source_info_size,
            self.type_server_map_size,
            self.mfc_type_server_index,
            self.optional_dbg_header_size,
            self.ec_substream_size,
            self.flags,
            self.machine,
            self.padding,
        ) = struct.unpack_from("<iIIHHHHHHiiiiiIiiHHI", data, 0)


class Section:
    __slots__ = ("name", "virtual_address", "virtual_size")

    def __init__(self, name, virtual_address, virtual_size):
        self.name = name
        self.virtual_address = virtual_address
        self.virtual_size = virtual_size


class Symbol:
    __slots__ = ("name", "rva", "kind", "section")

    def __init__(self, name, rva, kind, section):
        self.name = name
        self.rva = rva
        self.kind = kind
        self.section = section

    def __repr__(self):
        return f"Symbol({self.name!r}, rva=0x{self.rva:X})"


class PdbFile:
    def __init__(self, path):
        self.msf = MsfFile(path)
        dbi_raw = self.msf.stream(STREAM_DBI)
        if len(dbi_raw) < DbiHeader.SIZE:
            raise ValueError("DBI stream missing or truncated")
        self.dbi = DbiHeader(dbi_raw)
        self._dbi_raw = dbi_raw
        self.sections = self._read_sections()



    def _optional_dbg_header(self):
        offset = (
            DbiHeader.SIZE
            + self.dbi.mod_info_size
            + self.dbi.section_contribution_size
            + self.dbi.section_map_size
            + self.dbi.source_info_size
            + self.dbi.type_server_map_size
            + self.dbi.ec_substream_size
        )
        count = self.dbi.optional_dbg_header_size // 2
        if count <= 0:
            return []
        return list(struct.unpack_from(f"<{count}H", self._dbi_raw, offset))

    def _read_sections(self):
        headers = self._optional_dbg_header()
        stream_index = None
        for slot in (DBG_SECTION_HDR, DBG_SECTION_HDR_ORIG):
            if slot < len(headers) and headers[slot] != 0xFFFF:
                stream_index = headers[slot]
                break
        if stream_index is None:
            raise ValueError("PDB has no section header stream")

        raw = self.msf.stream(stream_index)
        sections = []

        for pos in range(0, len(raw) - 39, 40):
            name = raw[pos : pos + 8].rstrip(b"\x00").decode("ascii", "replace")
            virtual_size, virtual_address = struct.unpack_from("<II", raw, pos + 8)
            sections.append(Section(name, virtual_address, virtual_size))
        return sections

    def _to_rva(self, segment, offset):

        if segment == 0 or segment > len(self.sections):
            return None
        return self.sections[segment - 1].virtual_address + offset



    def iter_symbols(self):
        """Yield Symbol objects from the global symbol record stream."""
        raw = self.msf.stream(self.dbi.sym_record_stream)
        pos = 0
        end = len(raw)
        while pos + 4 <= end:
            (length, kind) = struct.unpack_from("<HH", raw, pos)
            if length < 2:
                break
            record_end = pos + 2 + length
            if record_end > end:
                break
            body = pos + 4

            if kind == S_PUB32:

                _flags, offset, segment = struct.unpack_from("<IIH", raw, body)
                name_start = body + 10
                sym = self._make_symbol(raw, name_start, record_end, segment, offset, kind)
                if sym is not None:
                    yield sym
            elif kind in (S_GDATA32, S_LDATA32):

                _type, offset, segment = struct.unpack_from("<IIH", raw, body)
                name_start = body + 10
                sym = self._make_symbol(raw, name_start, record_end, segment, offset, kind)
                if sym is not None:
                    yield sym

            pos = record_end

    def _make_symbol(self, raw, name_start, record_end, segment, offset, kind):
        terminator = raw.find(b"\x00", name_start, record_end)
        if terminator < 0:
            terminator = record_end
        name = raw[name_start:terminator].decode("utf-8", "replace")
        rva = self._to_rva(segment, offset)
        if rva is None or not name:
            return None
        return Symbol(name, rva, kind, segment)

    def close(self):
        self.msf.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
