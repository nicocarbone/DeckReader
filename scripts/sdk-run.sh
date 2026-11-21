#!/bin/sh
# Run a command inside the Freedesktop SDK that the Flatpak is built on, with
# the source tree mounted at the same path. Development builds and runs use
# this, so they link against exactly the libraries the Flatpak ships (SDL3,
# FreeType, HarfBuzz, ...) and nothing needs installing on the host (D7).
#
#   scripts/sdk-run.sh <command> [args...]       build tools, tests
#   GUI=1 scripts/sdk-run.sh <command> [args...] also display, GPU, input
#   NET=1 scripts/sdk-run.sh <command> [args...] also the network, as the
#                                  Flatpak has it (wallabag, D31)
#
# Without NET=1 the sandbox has only its own loopback: the unit tests' fake
# wallabag server works, nothing else does.
#
# Extra flatpak-run options can be passed in SDK_RUN_ARGS.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUNTIME_BRANCH=26.08

if [ $# -eq 0 ]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi
if ! flatpak info "org.freedesktop.Sdk//$RUNTIME_BRANCH" >/dev/null 2>&1; then
    echo "The Freedesktop SDK $RUNTIME_BRANCH is not installed. Install it with:" >&2
    echo "  flatpak install --user flathub org.freedesktop.Sdk//$RUNTIME_BRANCH" >&2
    exit 1
fi

command=$1
shift
opts="--filesystem=$root --cwd=$PWD"
if [ "${GUI:-0}" = 1 ]; then
    opts="$opts --socket=x11 --socket=wayland --share=ipc --device=all --filesystem=/run/media"
fi
if [ "${NET:-0}" = 1 ]; then
    opts="$opts --share=network"
fi
# Word splitting of $opts is intended; $root/$PWD may contain non-ASCII but
# no spaces (checked here).
case "$root$PWD" in
    *" "*) echo "sdk-run.sh: paths with spaces are not supported: $root" >&2; exit 1 ;;
esac
# shellcheck disable=SC2086
exec flatpak run $opts ${SDK_RUN_ARGS:-} --command="$command" "org.freedesktop.Sdk//$RUNTIME_BRANCH" "$@"
