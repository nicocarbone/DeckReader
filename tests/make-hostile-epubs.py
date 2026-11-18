#!/usr/bin/env python3
"""Write deliberately malformed EPUBs for the archive-hardening tests (D5, D22).

    tests/make-hostile-epubs.py DIR

  nested.epub        a ZIP (no EPUB mimetype) holding another .epub
  size-mismatch.epub central directory says 1000 bytes, local header says
                     2 GB; the entry really inflates to 20 MB
  many-small.epub    40 entries just under 1 MB each, ~1000:1 compressed
  bad-zip64.epub     a ZIP64 extra field whose Size runs past the data
"""

import os
import struct
import sys
import zipfile

CONTAINER = (b'<?xml version="1.0"?><container version="1.0" '
             b'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
             b'<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
             b'</rootfiles></container>')


def epub(path, extra):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
        z.writestr("META-INF/container.xml", CONTAINER)
        for name, data in extra:
            z.writestr(name, data)


def patch_sizes(path, entry, central_size, local_size):
    """Rewrite the uncompressed size of `entry` in the central directory and
    in its local header to different values."""
    data = bytearray(open(path, "rb").read())
    name = entry.encode()
    # Central directory header: signature, ..., uncompressed size at +24,
    # name length at +28, name at +46, local header offset at +42.
    c = 0
    while True:
        c = data.find(b"PK\x01\x02", c)
        assert c >= 0, "entry not found"
        nlen = struct.unpack_from("<H", data, c + 28)[0]
        if data[c + 46:c + 46 + nlen] == name:
            break
        c += 4
    local = struct.unpack_from("<I", data, c + 42)[0]
    struct.pack_into("<I", data, c + 24, central_size)
    struct.pack_into("<I", data, local + 22, local_size)
    open(path, "wb").write(bytes(data))


def bad_zip64(path):
    """A stored entry whose local header claims ZIP64 sizes, with an extra
    field whose Size (28) runs past the 8 bytes actually present."""
    name = b"OEBPS/x.xhtml"
    body = b"<html/>"
    extra = struct.pack("<HH", 0x0001, 28) + b"\x01" * 8
    local = struct.pack("<IHHHHHIIIHH", 0x04034B50, 45, 0, 0, 0, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, len(name), len(extra))
    entry = local + name + extra + body
    central = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 45, 45, 0, 0, 0, 0, 0, len(body), len(body),
                          len(name), 0, 0, 0, 0, 0, 0) + name
    end = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 1, 1, len(central), len(entry), 0)
    open(path, "wb").write(entry + central + end)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(os.path.join(out, "nested.epub"), "w") as z:
        z.writestr("readme.txt", "hi")
        z.writestr("inner.epub", b"PK\x05\x06" + b"\0" * 18)
    epub(os.path.join(out, "size-mismatch.epub"), [("OEBPS/ch1.xhtml", b"a" * (20 << 20))])
    patch_sizes(os.path.join(out, "size-mismatch.epub"), "OEBPS/ch1.xhtml", 1000, 0x7FFFFFFF)
    epub(os.path.join(out, "many-small.epub"), [("OEBPS/c%02d.xhtml" % i, b"\0" * 900000) for i in range(40)])
    bad_zip64(os.path.join(out, "bad-zip64.epub"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
