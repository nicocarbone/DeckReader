#!/bin/sh
# Development build: fetch the dependencies pinned in the Flatpak manifest,
# build them and the app inside the Freedesktop SDK (scripts/sdk-run.sh).
#
#   scripts/dev-build.sh            build (incremental)
#   scripts/dev-build.sh --clean    rebuild the app from scratch
#   BUILD_TYPE=Debug scripts/dev-build.sh
#
# Layout (all git-ignored):
#   .dev/src/<module>   dependency sources at the manifest's commits
#   .dev/dl/            downloaded font archives (checksum-verified)
#   .dev/prefix/        built dependencies
#   .dev/build/         the app's CMake build tree
#   .dev/app/           the app installed like /app in the Flatpak:
#                       bin/deckreader, share/deckreader/{fonts,js,...}

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
manifest=packaging/flatpak/io.github.nicocarbone.DeckReader.yml
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
dev=$root/.dev
mkdir -p "$dev/src" "$dev/dl" "$dev/prefix" "$dev/app/share/deckreader/fonts"

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# --- dependencies (inside the SDK) ----------------------------------------
build_deps() {
    # Runs inside the SDK. Each module is rebuilt only when its commit changes.
    set -eu
    prefix=$dev/prefix
    export PKG_CONFIG_PATH=$prefix/lib/pkgconfig
    export CMAKE_PREFIX_PATH=$prefix
    # Rebuilt when the module's commit or its patches change.
    stamp() {
        phash=$(grep -P "^patch\t$1\t" "$dev/sources.tsv" | cut -f3 | (cd "$root" && xargs -r cat) | sha256sum | cut -c1-12)
        echo "$prefix/.stamp-$1-$(git -C "$dev/src/$1" rev-parse HEAD)-$phash"
    }

    if [ ! -f "$(stamp libunibreak)" ]; then
        echo "==> Building libunibreak"
        cd "$dev/src/libunibreak"
        [ -x configure ] || ./autogen.sh >/dev/null
        ./configure -q --prefix="$prefix" --disable-static >/dev/null
        make -s -j"$(nproc)" install >/dev/null
        git clean -qfdx
        touch "$(stamp libunibreak)"
    fi
    if [ ! -f "$(stamp utf8proc)" ]; then
        echo "==> Building utf8proc"
        cmake -S "$dev/src/utf8proc" -B "$dev/depbuild/utf8proc" -G Ninja -Wno-dev \
            -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DUTF8PROC_ENABLE_TESTING=OFF \
            -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
        cmake --build "$dev/depbuild/utf8proc" --target install >/dev/null
        touch "$(stamp utf8proc)"
    fi
    if [ ! -f "$(stamp crengine-ng)" ]; then
        echo "==> Building crengine-ng (a few minutes)"
        cmake -S "$dev/src/crengine-ng" -B "$dev/depbuild/crengine-ng" -G Ninja -Wno-dev \
            -DCMAKE_BUILD_TYPE=Release \
            -DOFFLINE_BUILD_MODE=ON -DUSE_CHM=OFF -DUSE_ANTIWORD=OFF -DWITH_LIBJXL=OFF -DWITH_XXHASH=OFF \
            -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
        cmake --build "$dev/depbuild/crengine-ng" --target install >/dev/null
        touch "$(stamp crengine-ng)"
    fi
    if [ ! -f "$(stamp quickjs)" ]; then
        echo "==> Building QuickJS"
        cmake -S "$dev/src/quickjs" -B "$dev/depbuild/quickjs" -G Ninja -Wno-dev \
            -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DQJS_BUILD_LIBC=OFF \
            -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
        cmake --build "$dev/depbuild/quickjs" --target install >/dev/null
        touch "$(stamp quickjs)"
    fi
}

# --- the app (inside the SDK) ---------------------------------------------
build_app() {
    set -eu
    prefix=$dev/prefix
    export PKG_CONFIG_PATH=$prefix/lib/pkgconfig
    cmake -S "$root" -B "$dev/build" -G Ninja \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_PREFIX_PATH="$prefix" \
        -DCMAKE_INSTALL_PREFIX="$dev/app" -DCMAKE_INSTALL_RPATH="$prefix/lib" \
        -DDECKREADER_CRENGINE_DATA="$prefix/share/crengine-ng" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
    cmake --build "$dev/build"
    cmake --install "$dev/build" >/dev/null
}

# Inside the SDK: run one stage and stop.
if [ "${DEVBUILD_IN_SDK:-0}" = 1 ]; then
    "$2"
    exit 0
fi


[ "${1:-}" = --clean ] && rm -rf "$dev/build"

# --- 1. sources (host: needs network) ---------------------------------------
python3 scripts/manifest_sources.py "$manifest" > "$dev/sources.tsv"
tab=$(printf '\t')
while IFS="$tab" read -r type module url a b; do
    case "$type" in
        git)
            dir=$dev/src/$module
            if [ ! -d "$dir/.git" ]; then
                say "Fetching $module"
                git clone -q "$url" "$dir"
            fi
            if [ "$(git -C "$dir" rev-parse HEAD)" != "$a" ]; then
                git -C "$dir" checkout -q -- . # drop patches of the old commit
                git -C "$dir" fetch -q origin || true
                git -C "$dir" checkout -q "$a"
            fi
            ;;
        patch)
            # Applied to the checkout like flatpak-builder does: once, in
            # order. .dev/src/.patches-<module> records the set applied (and
            # the commit); when it changes, the checkout starts again from
            # the commit, as one patch may change lines next to another's
            # and they can't be undone one by one.
            if [ "$module" != "${patched_module:-}" ]; then
                patched_module=$module
                phash=$( (git -C "$dev/src/$module" rev-parse HEAD; grep -P "^patch\t$module\t" "$dev/sources.tsv" |
                          cut -f3 | xargs -r cat) | sha256sum | cut -c1-12)
                mark=$dev/src/.patches-$module
                if [ "$(cat "$mark" 2>/dev/null)" = "$phash" ]; then
                    patches_done=1
                else
                    patches_done=0
                    git -C "$dev/src/$module" checkout -q -- .
                    echo "$phash" > "$mark"
                fi
            fi
            [ "$patches_done" = 1 ] && continue
            say "Patching $module: $(basename "$url")"
            if ! git -C "$dev/src/$module" apply "$root/$url"; then
                echo "Patch does not apply to $module: $url" >&2
                rm -f "$dev/src/.patches-$module"
                exit 1
            fi
            ;;
        archive|file)
            case "$module" in
                fonts) out=$dev/dl/$(basename "$url") ;;
                # Their own folders: stix-math's OFL.txt is not Atkinson's.
                stix-math|temml) mkdir -p "$dev/dl/$module"; out=$dev/dl/$module/$(basename "$url") ;;
                *) continue ;;
            esac
            if [ ! -f "$out" ] || [ "$(sha256sum "$out" | cut -d' ' -f1)" != "$a" ]; then
                say "Downloading $(basename "$url")"
                curl -sfL -o "$out.tmp" "$url"
                if [ "$(sha256sum "$out.tmp" | cut -d' ' -f1)" != "$a" ]; then
                    echo "Checksum mismatch for $url" >&2
                    exit 1
                fi
                mv "$out.tmp" "$out"
            fi
            ;;
    esac
done < "$dev/sources.tsv"

# --- 2. fonts (same files the manifest's "fonts" module installs) -----------
fonts=$dev/app/share/deckreader/fonts
if [ ! -f "$fonts/.done" ]; then
    say "Unpacking fonts"
    tmp=$dev/dl/unpack
    rm -rf "$tmp" && mkdir -p "$tmp"
    unzip -q -o "$dev/dl/Inter-4.1.zip" -d "$tmp/inter"
    unzip -q -o "$dev/dl/3.103.zip" -d "$tmp/literata"
    for s in Regular Italic Bold BoldItalic; do
        cp "$tmp/inter/extras/ttf/Inter-$s.ttf" "$fonts/"
        cp "$tmp/literata/fonts/ttf/Literata-$s.ttf" "$fonts/"
        cp "$dev/dl/AtkinsonHyperlegible-$s.ttf" "$fonts/"
    done
    rm -rf "$tmp"
    touch "$fonts/.done"
fi
# The math font and Temml (the manifest's stix-math and temml modules, D36).
cmp -s "$dev/dl/stix-math/STIXTwoMath-Regular.otf" "$fonts/STIXTwoMath-Regular.otf" ||
    cp "$dev/dl/stix-math/STIXTwoMath-Regular.otf" "$fonts/"
temml_tgz=$dev/dl/temml/$(basename "$(awk -F'\t' '$1 == "archive" && $2 == "temml" {print $3}' "$dev/sources.tsv")")
js=$dev/app/share/deckreader/js
if [ ! -f "$js/temml.min.js" ] || [ "$temml_tgz" -nt "$js/temml.min.js" ]; then
    say "Unpacking Temml"
    mkdir -p "$js"
    tar -xzf "$temml_tgz" -O package/dist/temml.min.js > "$js/temml.min.js.tmp"
    mv "$js/temml.min.js.tmp" "$js/temml.min.js"
fi

# --- 3. dependencies and app, inside the SDK ----------------------------------
scripts/sdk-run.sh env DEVBUILD_IN_SDK=1 dev="$dev" root="$root" BUILD_TYPE="$BUILD_TYPE" \
    sh "$root/scripts/dev-build.sh" --stage build_deps
scripts/sdk-run.sh env DEVBUILD_IN_SDK=1 dev="$dev" root="$root" BUILD_TYPE="$BUILD_TYPE" \
    sh "$root/scripts/dev-build.sh" --stage build_app
say "Built: .dev/app/bin/deckreader  (run it with scripts/dev-run.sh)"
