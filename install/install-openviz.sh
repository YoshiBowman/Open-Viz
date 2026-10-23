#!/bin/sh
# Openviz installer for macOS: downloads the newest Openviz release from GitHub and sets it up.
#
# Double-click "Install Openviz.command", or in Terminal:
#   curl -fsSL https://raw.githubusercontent.com/YoshiBowman/Open-Viz/main/install/install-openviz.sh | sh
#
# It asks where Openviz is to go (~/Openviz unless told otherwise), has Apple's own installer put
# in the Command Line Tools if they are missing (all Openviz needs to start), downloads the release
# and checks it against the checksum GitHub gives, unpacks it, builds the core (with CMake if there
# is one, else with Apple's compiler) and the Openviz app, and opens it. Run it again to update: the
# shows, fixtures and settings in the folder's library/ are kept. It never asks for your password.
# Xcode, its Metal toolchain and Unreal Engine, which the visualizer needs, are listed under
# System, Setup in Openviz, which installs or opens each of them in turn.
#
# Settings for those who want them: OPENVIZ_DIR (where Openviz goes), OPENVIZ_APPS (where the app
# goes, /Applications), OPENVIZ_NO_OPEN=1 (do not open it at the end), UE_ROOT (Unreal's folder).

REPOSITORY="YoshiBowman/Open-Viz"
case "$0" in *.command) OPENVIZ_DOUBLE_CLICKED=1 ;; esac    # opened from the Finder, in a window of its own

main() {
    set -u
    engine="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
    apps="${OPENVIZ_APPS:-/Applications}"
    say "Openviz installer"
    say "This puts Openviz, a show-lighting visualizer, on this Mac, from github.com/$REPOSITORY."

    step "1. What Openviz needs"
    if ! xcode-select -p >/dev/null 2>&1 || ! /usr/bin/python3 -c 'pass' >/dev/null 2>&1; then
        say "missing    Apple's Command Line Tools: the Python Openviz runs on and the compiler it builds with."
        if yes_no "Install them now? Apple's installer asks before it downloads anything." y; then
            xcode-select --install >/dev/null 2>&1 || true
            say "           Follow Apple's installer; this goes on by itself once they are installed."
            waited=0
            until xcode-select -p >/dev/null 2>&1 && /usr/bin/python3 -c 'pass' >/dev/null 2>&1; do
                sleep 5
                waited=$((waited + 5))
                [ "$waited" -ge 3600 ] && fail "the Command Line Tools did not arrive within an hour. Run this installer again once they are in."
            done
        else
            fail "Openviz cannot start without them. Run this installer again when you want them."
        fi
    fi
    if ! /usr/bin/python3 -c 'import sys; sys.exit(sys.version_info < (3, 9))' >/dev/null 2>&1; then
        fail "Python 3.9 or newer is needed, and Apple's tools bring an older one. Update the Command Line Tools in System Settings, Software Update."
    fi
    say "ok         Apple's developer tools ($(xcode-select -p)), Python $(/usr/bin/python3 -c 'import platform; print(platform.python_version())')"
    if [ -x "$engine/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" ]; then
        say "ok         Unreal Engine 5.8 ($engine)"
    else
        say "later      Unreal Engine 5.8, Xcode and its Metal toolchain, for the visualizer: Openviz lists them under"
        say "           System, Setup, and installs or opens each in turn."
    fi

    step "2. Where Openviz goes"
    dest="${OPENVIZ_DIR:-}"
    if [ -z "$dest" ]; then
        dest="$(ask "Install Openviz in $HOME/Openviz? Press Return, or type another folder:" "$HOME/Openviz")"
    fi
    case "$dest" in "~"*) dest="$HOME${dest#\~}" ;; esac
    case "$dest" in /*) ;; *) dest="$PWD/$dest" ;; esac
    if [ -d "$dest/.git" ]; then
        fail "$dest is a copy of Openviz made with Git: update it there with  git pull  instead."
    fi
    updating=""
    if [ -f "$dest/launcher/launcher.py" ]; then
        updating="yes"
        say "Openviz is already in $dest: it is updated, and its library (shows, fixtures, settings) is kept."
    elif [ -d "$dest" ] && [ -n "$(ls -A "$dest" 2>/dev/null)" ]; then
        fail "$dest has other things in it. Run the installer again and name an empty or new folder."
    else
        say "Openviz goes in $dest"
    fi

    step "3. Downloading"
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    if ! curl -fsSL -H "Accept: application/vnd.github+json" -H "User-Agent: Openviz-installer" -o "$work/releases.json" "https://api.github.com/repos/$REPOSITORY/releases"; then
        fail "GitHub could not be reached. Is this Mac on the internet?"
    fi
    found="$(/usr/bin/python3 - "$work/releases.json" <<'PYTHON'
import json, re, sys
releases = [r for r in json.load(open(sys.argv[1])) if not r.get("draft")]
for release in releases:
    for asset in release.get("assets", []):
        if re.fullmatch(r"Openviz-.*\.zip", asset["name"]):
            digest = asset.get("digest") or ""
            print(release["tag_name"], asset["browser_download_url"], digest[len("sha256:"):] if digest.startswith("sha256:") else "-", asset["name"], sep="\t")
            sys.exit(0)
sys.exit(1)
PYTHON
)" || fail "no release of Openviz with a download was found on GitHub."
    tag="$(printf '%s' "$found" | cut -f1)"
    url="$(printf '%s' "$found" | cut -f2)"
    sum="$(printf '%s' "$found" | cut -f3)"
    name="$(printf '%s' "$found" | cut -f4)"
    say "Openviz $tag: $name"
    curl -fL --progress-bar -H "User-Agent: Openviz-installer" -o "$work/$name" "$url" || fail "the download did not finish. Try again."
    if [ "$sum" != "-" ]; then
        got="$(shasum -a 256 "$work/$name" | cut -d' ' -f1)"
        [ "$got" = "$sum" ] || fail "the download is not what GitHub says it should be (its checksum differs). Try again."
        say "ok         checked against GitHub's checksum"
    fi
    mkdir "$work/unpacked"
    ditto -x -k "$work/$name" "$work/unpacked" || fail "the download could not be unpacked."
    [ -f "$work/unpacked/Openviz/launcher/launcher.py" ] || fail "the download does not hold Openviz as expected."

    step "4. Putting Openviz in place"
    mkdir -p "$dest" || fail "$dest cannot be made."
    # What is built on this Mac and what you made with Openviz stay; everything else is the release's.
    rsync -a --delete --exclude '/library/' --exclude '/core/build/' --exclude 'Binaries/' --exclude 'Intermediate/' \
        --exclude 'Saved/' --exclude 'DerivedDataCache/' "$work/unpacked/Openviz/" "$dest/" || fail "the files could not be copied into $dest."
    say "ok         $dest"

    step "5. Building Openviz's core"
    /usr/bin/python3 "$dest/launcher/core_build.py" >/dev/null \
        || fail "the core did not build. In Terminal: python3 \"$dest/launcher/core_build.py\" --verbose  shows why."
    say "ok         $dest/core/build/vizcore_inspect"

    step "6. The Openviz app"
    app="$apps/Openviz.app"
    other=""
    [ -f "$app/Contents/Resources/repo-path.txt" ] && other="$(cat "$app/Contents/Resources/repo-path.txt")"
    if [ -e "$app" ] && [ ! -f "$app/Contents/Resources/repo-path.txt" ]; then
        say "skipped    $app is there and was not made by Openviz: it is left alone. Open Openviz with: python3 \"$dest/launcher/launcher.py\""
        app=""
    elif [ -n "$other" ] && [ "$other" != "$dest" ] && ! yes_no "The Openviz app in $apps opens the copy in $other. Make it open this one instead?"; then
        say "skipped    the app still opens $other. This copy opens with: python3 \"$dest/launcher/launcher.py\""
        app=""
    else
        sh "$dest/launcher/build_mac_app.sh" "$apps" >/dev/null || fail "the app could not be made. In Terminal: sh \"$dest/launcher/build_mac_app.sh\"  shows why."
        say "ok         $app"
    fi

    step "Done"
    if [ -n "$updating" ]; then say "Openviz is updated to $tag."; else say "Openviz $tag is installed in $dest."; fi
    say "The first time a show is watched, Openviz builds the visualizer: ten minutes to half an hour."
    if [ -n "$app" ] && [ -z "${OPENVIZ_NO_OPEN:-}" ]; then
        open "$app"
    fi
    finish 0
}

say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

# Questions go to the terminal itself, so they work when this script arrives through a pipe too.
# Whether there is a terminal to ask in (not so when run by another program).
has_terminal() {
    (: </dev/tty) 2>/dev/null
}

ask() {
    printf '%s ' "$1" >&2
    answer=""
    if has_terminal; then read -r answer </dev/tty || answer=""; else printf '\n' >&2; fi
    [ -n "$answer" ] && printf '%s' "$answer" || printf '%s' "$2"
}

# yes_no question [y]: the second argument is the answer when Return alone is pressed.
yes_no() {
    if [ "${2:-n}" = "y" ]; then
        case "$(ask "$1 [Y/n]" "y")" in [nN]*) return 1 ;; *) return 0 ;; esac
    fi
    case "$(ask "$1 [y/N]" "n")" in [yY]*) return 0 ;; *) return 1 ;; esac
}

fail() {
    printf '\nNot installed: %s\n' "$*"
    finish 1
}

finish() {
    # A window opened by double-clicking stays until it has been read.
    if [ -n "${OPENVIZ_DOUBLE_CLICKED:-}" ] && has_terminal; then
        printf '\nPress Return to close this window.'
        read -r _ </dev/tty || true
    fi
    exit "$1"
}

main "$@"
