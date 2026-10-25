#!/bin/bash
# Install the macOS build into a game folder so the folder is self-contained:
# <game>/blackops plus the non-system libraries it links (Homebrew's SDL2 compat,
# SDL3, GLEW, OpenAL, Speex, libvpx) in <game>/lib, the binary pointed at those
# copies. The game then finds its data from its own location and writes its
# config, stats and logs inside the folder (the base path is the executable's
# directory when main/ and zone/ are next to it), whatever directory it is
# started from. The folder can be moved or copied to another Apple Silicon Mac.
# It also builds the launcher, the shader library and the DLC weapons' zone (below).
#
# usage: tools/make_portable.sh [--dlc-sources <dir>] <game dir> [build dir, default build_macos]
set -euo pipefail

usage="usage: tools/make_portable.sh [--dlc-sources <dir>] <game dir> [build dir]"
dlcsources=""
if [ "${1:-}" = --dlc-sources ]; then
    dlcsources=${2:?$usage}
    shift 2
fi
game=${1:?$usage}
build=${2:-$(dirname "$0")/../build_macos}
exe="$build/blackops"
[ -d "$game/main" ] && [ -d "$game/zone" ] || { echo "$game has no main/ and zone/" >&2; exit 1; }
[ -f "$exe" ] || { echo "no $exe; build it first" >&2; exit 1; }

lib="$game/lib"
mkdir -p "$lib"
cp "$exe" "$game/blackops"
chmod 755 "$game/blackops"

# Libraries outside the OS (/System, /usr/lib) that a Mach-O file links.
external_deps() {
    otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -v -e '^/System/' -e '^/usr/lib/' -e '^@' || true
}

# Copy a library into lib/ under its install name's file name, once.
copy_lib() {
    local src=$1 name
    name=$(basename "$src")
    if [ ! -f "$lib/$name" ]; then
        cp -L "$src" "$lib/$name"
        chmod 644 "$lib/$name"
    fi
    echo "$name"
}

declare -a copied=()
for dep in $(external_deps "$game/blackops"); do
    name=$(copy_lib "$dep")
    install_name_tool -change "$dep" "@executable_path/lib/$name" "$game/blackops" 2>/dev/null
    copied+=("$name")
done

# sdl2-compat loads SDL3 at run time and looks next to itself first.
if [[ " ${copied[*]} " == *" libSDL2-2.0.0.dylib "* ]]; then
    sdl2=$(external_deps "$exe" | grep libSDL2 || true)
    sdl3=""
    for c in "$(dirname "$sdl2")/libSDL3.dylib" /opt/homebrew/lib/libSDL3.dylib /usr/local/lib/libSDL3.dylib; do
        [ -f "$c" ] && { sdl3=$c; break; }
    done
    [ -n "$sdl3" ] || { echo "libSDL3.dylib not found (brew install sdl3)" >&2; exit 1; }
    cp -L "$sdl3" "$lib/libSDL3.dylib"
    chmod 644 "$lib/libSDL3.dylib"
    copied+=("libSDL3.dylib")
fi

# Each copy names itself by its new location; their own dependencies must be the
# OS's or each other's.
for name in "${copied[@]}"; do
    install_name_tool -id "@executable_path/lib/$name" "$lib/$name" 2>/dev/null
    for dep in $(external_deps "$lib/$name"); do
        depname=$(basename "$dep")
        [ "$depname" = "$name" ] && continue
        [ -f "$lib/$depname" ] || { echo "$name needs $dep, which is not bundled" >&2; exit 1; }
        install_name_tool -change "$dep" "@executable_path/lib/$depname" "$lib/$name" 2>/dev/null
    done
done

# install_name_tool invalidates the signatures; arm64 refuses unsigned code.
for name in "${copied[@]}"; do codesign --force --sign - "$lib/$name" 2>/dev/null; done
codesign --force --sign - "$game/blackops" 2>/dev/null

left=$( { external_deps "$game/blackops"; for name in "${copied[@]}"; do external_deps "$lib/$name"; done; } | sort -u)
[ -z "$left" ] || { echo "still linked outside the folder:" >&2; echo "$left" >&2; exit 1; }
echo "installed $game/blackops and ${#copied[@]} libraries in $lib: ${copied[*]}"

# The launcher app (tools/launcher: map, mode and bots, then Play), with its icon when the build
# folder has the picture (launcher_icon.png; game art, so not in git) and the maps' pictures from
# the game folder's archives.
if command -v swiftc >/dev/null; then
    "$(dirname "$0")/launcher/build.sh" "$game" "$build/launcher_icon.png"
fi

# The shader library (tools/make_shaderlib.sh): kb_shaders.metallib, rebuilt only when the
# shader translation changed. The first time it collects the game's shaders (the game runs in
# a window for ~10 minutes); without Xcode's Metal Toolchain it says how to get it, and the
# game compiles its shaders at run time until then.
"$(dirname "$0")/make_shaderlib.sh" "$game" "$build" || echo "(no shader library: see above)"

# The DLC weapons (tools/make_dlc_weapons.sh; the kb_dlcWeapons dvar): zone/Common/kb_dlc_weapons.ff,
# written from the game's zombie maps by this build in ~10 s, every time (the writer, the rows or the
# attachments may have changed). The script copies the zone in only when it was written, so a failure
# leaves the old one. A game folder without the zombie maps (the user's, since session 36) takes them from
# --dlc-sources <dir>, by default the backup folder next to it (codbo_wwii_guns_backup, which has them;
# Decisions session 38); without either it keeps its installed zone as it is.
[ -n "$dlcsources" ] || [ ! -d "$(dirname "$game")/codbo_wwii_guns_backup/zone/Common" ] \
    || dlcsources="$(dirname "$game")/codbo_wwii_guns_backup"
if "$(dirname "$0")/make_dlc_weapons.sh" --sources "$game" 2>/dev/null; then
    "$(dirname "$0")/make_dlc_weapons.sh" "$game" "$game/zone/Common/kb_dlc_weapons.ff" "$build" \
        || echo "(no DLC weapons zone: see above)"
elif [ -n "$dlcsources" ] && "$(dirname "$0")/make_dlc_weapons.sh" --sources --extra-sources "$dlcsources" "$game" 2>/dev/null; then
    echo "writing zone/Common/kb_dlc_weapons.ff with the zombie maps of $dlcsources"
    "$(dirname "$0")/make_dlc_weapons.sh" --extra-sources "$dlcsources" "$game" "$game/zone/Common/kb_dlc_weapons.ff" "$build" \
        || echo "(no DLC weapons zone: see above)"
elif [ -f "$game/zone/Common/kb_dlc_weapons.ff" ]; then
    echo "kept the installed zone/Common/kb_dlc_weapons.ff: the game folder has no zombie maps to write it from"
else
    "$(dirname "$0")/make_dlc_weapons.sh" --sources "$game" || true
    echo "(no DLC weapons zone: the game folder has no zombie maps to write it from)"
fi
