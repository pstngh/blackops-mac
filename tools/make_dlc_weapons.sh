#!/bin/bash
# Write kb_dlc_weapons.ff: the DLC weapons (Rezurrection's WWII guns from the remade World at War
# zombie maps and the MP40; no Pack-a-Punch versions or wonder weapons, the user's choice)
# under multiplayer names, extracted from the game's zombie zones by the zone writer (HANDOFF,
# Decisions sessions 25-27). The zone holds what the weapons need that multiplayer lacks and refers
# to the rest; the game loads it at startup (kb_dlcWeapons; make_portable.sh puts it in the game
# folder's zone/Common).
#
# usage: tools/make_dlc_weapons.sh [--no-parents] [--extra-sources <dir>] <game dir> <out .ff> [build dir, default build_macos]
#        tools/make_dlc_weapons.sh --sources [--extra-sources <dir>] <game dir>
#
# --sources only checks that the game folder has the source zones (the four World at War zombie maps,
# their patch zones and common_zombie): it names the missing ones and fails. The user's game folder no
# longer has them (session 36: zombies removed); the backup folder next to it does
# (codbo_wwii_guns_backup, its README.txt). --extra-sources <dir> takes the zones a folder of the game
# folder's layout has (zone/Common, zone/English) where the game folder lacks them: the backup folder
# (make_portable.sh passes it; Decisions session 38). Without it make_portable.sh keeps the installed zone.
#
# Each weapon's parentWeaponName is its statsTable reference (the zone's copy of the table has the
# rows); the client stops with "Parent weapon ... not found" for a weapon whose reference the table
# lacks. --no-parents leaves them empty (session 26's tests, before the rows).
#
# A dedicated server (the build's) runs from a folder of its own (links to the game's data, its own
# zone/Common for the zones it writes, a scratch profile): one zone per map (the map's patch zone
# first, as it overrides the map's assets, then the map and common_zombie: at most 3 source zones
# fit in memory), then merges of those, 3 at a time (`writezone ... all`; a name in several zones
# is written once). The last merge also gives each gun the damage of a multiplayer gun, puts the
# guns' rows into copies of mp/statsTable.csv (tools/dlc_weapons_stats.csv) and
# mp/attributesTable.csv (tools/dlc_weapons_attributes.csv, Create a Class's bars) and
# mp/weaponOptions.csv (tools/dlc_weapons_options.csv, camo; the zone replaces the tables when it
# loads) and adds their descriptions (tools/dlc_weapons_strings.txt): Create a
# Class, the weapon table and the bots read the tables (HANDOFF, Decisions session 27). It also makes the
# attachments (parts, variants, hides, sounds below: the Thompson's suppressor, HANDOFF, Decisions session 38).
# The zone also keeps a few suppressed shot sounds for suppressed versions of the guns (keep, below).
set -euo pipefail

usage="usage: tools/make_dlc_weapons.sh [--no-parents] [--extra-sources <dir>] <game dir> <out .ff> [build dir] | --sources [--extra-sources <dir>] <game dir>"
noparents=0
sourcesonly=0
extra=""
while [ $# -gt 0 ]; do
    case "$1" in
    --no-parents) noparents=1; shift ;;
    --sources) sourcesonly=1; shift ;;
    --extra-sources) extra=${2:?$usage}; shift 2 ;;
    *) break ;;
    esac
done
game=${1:?$usage}
[ $sourcesonly = 1 ] || out=${2:?$usage}
root=$(cd "$(dirname "$0")/.." && pwd)
build=${3:-$root/build_macos}
[ -d "$game/main" ] && [ -d "$game/zone/Common" ] || { echo "$game has no main/ and zone/Common/" >&2; exit 1; }
game=$(cd "$game" && pwd)
if [ -n "$extra" ]; then
    [ -d "$extra/zone/Common" ] || { echo "$extra has no zone/Common/" >&2; exit 1; }
    extra=$(cd "$extra" && pwd)
fi

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

# The source zones the game folder (or the extra sources) must have (the runs' first fields).
missing=""
for m in "${maps[@]}"; do
    set -- $m
    for z in ${1//,/ }; do
        [ -f "$game/zone/Common/$z.ff" ] || { [ -n "$extra" ] && [ -f "$extra/zone/Common/$z.ff" ]; } \
            || [[ " $missing " == *" $z "* ]] || missing="$missing $z"
    done
done
if [ -n "$missing" ]; then
    echo "$game/zone/Common${extra:+ and $extra/zone/Common} lack the source zones:$missing" >&2
    exit 1
fi
[ $sourcesonly = 1 ] && exit 0

build=$(cd "$build" && pwd)
exe="$build/blackops"
[ -f "$exe" ] || { echo "no $exe; build it first" >&2; exit 1; }
work="$build/dlc_weapons"
rm -rf "$work"
mkdir -p "$work/game/zone/Common" "$work/home/main"
ln -s "$game/main" "$work/game/main"
# Links to the game folder's zones, then to the extra sources' zones it lacks; not a zone this script wrote
# (make_portable.sh puts it in the game folder).
for f in "$game"/zone/*/* ${extra:+"$extra"/zone/*/*}; do
    rel=$(basename "$(dirname "$f")")/$(basename "$f")
    [ "$(basename "$f")" = kb_dlc_weapons.ff ] || [ -e "$work/game/zone/$rel" ] && continue
    mkdir -p "$work/game/zone/$(dirname "$rel")"
    ln -s "$f" "$work/game/zone/$rel"
done
zones="$work/game/zone/Common"

# The multiplayer gun whose damage each gun takes (the closest: kind, fire mode and rate).
damage=(
 thompson_mp=mp5k_mp type100_mp=ak74u_mp mp40_mp=mp5k_mp
 stg44_mp=ak47_mp m1garand_mp=m14_mp gewehr43_mp=fnfal_mp m1carbine_mp=fnfal_mp
 fg42_mp=stoner63_mp bar_mp=m60_mp bipodbar_mp=m60_mp
 kar98k_mp=l96a1_mp scopedkar98k_mp=l96a1_mp type99_mp=l96a1_mp
 trenchgun_mp=ithaca_mp doublebarrel_mp=rottweil72_mp sawedoff_mp=rottweil72_mp
 thompson_silencer_mp=mp5k_silencer_mp
)

# Attachments (HANDOFF, Decisions session 38): a suppressor for the Thompson, made only of what the game has
# (the user's answer in session 31). Multiplayer's gun models carry every attachment's parts and each
# variant hides the ones it lacks (hideTags); of the WWII guns only the MP40's view model has parts (World at
# War's silencer, aperture sight and drum; its world model none). "part" copies a model's part (its bone and
# the surfaces that follow it) into another model, moved by an offset: here the MP40's silencer onto both
# Thompson models, its collar 0.05 over the muzzle, centred on the bore (measured from slices of the models'
# vertices, a temporary listing in session 38). The MP40's silencer: collar from x 15.262, the can's axis y 0.490 z 0.950 (view
# model). The Thompson's muzzle: view model x 18.174, bore y 0.139 z 1.805; world model x 11.542, y 0.003
# z 1.145. The world model gets the view model's part too (the MP40's world model has none).
parts=(
 viewmodel_zombie_thompson_smg=viewmodel_mp_mp40:tag_silencer:2.862,-0.351,0.855
 weapon_zombie_thompson_smg=viewmodel_mp_mp40:tag_silencer:-3.770,-0.487,0.195
)
# Each variant is its base gun as a multiplayer variant is to its own base (the suppressor's sounds, muzzle
# flash and flags), with a localized name (tools/dlc_weapons_strings.txt); it copies its base's hidden parts
# before "hide", so the base guns hide the new part and the variant shows it. "sound" gives the variant the
# gun's own suppressed shots: the Black Ops-era Thompson silencer aliases (the AUG's suppressed recordings,
# the user's pick in session 31). The statsTable row lists the attachment (tools/dlc_weapons_stats.csv).
variants=(
 thompson_silencer_mp=thompson_mp:mp5k_silencer_mp:KB_DLC_THOMPSON_SILENCER
)
hides=(
 thompson_mp=tag_silencer
)
sounds=(
 thompson_silencer_mp=wpn_mp5_silencer_fire_plr:wpn_thompson_silencer_fire_plr
 thompson_silencer_mp=wpn_mp5_silencer_fire_npc:wpn_thompson_silencer_fire_npc
)

# Sound aliases kept for suppressed versions of the guns, whether a gun names them or not (HANDOFF, Decisions
# session 36): World at War's suppressed Thompson and Type 100 shots (their own recordings) and the
# Black Ops-era Thompson and MP40 silencer shots (the AUG's suppressed recordings; the Thompson's are the
# Thompson Suppressor's since session 38); their secondaries
# (sweet, bass) come along. The factory run's sources hold them all; every merge passes them on, as a
# merge cuts its bank again from what the zone names.
keep=(weap_thompson_fire_plr_sil weap_type100_fire_plr_act_sil weap_type100_act_sil
 wpn_thompson_silencer_fire_plr wpn_thompson_silencer_fire_npc wpn_mp40_silencer_fire_plr wpn_mp40_silencer_fire_npc)
keepfrom=kb_dlc_factory

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
    if [ "$name" = $keepfrom ]; then
        for k in "${keep[@]}"; do
            line="$line alias $k"
        done
    fi
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
    line="writezone $zones/$next.ff $sources all"
    for k in "${keep[@]}"; do
        line="$line alias $k"
    done
    if [ $next = kb_dlc_weapons ]; then
        for d in "${damage[@]}"; do
            line="$line damage $d"
        done
        for p in ${parts[@]+"${parts[@]}"}; do
            line="$line part $p"
        done
        for v in ${variants[@]+"${variants[@]}"}; do
            line="$line variant $v"
        done
        for h in ${hides[@]+"${hides[@]}"}; do
            line="$line hide $h"
        done
        for o in ${sounds[@]+"${sounds[@]}"}; do
            line="$line sound $o"
        done
        line="$line rows \"mp/statsTable.csv=$root/tools/dlc_weapons_stats.csv\""
        line="$line addrows \"mp/attributesTable.csv=$root/tools/dlc_weapons_attributes.csv\""
        line="$line addrows \"mp/weaponOptions.csv=$root/tools/dlc_weapons_options.csv\""
        line="$line strings \"$root/tools/dlc_weapons_strings.txt\""
    fi
    echo "$line" >> "$cfg"
    merged=$next
    n=$((n + 1))
done
echo "quit" >> "$cfg"

log="$work/writezone.log"
( cd "$work/game" && "$exe" +set dedicated 1 +set fs_h "$work/home" +set kb_dlcWeapons 0 +exec kb_dlc_weapons.cfg > "$log" 2>&1 ) || true
if grep -a -q "^Error: writezone:" "$log" || ! grep -a -q "^writezone: wrote '$zones/kb_dlc_weapons.ff'" "$log"; then
    grep -a "^writezone: wrote\|^Error: writezone:" "$log" >&2 || true
    echo "writing the zone failed; see $log" >&2
    exit 1
fi
grep -a "^writezone: wrote\|stub .* is in no source zone\|has no default asset\|takes the damage\|rows from\|multiplayer has the sound alias\|gets .* surfaces\|is .* as .* is to\|plays .* for .* sounds" "$log" || true
cp "$zones/kb_dlc_weapons.ff" "$out"
echo "wrote $out"
