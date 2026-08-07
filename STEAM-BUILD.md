# Building SifuCoop for the Steam edition

This kit lets the **Steam** PC compile its own `dsound.dll` from the Steam build's
own PDB. The result also carries the Epic build's fingerprint (from the bundled
`builds/epic.json`), so the single DLL it produces works on **both** stores — copy
it to the Epic PC too and both machines run byte-for-byte the same mod.

You only need to do this once per Steam game patch. If Sifu updates on Steam, run
it again.

> Prerequisite that cannot be worked around: the two editions must be the **same
> game patch**. Enemies are matched across machines by their pool object-names,
> which only line up when the content is identical. If a build below reports fewer
> than the Epic build's offset count, they are different patches — update both to
> the latest and redo.

---

## 1. Get the two tools (Steam PC, one time) — NO winget / Store needed

Both come as plain downloads. If you have winget the one-liners at the bottom still
work, but you do not need it.

**g++ (WinLibs, portable — just a zip, no installer):**
1. Go to https://winlibs.com → download the **UCRT runtime**, **Win64**, **POSIX
   threads** ZIP (the "release" build is fine).
2. Unzip it to `C:\winlibs`. You should end up with `C:\winlibs\mingw64\bin\g++.exe`.
   Nothing to install — you pass this folder to the build in step 4.

**Python (official installer from python.org — a normal .exe, not the Store one):**
1. https://www.python.org/downloads/windows/ → "Windows installer (64-bit)".
2. Run it, tick **"Add python.exe to PATH"**, install. Reopen PowerShell, check:
   ```
   python --version
   ```

(Python is only the pure-stdlib PDB reader; g++ is the compiler. If you would rather
not install g++ at all, see **the lighter Python-only route** at the bottom of this
file — you only need Python for that.)

> Have winget after all? `winget install BrechtSanders.WinLibs.POSIX.UCRT` and
> `winget install Python.Python.3.12` do the same thing; then skip the `-ToolchainBin`
> argument in step 4.

---

## 2. Unzip this kit

Put it somewhere with no spaces in the path, e.g. `C:\SifuCoopKit`. Open PowerShell
**in that folder** (Shift-right-click the folder → "Open PowerShell window here").

---

## 3. Find your Steam Sifu folder

It is normally:

```
C:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64
```

Confirm both of these exist inside it:

- `Sifu-Win64-Shipping.exe`
- `Sifu-Win64-Shipping.pdb`   ← the whole thing depends on this file

If Steam is on another drive/library, use that path instead in the next step.

---

## 4. Build and deploy — one command

```
.\build.ps1 -ToolchainBin "C:\winlibs\mingw64\bin" -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam -Deploy
```

(`-ToolchainBin` points the build at the portable g++ from step 1. Drop that argument
if you installed g++ via winget. If Python is portable too, add its folder:
`-ToolchainBin "C:\winlibs\mingw64\bin;C:\python312"`.)

What it does: reads the Steam PDB → records `builds\steam.json` → merges it with the
bundled `builds\epic.json` → verifies the crypto test vectors → compiles
`dsound.dll` → copies it into your Steam `Win64` folder.

**Read the output.** You want to see:

- `wrote ...\builds\steam.json: build 'steam' stamp=0x... size=0x... (N offsets)`
  — **N must equal the Epic build's count.** As of this kit that is **69**. If it is
  lower, you are on a different patch than Epic (see the warning at the top).
- `all vectors pass`
- `built ...\build\dsound.dll (...)` with no warnings
- `deployed to ...Steam...\Win64`

If g++ or python is "not found", step 1 did not take — reopen PowerShell.

---

## 5. Put the config next to the game

Copy the bundled `SifuCoop.ini` into the same Steam `Win64` folder (next to the DLL),
then edit `[net]`:

- On the machine that hosts: `mode=host`
- On the other: `mode=client` and `host=<the host's IP>`
- **Same `passphrase` on both. Same `port` on both.**

Nothing else needs changing to start.

---

## 6. (Optional but recommended) unify both PCs on this DLL

The DLL you just built is dual-build. Copy `build\dsound.dll` from this kit to the
**Epic** PC's `...\Epic Games\Sifu\Sifu\Binaries\Win64\` too, replacing the one there.
Now both machines run the identical mod and protocol version — one less thing to
differ.

---

## 7. Confirm it loaded (each PC)

Launch Sifu (through the Steam / Epic launcher, not the exe directly). Check:

```
%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log
```

- Steam PC should log `guard: build 'steam' matched`.
- Epic PC should log `guard: build 'epic' matched`.
- Both should then log `GEngine populated` and reach reflection init.

`guard: UNKNOWN BUILD` on Steam means the fingerprint did not match — the DLL there
is stale, redo step 4 and recopy. Any core offset logging as `0` (GEngine/GWorld)
means a different patch — see the top warning.

---

## 8. Connect and test

Easiest link is ZeroTier: both PCs join one network at my.zerotier.com, authorise
each device, use the `10.x` Managed IPs (client's `host=` is the host's `10.x`).
Same passphrase. Then follow the test order in `SETUP.md`: see each other move →
enemy positions/health/death agree → the joiner kills a real enemy → the room clears
on both → level transitions carry the joiner along.

Cross-store is invisible to the netcode. If the patches match, Epic↔Steam plays
exactly like same-store.

---

## The lighter Python-only route (no g++ on the Steam PC)

If you would rather not put a compiler on the Steam PC, it only needs **Python**. It
generates the small `steam.json` fingerprint file; whoever has the compiler (the Epic
PC) builds the DLL from it.

On the Steam PC, in the unzipped kit folder:

```
python tools\pdbdump\pdbdump.py "C:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64\Sifu-Win64-Shipping.pdb" --exe "C:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64\Sifu-Win64-Shipping.exe" --emit-build steam builds\steam.json
```

It writes `builds\steam.json` (a few KB) and prints the offset count — still must be
**69**. Send that one file to the Epic PC, drop it in its `builds\` folder, and there
run:

```
.\build.ps1 -Deploy
```

That merges Epic + Steam into one DLL. Copy the resulting `build\dsound.dll` back to
the Steam PC's `Win64` folder (step 5 onward). Same end result, no compiler on the
Steam side — just one small file each way.
