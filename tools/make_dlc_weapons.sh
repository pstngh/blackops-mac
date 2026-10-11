#!/bin/bash
# Write kb_dlc_weapons.ff: the DLC weapons (Rezurrection's WWII guns from the remade World at War
# zombie maps and the MP40; no Pack-a-Punch versions or wonder weapons, the user's choice)
# under multiplayer names, extracted from the game's zombie zones by the zone writer (HANDOFF,
# Decisions sessions 25-26). The zone holds what the weapons need that multiplayer lacks and refers
# to the rest.
#
# usage: tools/make_dlc_weapons.sh [--no-parents] <game dir> <out .ff> [build dir, default build_macos]
#
# Each weapon's parentWeaponName is its statsTable reference; the client stops with "Parent weapon
# ... not found" for a weapon whose reference the table lacks. --no-parents leaves them empty, to
# try the weapons (loadzone, give) without the rows.
#
# A dedicated server (the build's) runs from a folder of its own (links to the game's data, its own
# zone/Common for the zones it writes, a scratch profile): one zone per map (the map's patch zone
# first, as it overrides the map's assets, then the map and common_zombie: at most 3 source zones
# fit in memory), then merges of those, 3 at a time (`writezone ... all`; a name in several zones
# is written once).
set -euo pipefail

noparents=0
if [ "${1:-}" = --no-parents ]; then
    noparents=1
    shift
fi
game=${1:?usage: tools/make_dlc_weapons.sh <game dir> <out .ff> [build dir]}
out=${2:?usage: tools/make_dlc_weapons.sh <game dir> <out .ff> [build dir]}
root=$(cd "$(dirname "$0")/.." && pwd)
build=${3:-$root/build_macos}
[ -d "$game/main" ] && [ -d "$game/zone/Common" ] || { echo "$game has no main/ and zone/Common/" >&2; exit 1; }
game=$(cd "$game" && pwd)
build=$(cd "$build" && pwd)
exe="$build/blackops"
[ -f "$exe" ] || { echo "no $exe; build it first" >&2; exit 1; }
work="$build/dlc_weapons"
rm -rf "$work"
mkdir -p "$work/game/zone/Common" "$work/home/main"
ln -s "$game/main" "$work/game/main"
for d in "$game"/zone/*; do
    [ "$(basename "$d")" = Common ] || ln -s "$d" "$work/game/zone/$(basename "$d")"
done
for f in "$game"/zone/Common/*; do
    ln -s "$f" "$work/game/zone/Common/$(basename "$f")"
done
zones="$work/game/zone/Common"

# <source zones> <zone written> then weapon <zombie name>=<multiplayer name>[:<statsTable reference>]
# (the reference defaults to the name without _mp). References are no prefix of another one or of a
# multiplayer weapon (BG_UnlockablesGetItemIndexforWeapon takes the first prefix match). A weapon's
# alt, dual-wield and spawned weapons come along and need a name too.
maps=(
"zombie_cod5_factory_patch,zombie_cod5_factory,common_zombie kb_dlc_factory
 zombie_thompson=thompson_mp zombie_type100_smg=type100_mp zombie_stg44=stg44_mp zombie_fg42=fg42_mp
 zombie_gewehr43=gewehr43_mp zombie_kar98k=kar98k_mp zombie_m1carbine=m1carbine_mp
 zombie_shotgun=trenchgun_mp zombie_doublebarrel=doublebarrel_mp mp40_zm=mp40_mp"
"zombie_cod5_asylum_patch,zombie_cod5_asylum,common_zombie kb_dlc_asylum
 zombie_bar=bar_mp zombie_bar_bipod=bipodbar_mp zombie_m1garand=m1garand_mp
 zombie_doublebarrel_sawed=sawedoff_mp"
"zombie_cod5_prototype_patch,zombie_cod5_prototype,common_zombie kb_dlc_prototype
 kar98k_scoped_zombie=scopedkar98k_mp"
"zombie_cod5_sumpf_patch,zombie_cod5_sumpf,common_zombie kb_dlc_sumpf
 zombie_type99_rifle=type99_mp"
)

cfg="$work/home/main/kb_dlc_weapons.cfg"
: > "$cfg"
written=()
for m in "${maps[@]}"; do
    set -- $m
    sources=$1
    name=$2
    shift 2
    line="writezone $zones/$name.ff $sources"
    for w in "$@"; do
        [ $noparents = 1 ] && w="${w%%:*}:"
        line="$line weapon $w"
    done
    echo "$line" >> "$cfg"
    written+=("$name")
done
# The merges: the first zone, then two more per run.
merged=${written[0]}
i=1
n=1
while [ $i -lt ${#written[@]} ]; do
    next="kb_dlc_merge$n"
    sources="$merged,${written[$i]}"
    [ $((i + 1)) -lt ${#written[@]} ] && sources="$sources,${written[$((i + 1))]}"
    i=$((i + 2))
    [ $i -ge ${#written[@]} ] && next=kb_dlc_weapons
    echo "writezone $zones/$next.ff $sources all" >> "$cfg"
    merged=$next
    n=$((n + 1))
done
echo "quit" >> "$cfg"

log="$work/writezone.log"
( cd "$work/game" && "$exe" +set dedicated 1 +set fs_h "$work/home" +exec kb_dlc_weapons.cfg > "$log" 2>&1 ) || true
if grep -a -q "^Error: writezone:" "$log" || ! grep -a -q "^writezone: wrote '$zones/kb_dlc_weapons.ff'" "$log"; then
    grep -a "^writezone: wrote\|^Error: writezone:" "$log" >&2 || true
    echo "writing the zone failed; see $log" >&2
    exit 1
fi
grep -a "^writezone: wrote\|stub .* is in no source zone\|has no default asset" "$log" || true
cp "$zones/kb_dlc_weapons.ff" "$out"
echo "wrote $out"
