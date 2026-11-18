#!/usr/bin/env python3
"""Builds the "corresponding source" archive for a release (GPL compliance).

    scripts/make-source-archive.py OUT.tar.gz [--manifest PATH]

The Flatpak bundle we publish contains GPL software (DeckReader itself and
the crengine-ng book engine, statically linked). Whoever distributes those
binaries must make their exact source available, so every release carries
this archive next to the bundle (docs/DECISIONS.md D27, as DeckVideoPlayer's
D36).

It contains, for every module of the Flatpak manifest:
  - git sources: the pinned commit, with its submodules (flatpak-builder
    builds them too), without .git directories;
  - patches: the files the build applies, next to the module they patch;
  - archive/file sources: the original download, checksum-verified;
  - the app itself (the `dir` source): `git archive` of this repository's
    HEAD, i.e. the tagged release when run by CI.
plus the manifest and a SOURCES.md index. The Freedesktop runtime is not
included: it is distributed by Flathub, which provides its source.

Needs git and curl. The manifest is read with scripts/manifest_sources.py
(no PyYAML).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_MANIFEST = os.path.join(ROOT, "packaging", "flatpak", "io.github.nicocarbone.DeckReader.yml")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import manifest_sources  # noqa: E402  (scripts/manifest_sources.py)

# No automatic gc / maintenance: a background git process could still be
# writing in .git (e.g. .git/modules) while it is removed below. The
# environment form also reaches the git processes of submodule updates.
GIT_ENV = dict(os.environ, GIT_CONFIG_COUNT="2",
               GIT_CONFIG_KEY_0="gc.auto", GIT_CONFIG_VALUE_0="0",
               GIT_CONFIG_KEY_1="maintenance.auto", GIT_CONFIG_VALUE_1="false")


def run(*cmd, cwd=None):
    env = GIT_ENV if cmd[0] == "git" else None
    subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.DEVNULL, env=env)


def remove_tree(path):
    """shutil.rmtree, retried briefly if something still writes in it."""
    for attempt in range(5):
        try:
            shutil.rmtree(path)
            return
        except OSError:
            if attempt == 4:
                raise
            time.sleep(2)


def log(*a):
    print("[sources]", *a, flush=True)


def fetch_git(url, commit, dest):
    os.makedirs(dest)
    run("git", "init", "-q", cwd=dest)
    run("git", "remote", "add", "origin", url, cwd=dest)
    run("git", "fetch", "-q", "--depth", "1", "origin", commit, cwd=dest)
    run("git", "checkout", "-q", "FETCH_HEAD", cwd=dest)
    if os.path.exists(os.path.join(dest, ".gitmodules")):
        run("git", "submodule", "update", "-q", "--init", "--recursive", "--depth", "1", cwd=dest)
    for dirpath, dirnames, filenames in os.walk(dest):
        if ".git" in dirnames:
            remove_tree(os.path.join(dirpath, ".git"))
            dirnames.remove(".git")
        if ".git" in filenames:  # submodule gitlink file
            os.remove(os.path.join(dirpath, ".git"))


def fetch_file(url, sha256, dest_dir):
    os.makedirs(dest_dir, exist_ok=True)
    path = os.path.join(dest_dir, os.path.basename(url.rstrip("/")))
    run("curl", "-fsSL", "--retry", "3", "-o", path, url)
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    if sha256 and digest != sha256:
        sys.exit(f"checksum mismatch for {url}: {digest} != {sha256}")
    return os.path.basename(path)


def main():
    ap = argparse.ArgumentParser(description="Build the corresponding-source archive")
    ap.add_argument("out")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    args = ap.parse_args()
    text = open(args.manifest, encoding="utf-8").read()
    m = re.search(r"^id: *(\S+)", text, re.M)
    app_id = m.group(1) if m else "app"
    manifest_dir = os.path.dirname(os.path.abspath(args.manifest))
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True,
                          check=True).stdout.strip()

    work = tempfile.mkdtemp(prefix="deckreader-sources-")
    top = os.path.join(work, f"{app_id}-sources")
    os.makedirs(top)
    index = [f"# Corresponding source for {app_id}\n",
             f"App source: this repository at commit `{head}` (in `deckreader/`).\n",
             "Third-party modules, exactly as pinned in the Flatpak manifest "
             f"(`{os.path.basename(args.manifest)}`, included):\n",
             "| Module | Source | Revision / SHA-256 | Folder |", "|---|---|---|---|"]
    git_folder = {}  # module -> folder of its git checkout, for its patches
    try:
        for name, src in manifest_sources.parse(text):
            kind = src.get("type")
            if kind == "git":
                folder = f"{name}-{src['commit'][:12]}"
                git_folder[name] = folder
                log("git", name, src["url"], src["commit"][:12])
                fetch_git(src["url"], src["commit"], os.path.join(top, folder))
                index.append(f"| {name} | {src['url']} (tag {src.get('tag', '-')}) | `{src['commit']}` | `{folder}/` |")
            elif kind == "patch":
                # Applied by flatpak-builder in manifest order (`patch -p1`).
                path = os.path.normpath(os.path.join(manifest_dir, src["path"]))
                folder = os.path.join(f"{git_folder.get(name, name)}.patches")
                os.makedirs(os.path.join(top, folder), exist_ok=True)
                shutil.copy(path, os.path.join(top, folder))
                log("patch", name, os.path.basename(path))
                index.append(f"| {name} | patch, applied in this order | `{os.path.basename(path)}` | "
                             f"`{folder}/{os.path.basename(path)}` |")
            elif kind in ("archive", "file"):
                log(kind, name, src["url"])
                got = fetch_file(src["url"], src.get("sha256"), os.path.join(top, name))
                index.append(f"| {name} | {src['url']} | `{src.get('sha256', '')}` | `{name}/{got}` |")
            elif kind == "dir":
                log("app", name, "git archive", head[:12])
                dest = os.path.join(top, "deckreader")
                os.makedirs(dest)
                tar = subprocess.run(["git", "-C", ROOT, "archive", "--format=tar", "HEAD"],
                                     capture_output=True, check=True).stdout
                subprocess.run(["tar", "-x", "-C", dest], input=tar, check=True)
            else:
                sys.exit(f"unsupported source type {kind!r} in module {name}")
        shutil.copy(args.manifest, top)
        index.append("\nThe Freedesktop Platform runtime (org.freedesktop.Platform: SDL3, FreeType, "
                     "HarfBuzz, FriBiDi, libpng, libjpeg, libwebp, zstd, zlib, fontconfig) is "
                     "distributed by Flathub, which publishes its source.\n"
                     "Licences of each component are in its folder; the built Flatpak also "
                     "carries them in /app/share/licenses/.\n")
        with open(os.path.join(top, "SOURCES.md"), "w") as f:
            f.write("\n".join(index) + "\n")
        log("writing", args.out)
        with tarfile.open(args.out, "w:gz") as tar:
            tar.add(top, arcname=os.path.basename(top))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    log("done:", f"{os.path.getsize(args.out) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
