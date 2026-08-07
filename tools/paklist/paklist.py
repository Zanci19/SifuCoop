"""List level (.umap) paths from Sifu's pak index.

Reads only the FILE INDEX -- the directory listing -- not asset contents. The
mod needs level names to offer a level picker, and they exist nowhere else: the
executable has no map list, and the pak index is encrypted.

The AES key is supplied on the command line and deliberately not stored in this
repository.

Usage:
    python paklist.py <pak> <aes-key-hex> [--out levels.txt]
"""

import argparse
import struct
import sys

from Crypto.Cipher import AES

PAK_MAGIC = 0x5A6F12E1


def read_fstring(data, pos):
    """UE FString: int32 length, then chars. Negative length means UTF-16."""
    (length,) = struct.unpack_from("<i", data, pos)
    pos += 4
    if length == 0:
        return "", pos
    if length < 0:
        count = -length
        raw = data[pos : pos + count * 2]
        pos += count * 2
        return raw.decode("utf-16-le").rstrip("\0"), pos
    raw = data[pos : pos + length]
    pos += length
    return raw.decode("utf-8", "replace").rstrip("\0"), pos


def find_footer(data):
    """Locate the pak footer magic near the end of the file."""
    for offset in range(len(data) - 4, -1, -1):
        if struct.unpack_from("<I", data, offset)[0] == PAK_MAGIC:
            return offset
    raise ValueError("pak magic not found")


def decrypt(blob, key):
    if len(blob) % 16 != 0:
        raise ValueError(f"encrypted block is not a multiple of 16 ({len(blob)})")
    # UE encrypts pak indices with AES-256 in ECB.
    return AES.new(key, AES.MODE_ECB).decrypt(blob)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pak")
    parser.add_argument("key", help="AES key as hex, with or without 0x")
    parser.add_argument("--out", default=None)
    parser.add_argument("--all", action="store_true", help="list every file, not just maps")
    args = parser.parse_args()

    key_hex = args.key.lower().removeprefix("0x")
    key = bytes.fromhex(key_hex)
    if len(key) != 32:
        print(f"expected a 32-byte (256-bit) key, got {len(key)}")
        return 1

    with open(args.pak, "rb") as fh:
        fh.seek(0, 2)
        size = fh.tell()
        fh.seek(size - 256)
        tail = fh.read(256)

        magic_at = find_footer(tail)
        version, index_offset, index_size = struct.unpack_from("<IQQ", tail, magic_at + 4)
        encrypted = tail[magic_at - 1] != 0
        print(f"pak version {version}, index at {index_offset} ({index_size} bytes), "
              f"encrypted={encrypted}")

        fh.seek(index_offset)
        index = fh.read(index_size)
        if encrypted:
            index = decrypt(index, key)

        # Primary index: mount point, entry count, then the path-hash and full
        # directory index locations.
        pos = 0
        mount_point, pos = read_fstring(index, pos)
        (num_entries,) = struct.unpack_from("<I", index, pos)
        pos += 4
        print(f"mount point: {mount_point}   entries: {num_entries}")

        pos += 8  # PathHashSeed
        (has_path_hash,) = struct.unpack_from("<i", index, pos)
        pos += 4
        if has_path_hash:
            pos += 8 + 8 + 20  # offset, size, hash

        (has_full_dir,) = struct.unpack_from("<i", index, pos)
        pos += 4
        if not has_full_dir:
            print("this pak has no full directory index -- filenames are unavailable")
            return 1

        dir_offset, dir_size = struct.unpack_from("<qq", index, pos)
        pos += 16

        fh.seek(dir_offset)
        directory = fh.read(dir_size)
        if encrypted:
            directory = decrypt(directory, key)

    # Full directory index: TMap<FString dir, TMap<FString file, u32>>
    pos = 0
    (dir_count,) = struct.unpack_from("<I", directory, pos)
    pos += 4

    files = []
    for _ in range(dir_count):
        dir_name, pos = read_fstring(directory, pos)
        (file_count,) = struct.unpack_from("<I", directory, pos)
        pos += 4
        for _ in range(file_count):
            file_name, pos = read_fstring(directory, pos)
            pos += 4  # entry offset into the encoded entries blob
            files.append(dir_name + file_name)

    print(f"{len(files)} files in the index")

    if args.all:
        selected = sorted(files)
    else:
        # Levels only. Sifu keeps sublevels next to their _Main map; the mod can
        # only travel to real maps, so everything else is noise here.
        selected = sorted(f for f in files if f.lower().endswith(".umap"))

    print(f"{len(selected)} selected")
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write("\n".join(selected) + "\n")
        print(f"wrote {args.out}")
    else:
        for name in selected[:80]:
            print("  " + name)

    return 0


if __name__ == "__main__":
    sys.exit(main())
