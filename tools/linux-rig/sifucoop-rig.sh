#!/usr/bin/env bash
#
# Two isolated Sifu instances on one Linux box, for SifuCoop testing.
#
# Why this works with no changes to the mod: SifuCoop's single-instance guard is
# a named mutex ("Local\SifuCoop.Game.Process"). Named mutexes are implemented by
# wineserver, and every wine prefix runs its own wineserver, so the two instances
# cannot see each other's mutex. Same for the host port guard.
#
# The one thing wine does NOT isolate is the network stack. That is fine here:
# the host binds UDP 7777 on the real kernel, and the joiner binds an ephemeral
# port (local_port=0, the default), so they do not collide and talk over
# loopback.
#
# SifuCoop.ini has to live next to Sifu-Win64-Shipping.exe, and the two
# instances need different ones, so the joiner runs from a symlink overlay of
# the game tree. Nothing is copied except dsound.dll and the two ini files.
#
# Usage:
#   ./sifucoop-rig.sh setup     # create prefixes, overlay, ini files
#   ./sifucoop-rig.sh host      # launch the host instance
#   ./sifucoop-rig.sh join      # launch the joining instance
#   ./sifucoop-rig.sh check     # print what is configured and what is missing
#   ./sifucoop-rig.sh clean     # remove prefixes and overlay (not the game)

set -euo pipefail

# ---------------------------------------------------------------- configuration

# Directory holding Sifu-Win64-Shipping.exe.
GAME_WIN64="${GAME_WIN64:-$HOME/.steam/steam/steamapps/common/Sifu/Sifu/Binaries/Win64}"

# Built by build.ps1 on the Windows machine; copy dist/dsound.dll over.
MOD_DLL="${MOD_DLL:-$HOME/sifucoop/dsound.dll}"

# Where the rig keeps its prefixes and the joiner's overlay.
RIG_DIR="${RIG_DIR:-$HOME/sifucoop/rig}"

# "proton" for a Steam copy, "wine" if the game runs standalone.
LAUNCH_MODE="${LAUNCH_MODE:-proton}"

PROTON="${PROTON:-$HOME/.steam/steam/steamapps/common/Proton - Experimental/proton}"
STEAM_ROOT="${STEAM_ROOT:-$HOME/.steam/steam}"
WINE="${WINE:-wine}"

PORT="${PORT:-7777}"
PASSPHRASE="${PASSPHRASE:-}"

# ---------------------------------------------------------------------- derived

EXE_NAME="Sifu-Win64-Shipping.exe"
REL_PATH="Sifu/Binaries/Win64"
HOST_PREFIX="$RIG_DIR/prefix-host"
JOIN_PREFIX="$RIG_DIR/prefix-join"
OVERLAY_ROOT="$RIG_DIR/overlay-join"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }

game_root() {
    # GAME_WIN64 is <root>/Sifu/Binaries/Win64
    ( cd "$GAME_WIN64/../../.." && pwd )
}

# Mirror one directory level: symlink every entry except the one we descend into.
mirror_level() {
    local real="$1" shadow="$2" skip="$3"
    mkdir -p "$shadow"
    local entry name
    while IFS= read -r -d '' entry; do
        name="$(basename "$entry")"
        [ "$name" = "$skip" ] && continue
        ln -sfn "$entry" "$shadow/$name"
    done < <(find "$real" -mindepth 1 -maxdepth 1 -print0)
}

write_ini() {
    local target="$1" mode="$2"
    cat > "$target" <<INI
[net]
mode=$mode
host=127.0.0.1
port=$PORT
passphrase=$PASSPHRASE
local_port=0

[coop]
native_network=0
INI
}

# ------------------------------------------------------------------- operations

do_check() {
    echo "SifuCoop Linux rig"
    note "game Win64   : $GAME_WIN64"
    note "mod dll      : $MOD_DLL"
    note "rig dir      : $RIG_DIR"
    note "launch mode  : $LAUNCH_MODE"
    [ "$LAUNCH_MODE" = "proton" ] && note "proton       : $PROTON"
    note "port         : $PORT"
    echo
    [ -f "$GAME_WIN64/$EXE_NAME" ] || die "$EXE_NAME not found in GAME_WIN64"
    [ -f "$MOD_DLL" ] || die "mod dll not found at MOD_DLL (copy dist/dsound.dll here)"
    if [ "$LAUNCH_MODE" = "proton" ]; then
        [ -x "$PROTON" ] || die "proton not executable at PROTON"
        [ -d "$STEAM_ROOT" ] || die "STEAM_ROOT does not exist"
    else
        command -v "$WINE" >/dev/null || die "wine not on PATH"
    fi
    echo "prerequisites OK"
    [ -d "$OVERLAY_ROOT" ] && echo "overlay present" || echo "overlay NOT built -- run: $0 setup"
}

do_setup() {
    do_check >/dev/null

    local root; root="$(game_root)"
    echo "game root: $root"

    mkdir -p "$HOST_PREFIX" "$JOIN_PREFIX"

    # Build the joiner's overlay: real directories along Sifu/Binaries/Win64,
    # symlinks for everything else, so the game sees an identical tree.
    rm -rf "$OVERLAY_ROOT"
    local real="$root" shadow="$OVERLAY_ROOT" segment
    local -a segments
    local IFS_SAVE="$IFS"
    IFS='/'
    read -r -a segments <<< "$REL_PATH"
    IFS="$IFS_SAVE"
    for segment in "${segments[@]}"; do
        mirror_level "$real" "$shadow" "$segment"
        real="$real/$segment"
        shadow="$shadow/$segment"
    done
    # Leaf: the Win64 directory itself.
    mirror_level "$real" "$shadow" "dsound.dll"
    rm -f "$shadow/SifuCoop.ini"

    install -m 644 "$MOD_DLL" "$shadow/dsound.dll"
    write_ini "$shadow/SifuCoop.ini" "client"
    echo "overlay built: $shadow"

    # Host runs from the real install, so back up anything already there once.
    local file
    for file in dsound.dll SifuCoop.ini; do
        if [ -e "$GAME_WIN64/$file" ] && [ ! -e "$GAME_WIN64/$file.rig-backup" ]; then
            cp -p "$GAME_WIN64/$file" "$GAME_WIN64/$file.rig-backup"
            echo "backed up existing $file -> $file.rig-backup"
        fi
    done
    install -m 644 "$MOD_DLL" "$GAME_WIN64/dsound.dll"
    write_ini "$GAME_WIN64/SifuCoop.ini" "host"
    echo "host install patched: $GAME_WIN64"

    echo
    echo "done. now:  $0 host    (then, in another terminal)  $0 join"
}

launch() {
    local prefix="$1" win64="$2" label="$3"
    [ -f "$win64/$EXE_NAME" ] || die "$label: $EXE_NAME missing at $win64"
    [ -f "$win64/dsound.dll" ] || die "$label: dsound.dll missing -- run: $0 setup"

    echo "launching $label"
    echo "  prefix : $prefix"
    echo "  exe    : $win64/$EXE_NAME"

    if [ "$LAUNCH_MODE" = "proton" ]; then
        STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT" \
        STEAM_COMPAT_DATA_PATH="$prefix" \
        WINEDLLOVERRIDES="dsound=n,b" \
        "$PROTON" run "$win64/$EXE_NAME"
    else
        WINEPREFIX="$prefix" \
        WINEDLLOVERRIDES="dsound=n,b" \
        "$WINE" "$win64/$EXE_NAME"
    fi
}

do_clean() {
    rm -rf "$HOST_PREFIX" "$JOIN_PREFIX" "$OVERLAY_ROOT"
    echo "removed prefixes and overlay (game install untouched)"
}

case "${1:-check}" in
    setup) do_setup ;;
    host)  launch "$HOST_PREFIX" "$GAME_WIN64" "HOST" ;;
    join)  launch "$JOIN_PREFIX" "$OVERLAY_ROOT/$REL_PATH" "JOIN" ;;
    check) do_check ;;
    clean) do_clean ;;
    *) die "unknown command '${1}' (setup|host|join|check|clean)" ;;
esac
