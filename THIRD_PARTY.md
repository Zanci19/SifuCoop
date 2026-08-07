# Third-party components

SifuCoop bundles two libraries in `third_party/`. Both are permissively
licensed and are compiled into `dsound.dll`; their full licence texts ship in
their own directories.

## Dear ImGui — MIT

<https://github.com/ocornut/imgui>

Draws the in-game menu. Used with the DirectX 11 and Win32 backends.

Copyright (c) 2014-2024 Omar Cornut. See `third_party/imgui/LICENSE.txt`.

## MinHook — BSD 2-Clause

<https://github.com/TsudaKageyu/minhook>

Installs the inline hooks the mod needs on the game's combat entry points. It
brings its own HDE length disassembler, which is the part that makes relocating
an arbitrary function prologue safe rather than a guess about instruction
boundaries.

Copyright (c) 2009-2017 Tsuda Kageyu and contributors.
See `third_party/minhook/LICENSE.txt`.

---

## Written for this project

Everything in `src/` and `tools/` is original and MIT licensed (see `LICENSE`),
including:

- **SHA-256 and HMAC-SHA256** (`src/net/crypto.cpp`) — RFC 6234 and RFC 2104
  written out directly rather than pulled in as a dependency, so the one
  security-relevant part of the mod can be read end to end in one file. Verified
  against the published FIPS 180-4 and RFC 4231 vectors on every build; the
  build fails if any vector does not match.
- **PDB parsing and Unreal reflection-table reading** (`tools/pdbdump/`) — reads
  function addresses and struct member offsets out of a shipped executable.

## Not included

No Sifu code, assets, level data or debug files are in this repository. The mod
requires a legitimately owned copy of the game and redistributes nothing
belonging to it.

`builds/*.json` contains function addresses and struct offsets measured from the
game executable. These are facts about a binary rather than a copy of it, and
they are what lets the mod attach to the right code; without them nobody could
use it without also owning the game's 970 MB debug symbols. Anything bulkier
than that — the full symbol dump, the level listing — is deliberately left out
of version control and regenerated locally. See the README.
