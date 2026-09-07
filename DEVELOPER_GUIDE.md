# SifuCoop Developer Guide

This is the practical guide for understanding, changing, and debugging SifuCoop. It assumes you know basic C and a little C++, but not Unreal Engine internals, Windows DLL injection, or network replication.

Read this before making gameplay changes. The mod makes two independent Sifu games look and behave as if they share a world. It is not Unreal multiplayer in the usual sense. That difference explains most of the code and most of the bugs.

## 1. The one-sentence model

Each player runs a normal single-player copy of Sifu. `dsound.dll` is loaded by each copy at startup, hooks selected engine/game functions, exchanges custom UDP packets with the other copy, and applies received state to a locally spawned remote-player puppet and locally existing enemies.

The two games do not share memory, a server, physics, AI, hitboxes, or a UE4 `UNetDriver`.

The code therefore has two jobs:

1. Decide which machine is allowed to decide a piece of gameplay.
2. Make the other machine reproduce the result convincingly and safely.

If both copies decide an enemy's AI, both apply damage, or both move an actor, the result is jitter, attacks aimed at nobody, missed hits, bad deaths, and divergent health.

## 2. What SifuCoop is and is not

### It is

- A 64-bit Windows DLL loaded as a DirectSound proxy.
- C++17, built with MinGW `g++`.
- A raw WinSock2 UDP protocol, not ENet, Steam Networking, EOS, or UE replication.
- A UE4.26 reverse-engineering project using PDB-derived symbols and Blueprint reflection.
- A two-machine mod, normally connected through ZeroTier or another VPN.

### It is not

- A dedicated server.
- A listen-server mod.
- Authoritative engine multiplayer.
- Safe to test with two Sifu processes on one PC.
- A project where it is okay to guess native function addresses or object layouts.

The shipping Sifu executable has the server half of UE networking compiled out. The obvious engine routes for creating a listen server and replicating actors are empty in this build. The custom UDP mirror is therefore the core design, not a temporary shortcut.

## 3. Repository map

Start with these files in this order:

| File or folder | Purpose |
| --- | --- |
| `README.md` | Player-facing overview. |
| `SETUP.md` | Installation and connection setup. |
| `AI-HANDOFF.md` | Current known facts, sharp edges, research results, and diagnostic names. |
| `HANDOFF.md` | Older, much more exhaustive development history. Verify protocol/version facts against source. |
| `BUGS-ENEMY-SYNC.md` | Focused notes about enemy synchronization failures. |
| `build.ps1` | Build, test, packaging, and optional deployment pipeline. |
| `src/dllmain.cpp` | Startup, build guard, initialization order, and hook installation. |
| `src/net/protocol.h` | Exact wire format. Read this before changing networking. |
| `src/net/session.cpp` | UDP socket, handshake, validation, queues, snapshots, interpolation. |
| `src/game/puppet.cpp` | Remote-player clone, transforms, animations, vitals, visual state. |
| `src/game/enemies.cpp` | Enemy census, ownership, damage ledger, death/down state, transforms. |
| `src/game/orders.cpp` | Sifu attack/order hook and replay/capture logic. |
| `src/game/actors.cpp` | Shared actor helpers: health, guard, relationships, motion, calls. |
| `src/game/runstate.cpp` | Age, outfit, and weapon synchronization. |
| `src/ue/reflection.cpp` | UE `UObject` lookup and `ProcessEvent` calls. |
| `src/core/offsets.g.h` | Generated address table. Never hand-edit it. |
| `tools/pdbdump/` | Extracts PDB information and generates the offsets table. |
| `third_party/` | Vendored ImGui and MinHook. Do not casually modify it. |

Read source in this order:

```text
dllmain.cpp
  -> coop.cpp
  -> net/session.cpp + net/protocol.h
  -> game/puppet.cpp
  -> game/enemies.cpp
  -> game/orders.cpp
  -> ue/reflection.cpp
```

## 4. Startup: how the DLL enters the game

Sifu loads `dsound.dll` from its own `Sifu\Binaries\Win64` directory before it searches the Windows system copy. `src/proxy/dsound_proxy.cpp` forwards real DirectSound exports to the system DLL so audio continues working. At the same time, `DllMain` starts SifuCoop.

Do not perform heavy work directly inside `DllMain`; Windows holds the loader lock there. SifuCoop opens its log then starts a bootstrap thread.

The bootstrap sequence in `src/dllmain.cpp` is deliberately defensive:

1. Create a process-level mutex. A second SifuCoop game process on the same PC is refused.
2. Read the PE header of `Sifu-Win64-Shipping.exe`.
3. Match its timestamp and image size against `offsets.g.h`.
4. Wait for `GEngine` and allow startup asset loading to settle.
5. Initialize the reflection bridge.
6. Load `SifuCoop.ini` and start networking.
7. Initialize actors, puppet, player-2 experiment, enemies, order hook, and self-test.
8. Start the overlay.
9. Install the tick hook.

If a supported-build check, reflection initialization, socket startup, or tick hook fails, the mod should become inert rather than calling a bad pointer. When the game suddenly crashes after an update, suspect the build/offset guard first.

## 5. Native calls, offsets, and reflection

There are two ways this code talks to Sifu.

### 5.1 PDB-resolved native calls

Some work needs a genuine C++ function address: engine ticking, actor spawning, actor movement, and other non-Blueprint APIs. An address is not universal: Epic and Steam executables can place the same function at different RVAs.

`tools/pdbdump/pdbdump.py` reads the PDB, saves per-build information in `builds/*.json`, and emits `src/core/offsets.g.h`. At runtime, the mod identifies the executable build and chooses the matching table.

Never type an address into `offsets.g.h`. Never use an Epic address on Steam. Never assume a successful compile means an address is correct. A bad native call can corrupt state or crash much later than the call that caused it.

When adding a native symbol or member offset:

1. Add the desired symbol/member to the pdbdump inputs.
2. Regenerate for the Epic game folder.
3. Regenerate for the Steam game folder.
4. Inspect the generated table for zero values in both builds.
5. Rebuild and deploy a DLL matched to each installation.
6. Confirm the startup log says `guard: build 'epic' matched` or `guard: build 'steam' matched`.

The important trap: generating a new table for only one build can make a feature silently do nothing on the other machine because its symbol value is zero.

### 5.2 Blueprint reflection

Many gameplay actions are Blueprint-exposed. `src/ue/reflection.cpp` locates `UObject`, `UClass`, and `UFunction` objects and calls Blueprint functions through UE4's `ProcessEvent` mechanism.

Reflection is generally safer than inventing a native class layout, but it still has rules:

- An object pointer can become invalid after map travel, death, destruction, or garbage collection.
- Function and asset paths must match the game build.
- Parameters must have the correct layout, alignment, and lifetime.
- A function existing in the PDB does not prove it has the expected behavior in live combat.

Use helpers from `actors.cpp` and `reflection.cpp` before inventing new ones. Validate actor, world, function, and return state. Log both request and observed result while testing.

## 6. The custom UDP network layer

Networking is in `src/net/session.cpp`; packet definitions are in `src/net/protocol.h`. It uses non-blocking WinSock2 UDP:

```text
WSAStartup
socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
ioctlsocket(..., FIONBIO)
bind
sendto / recvfrom
closesocket / WSACleanup
```

The default port is `7777`. ZeroTier is not embedded in the DLL; it only creates a private network interface. SifuCoop sends normal UDP packets to the peer's ZeroTier address.

### 6.1 Connection roles

- **Host** binds the configured UDP port and is authoritative for canonical enemy health, damage, and death.
- **Client** sends to the host and supplies its local player and selected locally owned enemy state.
- **Offline** is used when networking is disabled or unavailable.

The connection begins with `Hello` and `Welcome` packets carrying session nonces. The packet header includes a magic number, protocol version, packet type, sequence number, send time, and an 8-byte authentication tag. The protocol version is `kProtocolVersion` in `protocol.h`; both DLLs must match it. A passphrase is used with HMAC-SHA256 to authenticate packets, with a truncated tag in the header. This provides authentication, not encryption: do not put secrets in packets.

### 6.2 Packet families

| Packet | What it carries |
| --- | --- |
| `Hello`, `Welcome`, `Goodbye` | Session establishment and exit. |
| `Snapshot` | Player position, rotation, velocity, health, guard, down/in-level flags. |
| `OrderEvent` | Captured Sifu combat-order metadata. |
| `LevelSync`, `InviteReply` | Map synchronization and join flow. |
| `EnemyState` | Batched enemy position, velocity, vitals, damage totals, death/down/target flags. |
| `EnemyDamage` | Damage ledger reports. |
| `OwnedEnemy` | Transform updates from the machine that temporarily owns enemy combat. |
| `Ping`, `Pong` | RTT measurement for diagnostics and interpolation. |
| `RunState` | Age, outfit, and held-weapon state. |
| `MontageState` | Montage/animation/reaction playback information. |
| `CheatState` | Host-authoritative modifier state. |

Large enemy sets are chunked. Packets have a fixed maximum size and the protocol avoids unbounded allocations inside the game.

### 6.3 UDP implications

UDP can lose, duplicate, reorder, or delay packets. Treat it as an unreliable stream of snapshots, not a transaction system.

Good UDP data:

- Latest transform and velocity.
- Health/guard repeatedly resent in state packets.
- Periodic run-state or cheat snapshots.
- A visual hint that can be replaced by later state.

Dangerous unless you add acknowledgement/retry/idempotence:

- “Deal exactly 30 damage once.”
- “This enemy is permanently dead.”
- Level-changing operations.
- One-time revive or weapon pickup.
- Any event that must never be missed.

For every new message ask: if it is lost, does the next snapshot repair state? If not, make it idempotent and resend/acknowledge it, or represent the durable outcome in the authoritative snapshot/ledger.

## 7. Authority: the main design rule

Both games still simulate a full local Sifu level. Both have their own enemy controllers, target selectors, collision, animation graph, health component, and combat director. Sending only a position cannot make them agree about why an actor is there or what it may do next.

SifuCoop therefore assigns authority by feature.

| Feature | Intended authority | Observer's job |
| --- | --- | --- |
| Local player input | That player's local machine | Spawn/update a puppet. |
| Enemy health/damage/death | Host | Apply host ledger and terminal state. |
| Enemy action decisions | Exactly one owner | Suppress or park competing local AI. |
| Enemy transform | Current owner | Interpolate/apply received transform. |
| Player visuals | Owning player | Replay safe visual state on puppet. |
| Cheats/modifiers | Host | Receive and apply host snapshot. |

`peer_fights_locally` can assign nearby enemies to the joining machine. It must also stop competing host AI and publish the owned enemy transform back to host. Ownership is a state transition; do not flip it carelessly every frame.

The central invariant is:

```text
For each enemy, at each moment:
- one authoritative health/death ledger;
- one authority for action decisions;
- one writer of authoritative transform;
- zero or more observers.
```

Breaking the first causes double/missing damage. Breaking the second produces attacks at nobody. Breaking the third causes jitter and teleports. Observers must not secretly restart brains, apply local damage, or force locomotion after an authority sends a terminal state.

## 8. Player puppets

`puppet.cpp` creates a local visual stand-in for the remote player. It is not the peer's actual pawn. It has a local actor, physics state, animation graph, visual age/outfit, and potential AI interactions.

Typical flow:

1. The local tick gathers `LocalState` from the actual player.
2. `net::TickSession` sends state at the configured snapshot rate.
3. The receiver stores recent peer snapshots.
4. The receiver interpolates compatible snapshots.
5. Puppet code applies transform/velocity and selected vitals/animation data.
6. Run-state code applies peer cosmetic state where supported.

Do not confuse puppet health with remote true health. Mirrored health for UI or visuals must not become a second source of combat damage. Remote player attack replay is similarly dangerous: a local replayed hitbox can hurt the observer even though the real attack already resolved on the sender's game. That is why player attack replay is normally off in co-op.

| Symptom | Likely cause |
| --- | --- |
| Vertical falling frame at spawn | Puppet was created before a valid peer transform/velocity existed. |
| Jitter while running | Local movement and network movement both write the puppet, or interpolation is wrong. |
| Wrong age/outfit | Puppet inherited local appearance; run-state update/refresh failed. |
| Attack phases through | It is visual replay; real hit detection happened on another machine. |
| Bad state after map travel | An old world/actor pointer was retained instead of rebuilding state. |

## 9. Enemy synchronization and death

`enemies.cpp` is the most sensitive subsystem. It tracks enemy actors, builds network state, receives remote state, records damage totals, and tries to keep AI, position, vitals, and death consistent.

An enemy snapshot includes identity hash, transform, velocity, health/max health, guard, accumulated damage, time dilation, and flags. Flags include active, down, target host, target peer, dead, and reaction-motion state.

### Death must be terminal

The “dead enemy standing up” problem is not merely visual. It means a local system still believes the actor may stand, move, target, deal damage, or select an order.

When authoritative host state says an enemy is dead, observer handling must:

1. Accept host health/death as newer than local opinion.
2. Stop local AI/brain/order generation for that actor.
3. Prevent local retargeting and hit interaction that resumes combat behavior.
4. Place it in the correct down/death state or request matching death animation.
5. Reject stale transform/action updates that would wake it.
6. Keep terminal state until an actual level reset/respawn generation says otherwise.

If you only play a fall montage, local health/AI can stand it back up. If you only set health to zero, the other screen can show a standing corpse. A good fix synchronizes semantic state and makes every competing subsystem respect it.

### Damage must be idempotent

Both players can hit one enemy. Packets can duplicate or arrive late. Never simply add every received damage event to health.

The existing approach uses cumulative damage/guard totals. Compare received total with last applied total and apply only the difference. Repeated packets become harmless and a later snapshot repairs a lost packet.

When changing this code, preserve:

- Never apply the same damage twice.
- Never let observer-calculated health overwrite host authority.
- Clamp and validate values before applying.
- Reset ledgers only for a verified new enemy generation/map lifecycle.
- Log identity, prior total, received total, delta, health, down, and dead state.

## 10. Orders, reactions, and animations

Sifu combat is not just “play animation X.” An order carries attack selection, combo depth, state gates, target, movement, hit timing, and AI decisions. `orders.cpp` hooks the order path to observe/replay selected orders.

Keep these separate:

- **Gameplay order:** can create hitboxes, movement, damage, and state changes.
- **Cosmetic replay:** only makes an observer see an action.
- **Reaction/death state:** must agree with authoritative health and AI state.

Do not replay arbitrary remote player attacks as live local attacks in co-op. It can create a second hitbox that hurts the wrong local actor. Enemy replay must be constrained by ownership and target. If observer AI is not suppressed, it can issue a second incompatible order.

Animation diagnosis order:

1. Did authority record the attack/reaction?
2. Was the packet sent, authenticated, and received?
3. Did receiver identify the same actor?
4. Was the event rejected as stale/incompatible?
5. Did local locomotion/AI immediately replace the replay?
6. Is the semantic state normal hit, launch, fall, down, death, or revive?

Do not trust a montage path alone. Some cached pose assets are `UPoseAsset`, not animation sequences, and cannot be fed into a montage slot. Type-gate received assets and retain fallback behavior.

## 11. Configuration and feature flags

`SifuCoop.ini` sits next to deployed DLL. `coop.cpp` loads it with Windows INI APIs. The F1 menu exposes many options and can write the file.

| Setting | Use when diagnosing |
| --- | --- |
| `sync_enemies` | Turn broad enemy sync on/off. |
| `suppress_client_ai` | Find out whether observer AI fights replication. |
| `sync_enemy_vitals` | Isolate health/guard/death sync. |
| `echo_enemy_attacks` | Isolate enemy attack replay. |
| `report_damage` | Test joiner-to-host damage. |
| `peer_fights_locally` | Test ownership handoff. |
| `client_simulates_enemies` | Prevent/allow client enemy simulation. |
| `adaptive_interp`, `interp_delay_ms` | Diagnose movement smoothing. |
| `sync_montages`, `mirror_hit_reactions` | Diagnose visual combat reactions. |
| `sync_run_state`, `sync_peer_age`, `sync_peer_visual_age` | Diagnose age/outfit/weapon sync. |
| `verbose_enemies`, `verbose_orders` | Increase relevant logs. |

Use flags to isolate one subsystem, not to hide failures. Change one group at a time and record the configuration on both machines. Some experimental features disable themselves in `coop.cpp` because they are unsafe; do not re-enable them without a focused test plan.

## 12. Build, test, and deploy safely

### Prerequisites

- Supported Epic/Steam Sifu builds plus matching PDBs when regenerating offsets.
- Python on `PATH` for PDB tooling.
- WinLibs/MinGW `g++` with C++17 support.
- Two real machines for meaningful co-op tests.
- ZeroTier/private LAN for easiest connectivity.

### Normal local build

```powershell
cd C:\Users\zanci\SifuCoop
.\build.ps1 -NoGen
```

`-NoGen` uses existing generated address tables. It first compiles/runs crypto test vectors, then builds proxy DLL, test client, launcher, installer, and distribution files. Inspect `build\build.log` on compile failure.

### Regenerate offsets

Do this only after a game update, when adding a native symbol/member, or when logs report an unknown build/missing offsets.

```powershell
.\build.ps1 -GameDir "C:\Program Files\Epic Games\Sifu\Sifu\Binaries\Win64" -LocalBuildName epic
.\build.ps1 -GameDir "Z:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam
```

Then build/deploy both matched versions. Do not mix generated tables or DLLs between game builds.

### Deployment

Close Sifu first. The target is the directory containing `Sifu-Win64-Shipping.exe`:

```powershell
.\build.ps1 -NoGen -Deploy
.\build.ps1 -NoGen -GameDir "Z:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam -Deploy
```

Before overwriting working DLLs, create date-stamped backups. After deployment, verify both logs show expected build name, both DLLs have matching protocol version, both INIs use intended passphrase/settings, and no second local SifuCoop process exists.

### Unit-like checks versus real tests

Crypto vectors and testclient prove packet/authentication logic, DLL export loading, and some protocol behavior. They cannot prove Sifu applies a hit reaction, maintains a corpse, or targets a puppet correctly. That needs a two-machine game test.

## 13. Logs and crash reports

Primary log:

```text
%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log
```

Find the latest `=== SifuCoop attached` line and read forward. Logs append across sessions, so old sessions can mislead you.

| Prefix | Meaning |
| --- | --- |
| `guard:` | Executable-build checks and hook safety. |
| `net:` | Socket, peer address, authentication, session lifecycle. |
| `coop:` | Network heartbeat, RTT, counts, packet rates. |
| `esync:` | Enemy health/damage/down/dead/ownership state. |
| `death:` | Death transitions and animation attempts. |
| `targets:` | Which player enemies target. |
| `roles:` | Combat role census. |
| `orders:` | Captured/replayed combat-order information. |
| `reaction:` | Hit-reaction capture/replay attempts. |

For crashes, inspect newest UE crash report's `CrashContext.runtime-xml` and call stack before changing code. A named crashing function is better evidence than any visual symptom.

## 14. Disciplined bug-fixing workflow

### Step 1: state the bug as a precise mismatch

Bad: “enemy sync is broken.”

Good: “Host kills enemy A with a takedown. Client receives `dead=1`, but after the next local AI tick the enemy stands and attacks the puppet. Host still shows it dead.”

Record host/client action, map/enemy type, trigger, each screen's result, whether health truly differs or visuals alone differ, repeatability, and paired logs.

### Step 2: choose owner and invariant

For the example: host owns enemy health/death. Expected invariant: once host reports dead, client cannot issue or execute combat actions for that enemy until reset.

### Step 3: instrument before changing behavior

Add narrow transition logs, not per-frame spam. Include actor/source hash, generation, state before/after, owner, time, and reason for rejecting packet/state.

### Step 4: isolate with flags

For corpse failures test in this order:

1. `sync_enemies=1`, `echo_enemy_attacks=0`.
2. Enable vitals/death sync only.
3. Suppress observer AI.
4. Enable visual death/reaction replay.
5. Re-enable attack echo and peer-local ownership one at a time.

The first setting that reintroduces failure identifies the subsystem to inspect.

### Step 5: make the smallest authority-preserving change

Fix the transition that violates the invariant. Do not cover it with timers, constant teleports, or replaying an animation every tick. Those make packet loss and frame rate decide gameplay.

### Step 6: test both directions

Every co-op fix needs host-causes/client-observes, client-causes/host-observes, both players affecting the same enemy, player death/revive during the condition, and map reload after it.

### Step 7: preserve evidence

Keep before/after logs, config, map, and build hashes in a bug note or commit message. Otherwise later work repeats the same theory.

## 15. Recipes for common changes

### Add a repeating state field

For example, a player cosmetic value:

1. Decide authority and whether later snapshots repair packet loss.
2. Add a fixed-size field to a packed packet in `protocol.h`.
3. Bump `kProtocolVersion`.
4. Populate it on sender.
5. Validate/store it on receiver.
6. Apply it in puppet/run-state at a safe lifecycle point.
7. Add default/reset behavior for disconnect and map travel.
8. Deploy both sides together and test valid/missing/old/rapidly changing values.

Never send raw pointers, UE object addresses, STL objects, or C++ class layouts over the network.

### Add a one-time event

For revive confirmation or permanent death, do not use one fire-and-forget UDP command. Give it an ID/generation, make it idempotent, store durable result in later snapshots, and retry/acknowledge it if it truly must arrive.

### Add a native engine call

1. Confirm Blueprint/reflection cannot do it.
2. Locate the exact symbol in matching PDB.
3. Add it to generator inputs and regenerate both builds.
4. Define calling convention and parameter/return types exactly.
5. Add null/build checks and logging.
6. Call only from a known-safe game-thread hook, never network receive path.
7. Test separately on Epic and Steam.

### Add/change animation replay

Identify whether it is montage, sequence, or pose asset. Record semantic intent: generic, attack, reaction, fall, or death. Do not use visual replay as damage/death truth. Ensure local AI/locomotion cannot immediately overwrite it.

### Add a config option

1. Add field/default in `Config` in `coop.h`.
2. Load it in `coop.cpp`.
3. Save it in `coop.cpp`.
4. Add menu UI after behavior works through INI.
5. Document safe values/default.
6. If networking changes, make both peers compatible.

## 16. C/C++ practices used here

The project intentionally uses little complex STL. Most runtime data is fixed-size structs, C arrays, `char` buffers, `memcpy`, and explicit sizes. The notable `std` synchronization in first-party code is an `std::atomic<bool>` for overlay menu-open state.

This conservatism is intentional:

- Code runs inside a proprietary game process.
- Hooks should avoid unpredictable allocation, locks, exceptions, and long work.
- Packet structs need exact layouts.
- Fixed maxima are easier to validate and log.

Good habits:

- Initialize structs with `{}`.
- Pass output buffer size with every `char*` buffer.
- Use `std::uint32_t`/`std::int32_t` on the wire, not `int`.
- Check pointer, actor, and world validity around lifecycle boundaries.
- Treat casts as suspicious until PDB/reflection evidence supports them.
- Never let exceptions cross a game hook or DLL boundary.
- Keep per-tick work bounded: no blocking socket calls, disk I/O, or expensive object searches each frame.

## 17. Recommended learning plan

Do not begin by trying to fix every enemy bug. Build confidence in layers.

1. Build with `-NoGen`, launch one host/client pair over ZeroTier, open F1 menu, and locate both logs.
2. Read `dllmain.cpp`, `protocol.h`, and public APIs in `session.h`.
3. Reproduce one enemy-death mismatch and identify actor hash, death flag, and owner in both logs.
4. Make one diagnostic-only change around that transition; build/deploy and compare evidence.
5. Fix one invariant, for example refusing AI restart for host-terminal-dead enemy.
6. Test both directions. If only one direction works, inspect offset generation/deployment before rewriting gameplay.
7. Keep new features behind config flags until they survive repeated real-machine tests.

You do not need to be an Unreal expert to contribute. You do need to be systematic. A reproducible test case with paired logs and a small instrumented patch is more valuable than a large speculative rewrite.

## 18. Final shipping checklist

- [ ] Sifu closed before DLL replacement.
- [ ] Previous DLL backed up.
- [ ] Epic and Steam builds have complete nonzero offset tables.
- [ ] Both DLLs use same protocol version.
- [ ] Crypto test vectors passed.
- [ ] DLL proxy exports load successfully.
- [ ] Both startup logs report expected build.
- [ ] Both machines use matching passphrase/port and intended config.
- [ ] Feature tested in both directions.
- [ ] Death, revive, map travel, and disconnect tested if feature touches actors/combat.
- [ ] Logs from final test saved.

For deeper historical context and facts that should not be rediscovered, continue with `AI-HANDOFF.md` after this guide.
