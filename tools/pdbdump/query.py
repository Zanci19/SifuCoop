"""One-pass PDB query tool.

Parsing Sifu's 970 MB PDB takes minutes, so this loads it once and answers a
whole batch of regex queries, writing each result to its own file. Used for
research; nothing in the build depends on it.

Usage:
    python query.py <pdb> <outdir> <name>=<regex> [<name>=<regex> ...]
"""

import os
import re
import sys

from pdbdump import load_symbols


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1

    pdb_path = sys.argv[1]
    outdir = sys.argv[2]
    queries = []
    for arg in sys.argv[3:]:
        name, _, pattern = arg.partition("=")
        queries.append((name, re.compile(pattern)))

    os.makedirs(outdir, exist_ok=True)
    pdb, symbols = load_symbols(pdb_path)

    # Deduplicate: the same RVA/name pair shows up in several record kinds.
    seen = set()
    unique = []
    for sym in symbols:
        key = (sym.rva, sym.name)
        if key in seen:
            continue
        seen.add(key)
        unique.append(sym)
    print(f"unique symbols: {len(unique)}")

    for name, pattern in queries:
        hits = [s for s in unique if pattern.search(s.name)]
        hits.sort(key=lambda s: s.name)
        path = os.path.join(outdir, f"{name}.txt")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(f"# {len(hits)} hits for /{pattern.pattern}/\n")
            for sym in hits:
                fh.write(f"{sym.rva:08X}\t{sym.name}\n")
        print(f"{name:28s} {len(hits):6d} -> {path}")

    pdb.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
