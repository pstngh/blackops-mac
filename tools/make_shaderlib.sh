#!/bin/bash
# Build the shader library, <game dir>/kb_shaders.metallib: every shader the game has (and the
# per-sample variants of the pixel shaders, for transparency AA), translated to MSL as the Metal
# backend does and compiled ahead of time with Apple's compiler, so that loading a map compiles
# no shaders (HANDOFF, "the shader file"). The game loads it at startup and compiles at run time
# only what isn't in it (with a log line).
#
# usage: tools/make_shaderlib.sh <game dir> [build dir, default build_macos]
#
# 1. The game's shaders: <game dir>/kb_shaders/bytecode, collected once by running the game (the
#    GL layer's KB_DUMP_SHADERS) through the menus and every map (`map`, 20 s each, in a small
#    window, a scratch profile: ~10 minutes); delete the folder to collect again.
# 2. Their MSL (src/gfx_mtl/tools/shaderlib.cpp, the backend's own translator) and its stamp.
# 3. If kb_shaders.stamp holds that stamp and this compiler's version, the library is up to date;
#    otherwise `xcrun metal` compiles it (Xcode's Metal Toolchain: a separate download, per Xcode
#    version; without it this stops and says how to get it).
set -euo pipefail

game=${1:?usage: tools/make_shaderlib.sh <game dir> [build dir]}
root=$(cd "$(dirname "$0")/.." && pwd)
build=${2:-$root/build_macos}
[ -d "$game/main" ] && [ -d "$game/zone" ] || { echo "$game has no main/ and zone/" >&2; exit 1; }
game=$(cd "$game" && pwd)
build=$(cd "$build" && pwd)
exe="$build/blackops"
[ -f "$exe" ] || { echo "no $exe; build it first" >&2; exit 1; }
work="$build/shaderlib"
mkdir -p "$work"

# The generator.
c++ -std=gnu++17 -O1 -w -I"$root/src" "$root/src/gfx_mtl/tools/shaderlib.cpp" "$root/src/gfx_shader/sm3.cpp" \
    "$root/src/gfx_mtl/mtl_msl.cpp" -o "$work/shaderlib"

# 1. The shaders, collected once.
dump="$game/kb_shaders/bytecode"
if [ -z "$(ls "$dump" 2>/dev/null | head -1)" ]; then
    echo "collecting the game's shaders (the game runs in a window for ~10 minutes)..."
    mkdir -p "$dump"
    # A folder of its own (links to the game's data): none of the game folder's settings or
    # profile are read or written.
    run="$work/collect_game"
    home="$work/collect_home"
    rm -rf "$run" "$home"
    mkdir -p "$run" "$home"
    ln -s "$game/main" "$run/main"
    ln -s "$game/zone" "$run/zone"
    cmds=""
    t=10
    for ff in "$game"/zone/Common/mp_*.ff; do
        cmds="$cmds$t:map $(basename "$ff" .ff)|"
        t=$((t + 20))
    done
    cmds="$cmds$t:quit"
    ( cd "$run" && KB_RENDERER=gl KB_DUMP_SHADERS="$dump" KB_MENU_CMDS="$cmds" \
        "$exe" +set fs_h "$home" +set r_fullscreen 0 +set r_mode 800x600 +set r_vsync 0 \
        > "$work/collect.log" 2>&1 ) || true
    n=$(ls "$dump" | wc -l | tr -d ' ')
    if [ "$n" -lt 1000 ]; then
        echo "collected only $n shaders (see $work/collect.log); delete $dump and try again" >&2
        exit 1
    fi
    echo "collected $n shaders"
fi

# 2. The MSL and its stamp.
src="$work/src"
rm -rf "$src"
mkdir -p "$src"
stamp=$("$work/shaderlib" "$dump" "$src")

# 3. The library.
if ! version=$(xcrun -sdk macosx metal -v 2>&1) || [[ "$version" == *"missing Metal Toolchain"* ]]; then
    echo "The shader library needs Xcode's Metal Toolchain (a separate download, ~700 MB):" >&2
    echo "    xcodebuild -downloadComponent MetalToolchain" >&2
    echo "then run this again. Until then the game compiles its shaders at run time." >&2
    exit 1
fi
version=$(echo "$version" | head -1)
want="$stamp $version"
if [ -f "$game/kb_shaders.metallib" ] && [ "$(cat "$game/kb_shaders.stamp" 2>/dev/null)" = "$want" ]; then
    echo "kb_shaders.metallib is up to date"
    exit 0
fi
echo "compiling $(wc -l < "$src/names.txt" | tr -d ' ') shaders..."
air="$work/air"
rm -rf "$air"
mkdir -p "$air"
# -fpreserve-invariance: as the backend's runtime compiles (MTLCompileOptions.preserveInvariance).
ls "$src"/lib_*.metal | xargs -P "$(sysctl -n hw.ncpu)" -I{} sh -c \
    'xcrun -sdk macosx metal -c -fpreserve-invariance "$1" -o "$2/$(basename "$1" .metal).air"' _ {} "$air"
xcrun -sdk macosx metallib "$air"/*.air -o "$work/kb_shaders.metallib"
cp "$work/kb_shaders.metallib" "$game/kb_shaders.metallib.tmp"
mv "$game/kb_shaders.metallib.tmp" "$game/kb_shaders.metallib"
echo "$want" > "$game/kb_shaders.stamp"
echo "wrote $game/kb_shaders.metallib ($(du -h "$game/kb_shaders.metallib" | cut -f1))"
