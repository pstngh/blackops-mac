#!/bin/bash
# Build "Black Ops Launcher.app" (Launcher.swift) into a game folder, next to the
# blackops that tools/make_portable.sh installs there. With a picture (a square PNG; the
# user's is build_macos/launcher_icon.png, kept out of git: game art), the app gets an icon
# made from it (make_icon.swift), and keeps the picture in Contents/Resources, so the game
# folder carries it: without a picture, a rebuild reuses the one the installed app has. The
# map mosaic's pictures come from the game folder's archives the same way (below).
#
# usage: tools/launcher/build.sh <game dir> [icon picture]
set -euo pipefail

game=${1:?usage: tools/launcher/build.sh <game dir> [icon picture]}
icon=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
app="$game/Black Ops Launcher.app"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

swiftc -O -parse-as-library -target arm64-apple-macos13.0 "$here/Launcher.swift" -o "$work/BlackOpsLauncher"

# The bundle is put together and signed in the temporary folder, then copied into the game folder:
# in a Documents folder under a File Provider (iCloud Drive), the system tags a new bundle with Finder
# info again right after it is removed, and codesign refuses a bundle that carries it ("resource fork,
# Finder information, or similar detritus not allowed"). A tag added after signing is harmless.
bundle="$work/Black Ops Launcher.app"
mkdir -p "$bundle/Contents/MacOS"
cp "$work/BlackOpsLauncher" "$bundle/Contents/MacOS/BlackOpsLauncher"
cat > "$bundle/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>BlackOpsLauncher</string>
    <key>CFBundleIdentifier</key>
    <string>local.kisakblack.launcher</string>
    <key>CFBundleName</key>
    <string>Black Ops Launcher</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF
if { [ -z "$icon" ] || [ ! -f "$icon" ]; } && [ -f "$app/Contents/Resources/launcher_icon.png" ]; then
    icon="$work/launcher_icon.png"
    cp "$app/Contents/Resources/launcher_icon.png" "$icon"
fi
if [ -n "$icon" ] && [ -f "$icon" ]; then
    swiftc -O -target arm64-apple-macos13.0 "$here/make_icon.swift" -o "$work/make_icon"
    mkdir -p "$bundle/Contents/Resources"
    "$work/make_icon" "$icon" "$bundle/Contents/Resources/AppIcon.icns"
    cp "$icon" "$bundle/Contents/Resources/launcher_icon.png"
    /usr/libexec/PlistBuddy -c 'Add :CFBundleIconFile string AppIcon' "$bundle/Contents/Info.plist"
fi
# The map mosaic's pictures (game art too): the pictures of the game's map menu, which the launcher
# lists (--map-pictures), from the game folder's main/*.iwd, as Contents/Resources/maps/<map id>.png.
# The map menu's have a transparent rounded edge, cut off here. Without the archives a rebuild reuses
# the installed app's pictures.
mkdir -p "$work/maps"
"$work/BlackOpsLauncher" --map-pictures > "$work/map_pictures.txt"
if ls "$game"/main/*.iwd > /dev/null 2>&1; then
    python3 "$here/../iwi_to_png.py" "$game" "$work/pictures" $(cut -d' ' -f2 "$work/map_pictures.txt") > /dev/null
    while read -r id picture; do
        [ -f "$work/pictures/$picture.png" ] || continue
        cp "$work/pictures/$picture.png" "$work/maps/$id.png"
        case $picture in
            *_final)
                size=$(sips -g pixelWidth "$work/maps/$id.png" | awk '/pixelWidth/ { print $2 }')
                sips --cropToHeightWidth $((size * 15 / 16)) $((size * 15 / 16)) "$work/maps/$id.png" > /dev/null ;;
        esac
    done < "$work/map_pictures.txt"
fi
if [ -z "$(ls "$work/maps")" ] && [ -d "$app/Contents/Resources/maps" ]; then
    cp "$app/Contents/Resources/maps/"*.png "$work/maps/"
fi
if [ -n "$(ls "$work/maps")" ]; then
    mkdir -p "$bundle/Contents/Resources/maps"
    cp "$work/maps/"*.png "$bundle/Contents/Resources/maps/"
fi
if ! err=$(codesign --force --sign - "$bundle" 2>&1); then
    echo "$err" >&2
    exit 1
fi
rm -rf "$app"
ditto "$bundle" "$app"
echo "installed $app"
