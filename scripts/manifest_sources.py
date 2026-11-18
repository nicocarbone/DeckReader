#!/usr/bin/env python3
"""Print the sources of the Flatpak manifest, one per line, for shell scripts.

    manifest_sources.py MANIFEST

Output lines (tab separated):
    git     <module> <url> <commit>
    archive <module> <url> <sha256> <dest>
    file    <module> <url> <sha256> <dest>
    patch   <module> <path relative to the repository root>

The manifest is the single place where dependency versions are pinned;
scripts/dev-build.sh fetches the same sources for development builds. Only
the subset of YAML the manifest uses is understood (no PyYAML needed).
"""

import os
import re
import sys


def parse(text):
    """Returns [(module, {type, url, commit, sha256, dest}), ...]."""
    out = []
    module = None
    source = None
    in_modules = False
    for raw in text.splitlines():
        line = raw.split(" #", 1)[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if not line:
            continue
        if line == "modules:":
            in_modules = True
            continue
        if not in_modules:
            continue
        m = re.match(r"^  - name: *(\S+)$", line)
        if m:
            module = m.group(1)
            source = None
            continue
        m = re.match(r"^      - type: *(\S+)$", line)
        if m and module:
            source = {"type": m.group(1)}
            out.append((module, source))
            continue
        m = re.match(r"^        (url|commit|sha256|dest|tag|path): *(\S+)$", line)
        if m and source is not None:
            source[m.group(1)] = m.group(2)
    return out


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        sources = parse(f.read())
    for module, s in sources:
        if s["type"] == "git":
            print("\t".join(["git", module, s["url"], s["commit"]]))
        elif s["type"] == "patch":
            # Manifest paths are relative to packaging/flatpak/.
            print("\t".join(["patch", module, os.path.normpath(os.path.join("packaging/flatpak", s["path"]))]))
        elif s["type"] in ("archive", "file"):
            print("\t".join([s["type"], module, s["url"], s["sha256"], s.get("dest", ".")]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
