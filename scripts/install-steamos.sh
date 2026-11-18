#!/bin/bash
# Install DeckReader on SteamOS (run once from Desktop Mode, in Konsole).
#
#   scripts/install-steamos.sh [BUNDLE]   install/upgrade and add to Steam
#   scripts/install-steamos.sh --artwork  (re)install Steam library artwork
#   scripts/install-steamos.sh --uninstall
#
# BUNDLE defaults to dist/DeckReader.flatpak next to this script.
# Everything is installed per-user: nothing touches SteamOS's read-only
# system partition, so it survives SteamOS updates (requirements 3.6).
# Same steps as DeckVideoPlayer's installer, minus its HDR layer.

set -euo pipefail

APP_ID=io.github.nicocarbone.DeckReader
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/.." && pwd)
config_dir="$HOME/.config/deckreader"

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }

# This user's Steam only (on a shared PC another user's Steam doesn't matter).
steam_running() { pgrep -u "$(id -u)" -x steam >/dev/null 2>&1; }

# Uninstalls installed branches of the app (keeps settings and data):
# all of them, or all but KEEP (remove_installed_branches [KEEP]).
remove_installed_branches() {
    for ref in $(flatpak list --user --app --columns=ref 2>/dev/null | grep "^$APP_ID/"); do
        [ -n "${1:-}" ] && [ "${ref##*/}" = "$1" ] && continue
        flatpak uninstall --user -y --noninteractive "$ref" >/dev/null 2>&1 || true
    done
}

desktop_file() {
    local f
    for f in "$HOME/.local/share/flatpak/exports/share/applications/$APP_ID.desktop" \
             "/var/lib/flatpak/exports/share/applications/$APP_ID.desktop"; do
        [ -f "$f" ] && { echo "$f"; return 0; }
    done
    return 1
}

install_artwork() {
    # Steam writes shortcuts.vdf shortly after a shortcut is added.
    for _ in $(seq 1 15); do
        if python3 "$here/steam_shortcut.py" artwork >/dev/null 2>&1; then
            say "Steam library artwork installed (visible after restarting Steam)."
            return 0
        fi
        sleep 2
    done
    warn "Could not find the Steam shortcut yet. After restarting Steam run:"
    warn "  $0 --artwork"
}

add_to_steam() {
    if ! steam_running; then
        # Adds the shortcut to every Steam profile that lacks it, unpins an
        # existing one from a Flatpak branch and installs the artwork.
        say "Adding to Steam (or updating the shortcut)..."
        python3 "$here/steam_shortcut.py" add
        return 0
    fi
    if python3 "$here/steam_shortcut.py" status 2>/dev/null | grep -q 'app id'; then
        say "Already in your Steam library."
        python3 "$here/steam_shortcut.py" repair >/dev/null || true
        python3 "$here/steam_shortcut.py" artwork >/dev/null 2>&1 || true
        return 0
    fi
    if command -v steamos-add-to-steam >/dev/null 2>&1 && desktop=$(desktop_file); then
        say "Adding to Steam..."
        steamos-add-to-steam "$desktop"
        install_artwork
    else
        warn "Steam is running: close Steam and run this script again,"
        warn "or add it manually: Steam > Add a Game > Add a Non-Steam Game > DeckReader."
    fi
}

case "${1:-}" in
    --artwork)
        python3 "$here/steam_shortcut.py" artwork
        exit 0
        ;;
    --uninstall)
        if steam_running; then
            warn "Close Steam first so the shortcut can be removed."
        else
            python3 "$here/steam_shortcut.py" remove || true
        fi
        remove_installed_branches
        say "Uninstalled. Your settings remain in $config_dir and your reading"
        say "positions in ~/.var/app/$APP_ID (delete them by hand if you want)."
        exit 0
        ;;
esac

bundle=${1:-"$root/dist/DeckReader.flatpak"}
if [ ! -f "$bundle" ]; then
    echo "Flatpak bundle not found: $bundle" >&2
    echo "Download DeckReader.flatpak or build it with scripts/build-flatpak.sh" >&2
    exit 1
fi

say "Setting up Flathub (user installation)..."
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

say "Installing DeckReader..."
# Other branches would install next to this one and `flatpak run` could keep
# starting an old copy (DeckVideoPlayer learned this the hard way, its D44).
remove_installed_branches stable
flatpak install --user -y --reinstall "$bundle"

if [ ! -d "$HOME/Books" ]; then
    mkdir -p "$HOME/Books"
    say "Created ~/Books: copy your .epub files there (or onto the microSD card)."
fi

add_to_steam

cat <<EOF

Done! Return to Gaming Mode and launch "DeckReader" from your library
(Non-Steam section).

Tips:
  * Books go in ~/Books or on the microSD card; other folders are set in
    $config_dir/config.ini (created on the first start).
  * On an external 4K screen, set the game's Properties > Game Resolution to
    "Native" so pages are rendered at full resolution.
EOF
