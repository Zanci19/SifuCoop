"""Extract symbol RVAs from Sifu's shipped PDB.

Usage:
    python pdbdump.py <path-to.pdb> --probe
    python pdbdump.py <path-to.pdb> --find SpawnActor UWorld
    python pdbdump.py <path-to.pdb> --gen ../../src/core/offsets.g.h
"""

import argparse
import glob
import json
import os
import re
import struct
import sys
import time

from pdbfile import PdbFile, S_PUB32

# Symbols the mod needs, keyed by the C++ identifier we will expose.
# Each entry is a list of substrings that must ALL appear in the mangled name;
# MSVC mangling keeps identifiers readable, so substring matching is reliable.
WANTED = {
    "UGameEngine_Tick": ["?Tick@UGameEngine@@"],
    # This is the real physical-input boundary. Filtering controller 1 here
    # prevents one keyboard/gamepad from driving both local players without
    # blocking direct network movement on the remote-player actor.
    "APlayerController_InputKey": ["?InputKey@APlayerController@@"],
    # Sifu's targeting-reticle widget component. Creating a second local player
    # crashes inside this during the new controller's BeginPlay: it tries to add
    # a HUD widget to a player screen that the second player does not have, and
    # dereferences null. It is suppressed for the duration of the CreatePlayer
    # call -- a remote player has no use for a reticle on this machine's HUD.
    # Sifu's own player controller. Its BeginPlay is deferred while a second
    # player is being created -- see player2.cpp for why the engine runs it too
    # early to be survivable.
    "ASCPlayerController_BeginPlay": ["?BeginPlay@ASCPlayerController@@"],
    "UWidgetPoolComponent_BeginPlay": ["?BeginPlay@UWidgetPoolComponent@@"],
    # Sifu's HUD widget registers itself with the controller through this
    # virtual. Declining it for the SECOND player's controller is what stops the
    # remote player's health bar being drawn over the local player's own, which
    # is unavoidable once splitscreen is force-disabled and both local players
    # share one full-screen viewport.
    "AFightingPlayerController_BPF_SetHUD": ["?BPF_SetHUD@AFightingPlayerController@@UEAA"],
    "UTargetableWidgetUpdaterComponent_BeginPlay": [
        "?BeginPlay@UTargetableWidgetUpdaterComponent@@"
    ],
    "UWorld_SpawnActor": ["?SpawnActor@UWorld@@"],
    "AActor_SetActorLocationAndRotation": ["?SetActorLocationAndRotation@AActor@@"],
    # Native steering route for replicated characters. Sifu's synthetic second
    # player accepts AddMovementInput but does not consume it; direct movement
    # is consumed by the character movement component and drives locomotion.
    "ACharacter_GetMovementComponent": [
        "?GetMovementComponent@ACharacter@@UEBAPEAVUPawnMovementComponent@@XZ"
    ],
    "UCharacterMovementComponent_RequestDirectMove": [
        "?RequestDirectMove@UCharacterMovementComponent@@UEAAXAEBUFVector@@_N@Z"
    ],
    "AActor_K2_DestroyActor": ["?K2_DestroyActor@AActor@@"],
    "UEngine_GetWorldFromContextObject": ["?GetWorldFromContextObject@UEngine@@"],
    "GEngine": ["?GEngine@@"],
    "GWorld": ["?GWorld@@"],
    # Reflection layer. These let us call any Blueprint-exposed UFunction by
    # name, so the mod never has to hardcode C++ struct field offsets -- which
    # is both the fragile part of this kind of work and the part a game patch
    # silently breaks.
    "UObject_ProcessEvent": ["?ProcessEvent@UObject@@"],
    "UObject_FindFunction": ["?FindFunction@UObject@@"],
    "FName_FromWide": ["??0FName@@QEAA@PEB_WW4EFindName@@@Z"],
    "StaticFindObjectSafe": ["?StaticFindObjectSafe@@"],
    # The out-parameter overload specifically: the FString-returning one would
    # need a by-value return convention, which is what crashed us via GetType.
    "UObjectBaseUtility_GetPathName": [
        "?GetPathName@UObjectBaseUtility@@QEBAXPEBVUObject@@AEAVFString@@@Z"
    ],
    # Game-side accessors we can call directly.
    "UGameplayStatics_GetPlayerCharacter": ["?GetPlayerCharacter@UGameplayStatics@@"],
    "UGameplayStatics_OpenLevel": ["?OpenLevel@UGameplayStatics@@SAXPEBVUObject@@VFName@@_NVFString@@@Z"],
    # Phase A: enemy enumeration and AI suppression.
    "UGameplayStatics_GetAllActorsOfClass": ["?GetAllActorsOfClass@UGameplayStatics@@SA"],
    "AFightingCharacter_StaticClass": ["?StaticClass@AFightingCharacter@@SA"],
    "UBrainComponent_StaticClass": ["?StaticClass@UBrainComponent@@SA"],
    "USceneComponent_K2_GetComponentLocation": ["?K2_GetComponentLocation@USceneComponent@@Q"],
    "UReplaySystem_GetPlayingPlayerCharacter": [
        "?BPF_GetPlayingPlayerCharacter@UReplaySystem@@SA"
    ],
    # M2. The FVector/FRotator overload is named exactly: the FTransform one
    # would drag in that struct's 16-byte alignment rules for no benefit.
    "UWorld_SpawnActor_VecRot": [
        "?SpawnActor@UWorld@@QEAAPEAVAActor@@PEAVUClass@@PEBUFVector@@PEBUFRotator@@"
        "AEBUFActorSpawnParameters@@@Z"
    ],
    "ASCCharacterImpostor_StaticClass": ["?StaticClass@ASCCharacterImpostor@@SAPEAVUClass@@XZ"],
    "ASCCharacterImpostor_BPE_MimicCharacter": ["?BPE_MimicCharacter@ASCCharacterImpostor@@QEAA"],
    "AFightingCharacter_BPF_SetFaction": ["?BPF_SetFaction@AFightingCharacter@@QEAA"],
    "AFightingCharacter_BPF_SetInvincibility": [
        "?BPF_SetInvincibility@AFightingCharacter@@QEAA"
    ],
    "AFightingCharacter_GetFaction": ["?GetFaction_Implementation@AFightingCharacter@@UEBA"],
    # M4 animation.
    "USkeletalMeshComponent_StaticClass": ["?StaticClass@USkeletalMeshComponent@@SA"],
    "UAnimInstance_Montage_GetPosition": ["?Montage_GetPosition@UAnimInstance@@QEBA"],
    "UAnimInstance_Montage_Play": ["?Montage_Play@UAnimInstance@@QEAA"],
    # Orders: Sifu's real combat/animation currency. Attacks, hit reactions,
    # traversal and animation are all Orders, they serialise to a buffer, and
    # the engine already has a multicast RPC to replay them elsewhere.
    "ABaseCharacter_OnLocalPlayOrder": ["?OnLocalPlayOrder@ABaseCharacter@@UEAA"],
    "AFightingCharacter_OnLocalPlayOrder": ["?OnLocalPlayOrder@AFightingCharacter@@UEAA"],
    # The base class vtable itself, so the virtual's slot index can be derived
    # rather than guessed -- subclasses override the entry but never move it.
    "ABaseCharacter_vftable": ["??_7ABaseCharacter@@6B@"],
    "ABaseCharacter_PlayOrder": ["?PlayOrder@ABaseCharacter@@QEAA"],
    "AFightingCharacter_PlayOrder": ["?PlayOrder@AFightingCharacter@@QEAA"],
    "OrderBase_SaveTo": ["?SaveTo@OrderBase@@QEAA"],
    "OrderBase_GetType": ["?GetType@OrderBase@@QEBA"],
    "UOrderComponent_PlayOrder": ["?PlayOrder@UOrderComponent@@QEAA"],
    "UAttackComponent_PrepareToLaunchAttack": ["?PrepareToLaunchAttack@UAttackComponent@@QEAA"],
    "UAttackComponent_StaticClass": ["?StaticClass@UAttackComponent@@SA"],
    # The actual "perform this attack" call, downstream of move selection.
    "UAttackComponent_LaunchAttack": ["?LaunchAttack@UAttackComponent@@AEAA"],
    "OrderAttack_GetAnimPlayed": ["?GetAnimPlayed@OrderAttack@@UEBAPEAVUAnimSequence@@XZ"],
    "OrderAttack_OnStart": ["?OnStart@OrderAttack@@UEAAXXZ"],
    "UPlayerAnim_NativeUpdateAnimation": ["?NativeUpdateAnimation@UPlayerAnim@@EEAAXM@Z"],
    # Exact locomotion-state setter. FSpeedState is five bytes (four booleans
    # plus ESpeedState); writing only the booleans left the graph's enum at V0.
    "UPlayerAnim_BPF_SetSpeedState": ["?BPF_SetSpeedState@UPlayerAnim@@QEAAXW4ESpeedState@@@Z"],
    # ...but the anim instance only holds a COPY. The speed state belongs to the
    # movement component, which computes it during its own tick from input a
    # replicated body does not have -- so it read V0 at every speed, and writing
    # the anim's copy was writing a mirror. This is the source of the value.
    "UFightingMovementComponent_SetSpeedState": [
        "?SetSpeedState@UFightingMovementComponent@@UEAAXE@Z"
    ],
    # Sifu allocates the RIGHT to attack centrally, per target, through combat
    # role tickets -- which is why only one or two enemies swing at you at a
    # time. An actor with no ticket manager gets only IndirectOpponents, who
    # circle and deflect and never commit. Needed to reach the AI's own
    # BPF_ForceEnemy / BPF_GetCurrentCombatRole via GetComponentByClass.
    "UAIFightingComponent_StaticClass": ["?StaticClass@UAIFightingComponent@@SA"],
    # A spawned player-class clone runs BeginPlay, but is not guaranteed to be
    # in Sifu's global target registry. AI selection only considers registered
    # UTargetableActorComponents.
    "UTargetableActorHelper_GetTargetableActorComponent": [
        "?GetTargetableActorComponent@UTargetableActorHelper@@"
    ],
    "USCActorManager_RegisterTargetableActor": [
        "?RegisterTargetableActor@USCActorManager@@SAX"
    ],
    # Host-side aggro handoff when damage arrived from the joining player.
    "UAttackComponent_SetTarget": ["?SetTarget@UAttackComponent@@QEAAXPEAVAActor@@@Z"],
    # THE move selector. Returns the attack id that ends up in the delayed
    # action struct, so overriding its return value is how a specific move is
    # chosen -- the struct itself is downstream and ignores what we write.
    "FComboTransitions_GeNextAttackID": ["?GeNextAttackID@FComboTransitions@@QEBA"],
    "UAttackComponent_BPF_OverrideCombo": ["?BPF_OverrideCombo@UAttackComponent@@QEAA"],
    # Returns FString BY VALUE -> hidden return pointer in RCX, `this` in RDX.
    # Same convention that crashed us via GetType; here it is declared correctly.
    "DelayedActionAttack_ToString": ["?ToString@DelayedActionAttack@@UEBA"],
    # The puppet is invincible so it never resolves its own death; when the peer
    # reports they died, we put their puppet down explicitly.
    "UCharacterHealthComponent_SetIsDown": ["?SetIsDown@UCharacterHealthComponent@@QEAA"],
    "UCharacterHealthComponent_IsDown": ["?IsDown@UCharacterHealthComponent@@QEBA"],
    "UCharacterHealthComponent_StaticClass": ["?StaticClass@UCharacterHealthComponent@@SA"],
    "UCharacterHealthComponent_InternalSetDownState": [
        "?InternalSetDownState@UCharacterHealthComponent@@QEAA"
    ],
    "UOrderComponent_MultiCastPlayOrder_Impl": ["?MultiCastPlayOrder_Implementation@UOrderComponent@@"],
    "OrderBase_LoadFrom": ["?LoadFrom@OrderBase@@QEAA"],
    # Co-op. Static Get(AActor*) accessors: one call instead of a ProcessEvent
    # into GetComponentByClass, which matters when it runs per enemy per frame.
    "UCharacterHealthComponent_Get": ["?Get@UCharacterHealthComponent@@SAPEAV1@PEAVAActor@@@Z"],
    "UAttackComponent_Get": ["?Get@UAttackComponent@@SAPEAV1@PEAVAActor@@@Z"],
    "UDefenseComponent_Get": ["?Get@UDefenseComponent@@SAPEAV1@PEAVAActor@@@Z"],
    "UDefenseComponent_StaticClass": ["?StaticClass@UDefenseComponent@@SA"],
    # How the host applies the damage its peer dealt: a plain float, so there is
    # no FDamageInfos to reconstruct (its damage value is not even reflected).
    "UHealthComponent_BPF_ApplyDamage": ["?BPF_ApplyDamage@UHealthComponent@@QEAAXM@Z"],
    # (BPF_SetCanBeDamaged is inlined away -- only its exec thunk survives, so
    # invincibility goes through AFightingCharacter::BPF_SetInvincibility.)
    "UHealthComponent_IsDead": ["?IsDead@UHealthComponent@@UEBA_NXZ"],
    # The real StopLogic: UBrainComponent's is an empty base that ICF folded
    # onto several unrelated stubs, so it is only useful through reflection
    # (execStopLogic dispatches virtually). This is the direct fallback.
    "UBehaviorTreeComponent_StopLogic": ["?StopLogic@UBehaviorTreeComponent@@UEAA"],
    # Run-state sync (Phase D visibility): the story-game state and the
    # per-character stats component, so each player can see the other's age,
    # room-clear progress and held weapon. All read-only queries.
    "UWorld_GetGameState": [
        "?GetGameState@UWorld@@QEBAPEAVAThePlainesGameState@@"
    ],
    "UStatsComponent_Get": ["?Get@UStatsComponent@@SAPEAV1@PEAVAActor@@@Z"],
    "UStatsComponent_BPF_GetCharacterAge": ["?BPF_GetCharacterAge@UStatsComponent@@QEBAHXZ"],
    "AFightingCharacter_BPF_GetPickedUpWeapon": [
        "?BPF_GetPickedUpWeapon@AFightingCharacter@@QEBAPEAVABaseWeapon@@XZ"
    ],
    "ABaseWeapon_BPF_GetWeaponData": [
        "?BPF_GetWeaponData@ABaseWeapon@@QEBAPEAVUBaseWeaponData@@XZ"
    ],
}

# Member offsets recovered from Unreal's generated reflection tables rather
# than from hand-diffed hex dumps. Every reflected property records its byte
# offset inside the owning type, and the PDB names the table entry, so these
# are exact and re-derived per build instead of being hardcoded.
WANTED_MEMBERS = {
    "M_UHealthComponent_fHealth": ("UHealthComponent", "m_fHealth"),
    "M_UHealthComponent_fMaxHealth": ("UHealthComponent", "m_fMaxHealth"),
    "M_UCharacterHealthComponent_fGhostDamage": ("UCharacterHealthComponent",
                                                 "m_fGhostDamage"),
    "M_UDefenseComponent_fCurrentGuard": ("UDefenseComponent", "m_fCurrentGuard"),
    "M_AFightingCharacter_eFaction": ("AFightingCharacter", "m_eFaction"),
    "M_AFightingCharacter_HealthComponent": ("AFightingCharacter", "m_HealthComponent"),
    "M_AFightingCharacter_AttackComponent": ("AFightingCharacter", "m_AttackComponent"),
    "M_AFightingCharacter_DefenseComponent": ("AFightingCharacter", "m_DefenseComponent"),
    "M_UAttackComponent_DefaultCombo": ("UAttackComponent", "m_DefaultCombo"),
    # Story-mode room progress (C2). Host-authoritative; the joiner merely reads
    # its own copy, and can optionally be nudged toward the host's percentage.
    "M_AThePlainesGameState_fRoomClearedLifePercent": ("AThePlainesGameState",
                                                      "m_fRoomClearedLifePercent"),
}


class Image:
    """Just enough PE parsing to turn an RVA into the bytes at that address."""

    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: not a PE file")
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

    def cstring_va(self, va, limit=256):
        if va < self.image_base:
            return ""
        raw = self.read(va - self.image_base, limit)
        end = raw.find(b"\0")
        return raw[:end if end >= 0 else len(raw)].decode("utf-8", "replace")


# UE4CodeGen_Private::FPropertyParamsBaseWithOffset, shipping layout:
#   +0x00 const char* NameUTF8      +0x18 EPropertyGenFlags
#   +0x08 const char* RepNotifyFunc +0x1C EObjectFlags
#   +0x10 uint64      PropertyFlags +0x20 int32 ArrayDim   +0x24 uint16 Offset
#
# Bool properties are the exception: they carry a SetBitFunc instead of an
# offset, so a bitfield's address cannot be recovered this way. Nothing here
# needs one -- IsDown()/SetIsDown() cover the only bool that matters.
PROP_PARAMS_SIZE = 0x28

PROP_SYMBOL_RE = None  # compiled lazily; `re` import lives at module top


def resolve_members(symbols, exe_path, wanted_members, verbose=True):
    """Return {alias: byte offset} for every entry in `wanted_members`."""
    if not wanted_members:
        return {}

    pattern = re.compile(
        r"^\?NewProp_(?P<member>[^@]+)@Z_Construct_U(?:ScriptStruct|Class)_"
        r"(?P<type>[A-Za-z0-9_]+)_Statics@@2U(?P<params>F[A-Za-z]*PropertyParams)@")

    index = {}
    for sym in symbols:
        match = pattern.match(sym.name)
        if not match:
            continue
        member = match.group("member")
        if member.endswith("_SetBit"):
            continue
        index[(match.group("type"), member)] = (sym.rva, match.group("params"))

    image = Image(exe_path)
    resolved = {}
    for alias, (type_name, member) in wanted_members.items():
        entry = index.get((type_name, member))
        if entry is None:
            if verbose:
                print(f"UNRESOLVED MEMBER: {type_name}::{member}")
            continue
        rva, params_type = entry
        if params_type == "FBoolPropertyParams":
            if verbose:
                print(f"SKIPPED (bitfield, no offset): {type_name}::{member}")
            continue
        raw = image.read(rva, PROP_PARAMS_SIZE)
        if len(raw) < PROP_PARAMS_SIZE:
            if verbose:
                print(f"UNREADABLE MEMBER: {type_name}::{member}")
            continue
        resolved[alias] = struct.unpack_from("<H", raw, 0x24)[0]
    return resolved


def load_symbols(path, verbose=True):
    start = time.time()
    pdb = PdbFile(path)
    symbols = list(pdb.iter_symbols())
    if verbose:
        elapsed = time.time() - start
        print(
            f"parsed {path}\n"
            f"  sections      : {len(pdb.sections)}\n"
            f"  symbol records: {len(symbols)}\n"
            f"  elapsed       : {elapsed:.1f}s"
        )
    return pdb, symbols


# Symbols worth keeping offline so work can continue without the 925 MB PDB
# mounted (e.g. from the Ubuntu boot). Matched case-sensitively as substrings.
CATALOGUE_TERMS = [
    "SC",  # game class prefix
    "FightingCharacter",
    "Replay",
    "DemoNetDriver",
    "AttackComponent",
    "DefenseComponent",
    "HealthComponent",
    "AnimInstance",
    "Impostor",
    "ThePlaines",
    "GetLifetimeReplicatedProps",
    "NetSerialize",
    "ActorsPool",
    "Poolable",
]


def write_catalogue(path, symbols):
    """Dump every symbol matching CATALOGUE_TERMS, sorted by name."""
    keep = [s for s in symbols if any(t in s.name for t in CATALOGUE_TERMS)]
    keep.sort(key=lambda s: s.name)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(f"# {len(keep)} symbols matching {CATALOGUE_TERMS}\n")
        fh.write("# format: RVA<TAB>mangled-name\n")
        for sym in keep:
            fh.write(f"{sym.rva:08X}\t{sym.name}\n")
    print(f"wrote {path} with {len(keep)} symbols")


def probe(symbols):
    """Report whether this PDB carries the detail we need."""
    publics = [s for s in symbols if s.kind == S_PUB32]
    game = [s for s in publics if "SC" in s.name and "@" in s.name]
    engine = [s for s in publics if "@UWorld@@" in s.name or "@UEngine@@" in s.name]

    print(f"public symbols        : {len(publics)}")
    print(f"  engine (UWorld/UEngine): {len(engine)}")
    print(f"  game-side (SC*)        : {len(game)}")
    print()
    print("sample game symbols:")
    for sym in game[:15]:
        print(f"  0x{sym.rva:08X}  {sym.name[:110]}")


def find(symbols, terms, limit):
    needles = [t.lower() for t in terms]
    count = 0
    for sym in symbols:
        lowered = sym.name.lower()
        if all(n in lowered for n in needles):
            print(f"0x{sym.rva:08X}  {sym.name}")
            count += 1
            if count >= limit:
                print(f"... (truncated at {limit})")
                break
    if count == 0:
        print("no match")


def resolve_wanted(symbols):
    """Return {alias: Symbol} for everything in WANTED that we can find."""
    resolved = {}
    ambiguous = {}
    for alias, needles in WANTED.items():
        matches = [s for s in symbols if all(n in s.name for n in needles)]
        if not matches:
            continue
        # Prefer the shortest mangled name: the least-decorated overload.
        matches.sort(key=lambda s: len(s.name))
        resolved[alias] = matches[0]
        if len(matches) > 1:
            ambiguous[alias] = len(matches)
    return resolved, ambiguous


def read_pe_identity(exe_path):
    """Return (TimeDateStamp, SizeOfImage) so the DLL can refuse a patched exe.

    Offsets computed against one build are actively dangerous against another;
    this lets the mod fail loudly instead of writing hooks into wrong addresses.
    """
    with open(exe_path, "rb") as fh:
        data = fh.read(0x400)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError(f"{exe_path}: not a PE file")
    (timestamp,) = struct.unpack_from("<I", data, pe + 8)
    opt = pe + 24
    (size_of_image,) = struct.unpack_from("<I", data, opt + 56)
    return timestamp, size_of_image


def emit_build_json(path, name, values, exe_path):
    """Write one build's offsets, so another machine can contribute its own.

    The Epic and Steam executables differ, but the *protocol* does not: both
    stores ship the same game version, so asset paths and combo indices match.
    Only the addresses differ, which is exactly what this file carries.
    """
    timestamp, size_of_image = read_pe_identity(exe_path)
    record = {
        "name": name,
        "time_date_stamp": timestamp,
        "size_of_image": size_of_image,
        "offsets": dict(sorted(values.items())),
    }
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=2, sort_keys=True)
    print(f"wrote {path}: build '{name}' "
          f"stamp=0x{timestamp:08X} size=0x{size_of_image:08X} "
          f"({len(record['offsets'])} offsets)")


def generate_multi_build_header(path, builds):
    """Emit offsets as mutable globals plus a fingerprint-keyed table.

    Globals rather than constants so every call site stays `offsets::Name`,
    with SelectBuild() filling them in once the running executable has been
    identified. That keeps support for a second store to a data change.
    """
    if not builds:
        raise ValueError("no build definitions found")

    # Union of aliases, so a build missing one still compiles (it gets 0 and
    # the loader reports it rather than jumping to the image base).
    aliases = sorted({alias for b in builds for alias in b["offsets"]})

    lines = [
        "// GENERATED by tools/pdbdump/pdbdump.py -- do not hand-edit.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace sifucoop::offsets {",
        "",
        "// Filled in by SelectBuild(). Zero means this build did not provide the",
        "// symbol; callers must treat zero as unavailable.",
    ]
    for alias in aliases:
        lines.append(f"inline std::uint32_t {alias} = 0;")

    lines += [
        "",
        "struct BuildEntry {",
        "    std::uint32_t time_date_stamp;",
        "    std::uint32_t size_of_image;",
        "    const char* name;",
        f"    std::uint32_t values[{len(aliases)}];",
        "};",
        "",
    ]

    entries = []
    for b in builds:
        values = ", ".join(f"0x{b['offsets'].get(a, 0):08X}" for a in aliases)
        entries.append(
            f'    {{0x{b["time_date_stamp"]:08X}, 0x{b["size_of_image"]:08X}, '
            f'"{b["name"]}", {{{values}}}}},'
        )

    lines += ["inline constexpr BuildEntry kBuilds[] = {"] + entries + ["};", ""]
    lines += [
        "inline constexpr int kBuildCount = "
        f"sizeof(kBuilds) / sizeof(kBuilds[0]);",
        "",
        "// Returns the matching build's name, or nullptr if this executable is",
        "// unknown -- in which case nothing is populated and the mod stays inert.",
        "inline const char* SelectBuild(std::uint32_t time_date_stamp,",
        "                               std::uint32_t size_of_image) {",
        "    for (const BuildEntry& entry : kBuilds) {",
        "        if (entry.time_date_stamp != time_date_stamp) continue;",
        "        if (entry.size_of_image != size_of_image) continue;",
        "        int i = 0;",
    ]
    for alias in aliases:
        lines.append(f"        {alias} = entry.values[i++];")
    lines += [
        "        return entry.name;",
        "    }",
        "    return nullptr;",
        "}",
        "",
        "}  // namespace sifucoop::offsets",
    ]

    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    names = ", ".join(b["name"] for b in builds)
    print(f"wrote {path}: {len(aliases)} offsets x {len(builds)} build(s) [{names}]")


def generate_header(path, resolved, pdb_path, exe_path=None):
    lines = [
        "// GENERATED by tools/pdbdump/pdbdump.py -- do not hand-edit.",
        f"// source: {pdb_path}",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace sifucoop::offsets {",
        "",
    ]
    if exe_path:
        timestamp, size_of_image = read_pe_identity(exe_path)
        lines += [
            "// Build fingerprint of the exe these offsets were generated against.",
            "// The DLL refuses to hook anything if the running exe disagrees.",
            f"inline constexpr std::uint32_t kExpectedTimeDateStamp = 0x{timestamp:08X};",
            f"inline constexpr std::uint32_t kExpectedSizeOfImage   = 0x{size_of_image:08X};",
            "",
        ]
    for alias in sorted(resolved):
        sym = resolved[alias]
        lines.append(f"// {sym.name}")
        lines.append(f"inline constexpr std::uint32_t {alias} = 0x{sym.rva:08X};")
        lines.append("")
    lines.append("}  // namespace sifucoop::offsets")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    print(f"wrote {path} with {len(resolved)} offsets")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pdb")
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--find", nargs="+", metavar="TERM")
    parser.add_argument("--limit", type=int, default=40)
    parser.add_argument("--gen", metavar="HEADER")
    parser.add_argument("--catalogue", metavar="TXT")
    parser.add_argument("--exe", metavar="EXE", help="exe to fingerprint")
    parser.add_argument("--emit-build", nargs=2, metavar=("NAME", "JSON"),
                        help="write this machine's offsets as a build definition")
    parser.add_argument("--builds", metavar="DIR",
                        help="directory of build .json files to combine into --gen")
    args = parser.parse_args()

    # Combining existing build definitions needs no PDB at all, so a machine
    # that only has someone else's .json can still produce the header.
    if args.gen and args.builds and not args.emit_build:
        files = sorted(glob.glob(os.path.join(args.builds, "*.json")))
        builds = []
        for f in files:
            with open(f, "r", encoding="utf-8") as fh:
                builds.append(json.load(fh))
        generate_multi_build_header(args.gen, builds)
        return

    pdb, symbols = load_symbols(args.pdb)

    if args.probe:
        probe(symbols)
    if args.find:
        find(symbols, args.find, args.limit)
    if args.catalogue:
        write_catalogue(args.catalogue, symbols)
    if args.emit_build or args.gen:
        resolved, ambiguous = resolve_wanted(symbols)
        for alias in WANTED:
            if alias not in resolved:
                print(f"UNRESOLVED: {alias}")
        for alias, count in ambiguous.items():
            print(f"note: {alias} had {count} candidates")

        values = {alias: sym.rva for alias, sym in resolved.items()}
        if args.exe:
            members = resolve_members(symbols, args.exe, WANTED_MEMBERS)
            print(f"resolved {len(members)}/{len(WANTED_MEMBERS)} member offsets")
            values.update(members)

        if args.emit_build:
            if not args.exe:
                print("--emit-build needs --exe")
                return
            name, out_path = args.emit_build
            emit_build_json(out_path, name, values, args.exe)

        if args.gen:
            if args.builds:
                files = sorted(glob.glob(os.path.join(args.builds, "*.json")))
                builds = []
                for f in files:
                    with open(f, "r", encoding="utf-8") as fh:
                        builds.append(json.load(fh))
                generate_multi_build_header(args.gen, builds)
            else:
                generate_header(args.gen, resolved, args.pdb, args.exe)

    pdb.close()


if __name__ == "__main__":
    main()
