#!/bin/sh
# Builds Openviz.app, with its icon, and installs it in /Applications.
#
#   sh launcher/build_mac_app.sh                 install in /Applications
#   sh launcher/build_mac_app.sh ~/Applications  or in another folder
#
# Needs Xcode's command-line tools (swiftc, iconutil). The app is only a window: everything it
# does is run from this project folder, so it always runs the project as it is now, and it has
# to be built again only if this folder is moved or the window's own code or icon change.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(dirname "$here")"
folder="${1:-/Applications}"
app="$folder/Openviz.app"

if [ ! -d "$folder" ] || [ ! -w "$folder" ]; then
    echo "error: cannot write to $folder; name another folder, for example: sh launcher/build_mac_app.sh ~/Applications" >&2
    exit 1
fi
# Only ever replace an app this script built: it carries the marker file written below.
if [ -e "$app" ] && [ ! -f "$app/Contents/Resources/repo-path.txt" ]; then
    echo "error: $app is there already and was not built by this script; it is left alone" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
build="$work/Openviz.app"
mkdir -p "$build/Contents/MacOS" "$build/Contents/Resources"
swiftc -O -o "$build/Contents/MacOS/Openviz" "$here/mac/main.swift" -framework Cocoa -framework WebKit

# The icon is drawn by a program, at each size macOS asks for rather than scaled down from one.
swiftc -O -o "$work/make_icon" "$here/mac/make_icon.swift"
mkdir "$work/Openviz.iconset"
for size in 16 32 128 256 512; do
    "$work/make_icon" "$work/Openviz.iconset/icon_${size}x${size}.png" "$size"
    "$work/make_icon" "$work/Openviz.iconset/icon_${size}x${size}@2x.png" "$((size * 2))"
done
iconutil -c icns "$work/Openviz.iconset" -o "$build/Contents/Resources/Openviz.icns"

cp "$here/mac/Info.plist" "$build/Contents/Info.plist"
printf '%s\n' "$repo" > "$build/Contents/Resources/repo-path.txt"
codesign --force --sign - "$build" 2>/dev/null

rm -rf "$app"
cp -R "$build" "$app"
touch "$app"    # so the Finder and the Dock pick up a changed icon

# Copies this script left in the project folder before it installed to Applications.
for old in "$repo/Openviz.app" "$repo/Lighting Visualizer.app"; do
    if [ "$old" != "$app" ] && [ -f "$old/Contents/Resources/repo-path.txt" ]; then
        rm -rf "$old"
        echo "Removed the older copy at $old"
    fi
done
echo "Installed $app"
