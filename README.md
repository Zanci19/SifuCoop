# SifuCoop

Two-player co-op for [Sifu](https://www.sloclap.com/sifu). Both players fight the game's own
enemies together: shared encounters, shared kills, shared progress through a level.

Unofficial, unaffiliated with Sloclap, and experimental. The release includes a safe installer and can
be uninstalled by restoring its automatic backup or deleting the mod proxy files.

```
Sifu\Binaries\Win64\
    dsound.dll        <- the mod
    SifuCoop.ini      <- its configuration
```

**[SETUP.md](SETUP.md)** is the install and first-connection guide, and has a step-by-step
test list that isolates a failure to one layer. **[COOP-PLAN.md](COOP-PLAN.md)** is the
engineering account: what works, what is approximate, what is untried, and why.
**[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md)** explains the codebase, networking model,
Unreal integration, debugging workflow, and safe contribution process.

## Install

Extract the release and run SifuCoopInstaller.exe from that same folder. It detects Epic
and Steam installations, accepts a manually browsed Sifu folder, refuses to install while
the game is open, and copies an existing dsound.dll to a timestamped SifuCoop-backup folder
before replacement. Existing SifuCoop.ini settings stay intact unless you explicitly choose
to replace them.

For a manual install, copy dsound.dll and SifuCoop.ini next to Sifu-Win64-Shipping.exe.
See [SETUP.md](SETUP.md) for connection steps.

---

## What it does

| | |
|---|---|
| Both players: position, movement, attacks, health, guard, knockdown | yes |
| Enemy positions, health, guard and death | yes — the host's game decides |
| Enemy attacks on the joining screen | yes, though the exact strike may differ |
| Levels — the joiner follows the host automatically | yes |
| Shrines, upgrades, age, death counter, save progress | no — each player keeps their own |
| Bosses and cutscenes | untested; expect them to go their own way |
| More than two players | no |

## How it works

**The host's game is the authority for every enemy.** Their AI, position, health and death
all come from the host; the joiner's copies have their brains switched off and are driven
from what the host reports.

This is not a preference. Sifu's hitboxes, animation timings, parry windows and AI all live
inside the Windows executable, so only a running Sifu can decide whether a hit landed. A
separate server process could pass packets along but could never adjudicate a fight, and
there is no headless build to run one on. That constraint shapes everything else.

**Each player decides their own damage.** Nobody can tell you that you were hit, so a parry
is always judged on the machine where the button was pressed. Your own inputs are never
delayed and never mispredicted.

**The joiner's hits are reported to the host as running totals**, and the host echoes back
how much it has accounted for. Totals rather than deltas make the report idempotent: a lost
packet costs nothing because the next one carries the whole story, and a duplicate applies
nothing twice.

**Every packet is authenticated** with a truncated HMAC-SHA256 keyed by a passphrase both
players share. The protocol moves a player's position, tells the receiving game which level
to load, and applies damage — so an unsigned packet is a stranger with a hand inside your
session, and it is dropped before a single field is read.

## Building

Needs a Windows machine with Sifu installed, [WinLibs](https://winlibs.com/) g++ (`winget
install BrechtSanders.WinLibs.POSIX.UCRT`) and Python 3.

```bash
.\build.ps1              # regenerate offsets from the game's PDB, then build
.\build.ps1 -NoGen       # fast rebuild, skipping the PDB pass
.\build.ps1 -Deploy      # ...and copy into the game folder
```

The build compiles warning-clean under `-Wall -Wextra` and runs the crypto test vectors
before producing anything; a failing vector fails the build.

### How the offsets are found

Two mechanisms, both regenerated per game build:

- **Function addresses** come from the shipped PDB by mangled-name substring match.
- **Struct member offsets** come from Unreal's own generated reflection tables. Every
  reflected property records its byte offset inside the owning type, and the PDB names each
  table entry — so `tools/pdbdump` reads them straight out of the executable instead of
  guessing from hex dumps. This is how `m_fHealth`, `m_fCurrentGuard`, `m_eFaction` and the
  component pointers were located, exactly, in one pass.

To explore a type yourself:

```bash
python tools/pdbdump/structdump.py <Sifu-Win64-Shipping.exe> <Sifu-Win64-Shipping.pdb> UHealthComponent
```

### Supporting another store's build

Epic and Steam ship the same game version, so asset paths and combo indices match and the
two can play together — but the executables differ, so the address table is per-build. The
mod checks the running executable's fingerprint and refuses to hook anything it does not
recognise, rather than writing into unrelated code.

Adding one is a data change. On a machine with that build installed:

```bash
python tools/pdbdump/pdbdump.py <path-to.pdb> --exe <path-to.exe> --emit-build steam builds/steam.json
```

Commit the resulting JSON and rebuild; one DLL then serves both.

## What is not in this repository

No Sifu code, assets, level data or debug files. The mod requires a legitimately owned copy
of the game and redistributes nothing belonging to it.

`builds/*.json` holds function addresses and struct offsets measured from the executable.
Those are facts about a binary rather than a copy of one, and without them nobody could use
the mod without also owning the game's 970 MB of debug symbols. Bulkier extracts — the full
symbol dump, the level listing — are deliberately left out of version control and
regenerated locally in about two seconds; see `.gitignore`.

## Licence

MIT, see [LICENSE](LICENSE). Bundled Dear ImGui and MinHook keep their own; see
[THIRD_PARTY.md](THIRD_PARTY.md).
