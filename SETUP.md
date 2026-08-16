# SifuCoop — two players, one Sifu

Experimental co-op mod for Sifu. Two people fight the game's own enemies together: shared
encounters, shared kills, shared progress through a level.

Both players need the **same Sifu version**. The mod checks the executable's build
fingerprint and refuses to run against a different one rather than corrupting the game.

---

## 1. Install (both players)

**Recommended:** extract the release and run SifuCoopInstaller.exe from the extracted
folder. It auto-detects Epic and Steam installations, or lets you browse to the Sifu game
folder. It refuses to change files while Sifu is running. Any existing dsound.dll is backed
up to a date-and-time-named SifuCoop-backup subfolder before it is replaced; an existing SifuCoop.ini is kept
unless you select **Replace SifuCoop.ini**.

If auto-detection misses a custom library, choose the folder that contains Binaries
(normally the game's Sifu folder), not the executable itself.

### Manual install

Copy into `Sifu\Binaries\Win64\` — next to `Sifu-Win64-Shipping.exe`:

- `dsound.dll`
- `SifuCoop.ini`

A fresh game install normally has no local dsound.dll; if you already use another proxy DLL,
do not overwrite it manually. Use the installer so it makes a restorable backup. To
uninstall a manual installation, delete the two mod files. Epic's "verify files" leaves
additional mod files alone.

Run Sifu in **borderless**, not exclusive fullscreen, if you want the overlay to be clickable.

## 2. Network (both players)

There are three ways to reach each other. Try them in this order — the **Internet** tab in
the in-game menu (F1) repeats all of this with a button for the awkward part.

### Option 1 — a private network (recommended)

ZeroTier puts both machines on a private virtual network, so no router configuration is
needed and **nothing is exposed to the internet**. Free for personal use.

**Player A (host):**
1. Sign up at <https://my.zerotier.com>, click *Create A Network*, copy the 16-character ID.
2. Install ZeroTier, tray icon → *Join Network* → paste the ID.

**Player B:** install ZeroTier, join the same ID.

**Player A again:** on the ZeroTier web page, tick **Auth** next to each device. Until you do,
nothing connects. The page then shows each device's **Managed IP** (like `10.147.x.x`) — that
is the address to use, *not* your normal IP.

The host's log lists its own addresses at startup and flags the likely ZeroTier one:

    %LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log

### Option 2 — the host forwards a port

On the host's router, forward **UDP 7777** to the host's machine; the joiner then uses the
host's public address. Reliable, and it needs router access. **Set a passphrase before doing
this** — see below.

### Option 3 — hole punching, no router access needed

Both of you press **Find my public address** on the F1 → *Internet* tab and send each other
what it prints. Then in `SifuCoop.ini`, each of you sets the *other* person's address:

```ini
[net]
punch=203.0.113.9:7777   ; the OTHER player's public address
local_port=7777          ; both players set the same number
```

Connect at the same time. This works on most home routers and fails on some — if nothing
happens within a minute, fall back to option 1.

### Passphrase (all options)

Every packet is authenticated with a passphrase you both share. Anything that does not
verify is dropped before it is read.

```ini
[net]
passphrase=something-you-both-agree-on
```

Leaving it empty still works, and still authenticates — it just uses a key everybody else
also has. That is fine on a private VPN or LAN. It is **not** fine on a forwarded port,
because the protocol moves your character, applies damage, and tells your game which level to
load: an unauthenticated packet is a stranger with a hand inside your session.

A mismatched passphrase looks like nothing happening. The Lobby tab says so explicitly, and
the log counts what it rejected.

**Firewall:** the host needs inbound UDP allowed. Approve the Windows prompt on first launch.
If it is missed behind a fullscreen game, run this in an **admin** terminal:

```bash
netsh advfirewall firewall add rule name="SifuCoop" dir=in action=allow protocol=UDP localport=7777
```

## 3. Connect

Launch Sifu on both machines and press **F1**.

- Host: *Lobby* tab → **Host** → *Apply & (re)connect*.
- Joiner: *Lobby* tab → **Join**, type the host's `10.x` address → *Apply & (re)connect*.

Within a second or two both should show **CONNECTED** and a ping in milliseconds.

You can also edit `SifuCoop.ini` by hand, or use `SifuCoopLauncher.exe`:

```ini
[net]
mode=host                ; or: client
host=10.147.x.x          ; the HOST's address, client only
port=7777
passphrase=shared-secret ; must match on both machines
```

## 4. Play

The host loads a level — through Sifu's own menus, or from the *Levels* tab. The joiner is
pulled in automatically, and follows the host through every later level change too.

The other player's character appears on its own. Fight.

**F1** opens the menu at any time. The *Sync* tab shows every enemy currently being kept in
step, and the *Tuning* tab has a switch for each part of the machinery.

---

## How it works, in one screen

**The host's game is the authority for every enemy.** Their AI, position, health and death all
come from the host. The joiner's copies have their brains switched off and are driven from
what the host reports.

This is not a preference. Sifu's hitboxes, animation timings, parry windows and AI all live
inside the Windows executable, so only a running Sifu can decide whether a hit landed. A
separate server process could pass packets along but could never adjudicate a fight, and there
is no headless build to run one on.

**Each player decides their own damage.** Nobody can tell you that you were hit, so a parry is
always judged on the machine where the button was pressed — your inputs are never late and
never mispredicted.

**The joiner's hits are reported to the host as running totals.** The host applies them and
echoes back how much it has accounted for. Totals rather than deltas means a lost packet costs
nothing: the next one carries the whole story.

---

## What is and is not shared

| | |
|---|---|
| Both players: position, movement, attacks, health, guard, knockdown | **yes** |
| Enemy positions, health, guard, death | **yes** — the host decides |
| Enemy attacks on the joining screen | **yes**, but see below |
| Levels — the joiner follows the host automatically | **yes** |
| Shrines, upgrades, age, death counter, save progress | **no** — each player keeps their own |
| Bosses and cutscenes | **untested** — expect them to go their own way |
| More than two players | no |

**The enemy-attack caveat.** When an enemy swings on the host, the joiner's copy of that enemy
swings too, at the same moment. Which *exact* strike comes out may differ. Sifu chooses the
move inside its gameplay-ability system, above the entry point this mod can reach, so the two
screens agree that an attack happened and roughly when, but not always on precisely which one.
It is the difference between polish and playability, and this is the playable half.

**Priming.** Remote animations are rebuilt on top of an attack captured from your own
character, so **throw one punch after loading a level** and the remote player and enemies will
animate. Until you do, they move without swinging. The menu says so when it notices.

---

## Testing it, in order

Work down this list. Each step tells you something the next one depends on, so a failure high
up explains every failure below it.

### On one machine, no peer needed

1. **It loaded.** Launch Sifu, press **F1**. If the menu appears, the DLL is in and hooked.
   If it does not, read `SifuCoop.log` — a build-fingerprint mismatch is stated there in
   plain words.
2. **It can see the fight.** Load a level with enemies, open *Sync*. The table should list
   the enemies around you with live distances and health. If it is empty mid-fight, nothing
   downstream can work — send me the log.
3. **Remote animation works.** Throw a punch, press **F9** to spawn a practice character, then
   **F6**. It should attack. This is the exact path a peer's attacks take.
4. **Enemy animation works.** Press **F7** near an enemy. It should swing. This is the exact
   path the joiner uses for every enemy the host reports — the least proven part of the mod,
   and the one worth checking before you involve another person.
5. **Run-state reads work.** Once you are standing in a level, `SifuCoop.log` should print
   `run: local read ok -- age=N room-clear=..` once. This confirms the mod can read your age
   and room-clear progress (the values it shares with a peer). No peer required for this line.

### On one machine, with the bot

`tools\testclient.exe` is a real protocol peer — it authenticates like any other, so pass the
same passphrase the game is using (omit it if you have none set). Set the game to **Host**,
then:

```bash
tools\testclient.exe 127.0.0.1 7777 bot my-passphrase
```

5. **Connection and ping.** The Lobby should show CONNECTED with a ping. A second character
   appears, walks up to you, and attacks. Its health bar in the Lobby should be moving.
6. **The joiner can fight.** Now the important one — load a level with enemies and run:

   ```bash
   tools\testclient.exe 127.0.0.1 7777 damage my-passphrase
   ```

   It picks the enemy nearest you and reports damage on it. **In your game, that enemy should
   lose health and then die.** If it does, the whole client-hit path works, and a real second
   player will be able to fight. The console prints what it is doing.

   The bot also sends **run state** (age, room-clear %, a placeholder weapon) while connected,
   in every mode. Open **F1 → Sync/Tuning** and look under *Session*: `peer age`,
   `peer room-clear`, and `peer weapon` should populate and tick — synthetic values that move
   slowly on purpose. `SifuCoop.log` shows the same as `run: peer age=.. room=.. weapon=..`.
   This is how the run-state display is checked without a second Sifu.

7'. **Friendly fire (the experimental fix).** In `SifuCoop.ini` set `friendly_relationship=1`
   (or tick it in F1 → *Remote player's attacks*), load a level with at least two enemies,
   press **F9** to spawn a puppet, stand next to it and press **F6** to make it replay your
   last attack. If your health does **not** drop, Sifu's melee honours the relationship system
   and remote attacks can be made safe (then `echo_player_attacks=1`). If it still drops, that
   approach does not work and the log line `puppet: friendly relationship set ...` tells you it
   was at least applied. Set `friendly_relationship=0` again for normal play.

### With a real second player

7. Connect, get into the same level, and stand still. You should see each other move.
8. One of you attacks thin air. The other should see the swing.
9. Both attack the same enemy. It should die once, on both screens, at about the same time.
10. Let an enemy hit you. Your health drops on your screen, and the other player sees your
    character's health bar in their Lobby drop by the same amount.
11. Walk through a door / finish a room. The joiner should be pulled along.

---

## If the game crashes

First, work out whether it is this mod at all — rename `dsound.dll` to `dsound.dll.off` and
play. If it still crashes, it was never the mod, and you can rename it back.

If it *is* the mod, the most likely culprit is the F1 menu, because drawing it means hooking
the game's graphics pipeline alongside DLSS, the Epic overlay and whatever else is in there.
That part is purely cosmetic, so turn it off and keep playing:

```ini
[coop]
in_game_overlay=0
```

Everything else still works — the settings in this file, the hotkeys, and a small status
window beside the game instead of inside it.

Crash reports land in `%LOCALAPPDATA%\Sifu\Saved\Crashes\`. Sifu's own call stacks are
unsymbolised and usually name a module and nothing else, which is not enough to tell whose
fault a crash was. This tells you:

```bash
python tools/crashinfo/crashinfo.py "<crash folder>/UE4Minidump.dmp"
```

It prints the faulting address and which loaded module owns it, and says explicitly whether
that address is inside the mod.

## Answering "do enemy attacks actually hit me?" on one machine

The joining player's screen shows enemies swinging because the host said they swung. Whether
those swings *damage* you is the one thing a solo test cannot normally reach — but there is a
built-in harness for it. In `SifuCoop.ini`:

```ini
[coop]
selftest=1
```

Load a level and **walk into a fight**. Once you are standing among live enemies, the mod
teleports one next to you and makes it attack twice — first under its own AI (a baseline that
proves the setup is sound), then the way a joining player's screen drives it — and logs
whether your health dropped each time. The verdict is written to the log:

    %LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log   (search for "selftest")

If the driven round lands damage, co-op fights work as-is. If it does not, player damage
needs to become host-authoritative, and the log says so. Set `selftest=0` afterwards — it
moves enemies around and is not for normal play.

## When something is wrong

Open **F1 → Tuning**. Every part of the machinery has its own switch and a note on what turning
it off costs. Working down that list is the fastest way to find the guilty half:

- **Enemies teleport or jitter** → turn off *Drive enemies from the host*. If it stops, the
  problem is the position stream (check ping on the *Network* tab).
- **Enemies behave strangely, or the game hitches when they attack** → turn off *Replay the
  host's enemy attacks*. This is the least proven feature here.
- **Enemies die on one screen but not the other** → check *Sync* → `unmatched`. Anything above
  zero usually means you are not in the same level.
- **Nothing connects at all, no error** → almost always a mismatched passphrase. The Lobby tab
  shows a rejected-packet count when that is what is happening.
- **The joiner's hits do nothing** → check *Report my hits to the host* is on, and that *Sync*
  shows a rising "dealt ... reported".
- **The other player looks untouched no matter what** → *Show the peer's real health*.
- **Enemies stand around inert on the joining side** → *Hide enemies the host has not
  activated*, and check you are in the same level.

*Network* tab shows ping, jitter, packet loss and bandwidth. Occasional loss is normal and
invisible. Steady loss above a few percent looks like the other player stuttering and is worth
a look at the VPN rather than at these settings.

Full log: `%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log`. *Sync* → *Dump full roster to the log*
writes every tracked character, pooled ones included.

---

## Keys

| Key | Action |
|-----|--------|
| F1  | menu |
| F2  | bring your peer into the level you are in |
| F6  | make the remote character replay your last attack |
| F7  | make the nearest enemy replay your last attack |
| F8  | follow mode — the practice character mirrors your own movement |
| F9  | spawn the remote character by hand |
| F10 | despawn it |
| INSERT | dump every tracked character to the log |

F3–F5 are avoided: the Epic overlay owns F3.

---

## Known limits

- **Two players only.**
- **Exact strikes may differ** on the receiving screen (see above).
- **Progression is not shared.** Age, deaths, shrines and unlocks stay per player.
- **Bosses and cutscenes are untested** and heavily scripted; expect them to desynchronise.
- **Both players must own the same Sifu build.** Epic and Steam ship the same game version and
  can play together, but the address table is per-executable, so a build the mod has never seen
  makes it refuse to run. Adding one is a data change — see `tools/pdbdump`.
