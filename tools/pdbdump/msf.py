"""Minimal MSF (Multi-Stream Format) reader for PDB 7.00 files.

Deliberately dependency-free: this runs before any toolchain is installed, and it
has to keep working on the Ubuntu boot too.
"""

import struct

MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"


class MsfFile:
    """Random-access reader over the streams of a PDB 7.00 container."""

    def __init__(self, path):
        self.path = path
        self._fh = open(path, "rb")

        header = self._fh.read(len(MSF_MAGIC) + 24)
        if not header.startswith(MSF_MAGIC):
            raise ValueError(f"{path}: not an MSF 7.00 file")

        (
            self.block_size,
            self.free_block_map,
            self.num_blocks,
            self.directory_bytes,
            _unknown,
            self.block_map_addr,
        ) = struct.unpack_from("<6I", header, len(MSF_MAGIC))

        self.streams = self._read_directory()

    # -- block plumbing ---------------------------------------------------

    def _read_block(self, index):
        self._fh.seek(index * self.block_size)
        return self._fh.read(self.block_size)

    def _read_blocks(self, indices, size):
        """Concatenate `indices` and truncate to `size` bytes."""
        out = bytearray()
        for index in indices:
            out += self._read_block(index)
            if len(out) >= size:
                break
        return bytes(out[:size])

    def _block_count(self, size):
        return (size + self.block_size - 1) // self.block_size

    def _read_directory(self):
        # The block map holds the block indices of the stream directory itself.
        map_blocks = self._block_count(self.directory_bytes)
        raw_map = self._read_blocks(
            [self.block_map_addr], self.block_size
        )
        # A directory larger than one block's worth of indices spans several
        # map blocks; they are contiguous starting at block_map_addr.
        indices_per_block = self.block_size // 4
        map_block_count = (map_blocks + indices_per_block - 1) // indices_per_block
        if map_block_count > 1:
            raw_map = self._read_blocks(
                range(self.block_map_addr, self.block_map_addr + map_block_count),
                map_block_count * self.block_size,
            )
        dir_blocks = struct.unpack_from(f"<{map_blocks}I", raw_map, 0)
        directory = self._read_blocks(dir_blocks, self.directory_bytes)

        pos = 0
        (num_streams,) = struct.unpack_from("<I", directory, pos)
        pos += 4
        sizes = list(struct.unpack_from(f"<{num_streams}I", directory, pos))
        pos += 4 * num_streams

        streams = []
        for size in sizes:
            if size == 0xFFFFFFFF:  # deleted stream
                streams.append((0, []))
                continue
            count = self._block_count(size)
            blocks = list(struct.unpack_from(f"<{count}I", directory, pos))
            pos += 4 * count
            streams.append((size, blocks))
        return streams

    # -- public API -------------------------------------------------------

    def stream(self, index):
        if index >= len(self.streams) or index == 0xFFFF:
            return b""
        size, blocks = self.streams[index]
        return self._read_blocks(blocks, size)

    def stream_size(self, index):
        if index >= len(self.streams):
            return 0
        return self.streams[index][0]

    def close(self):
        self._fh.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
