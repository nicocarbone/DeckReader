#!/usr/bin/env python3
"""Write deliberately malformed EPUBs for the archive-hardening tests (D5, D22).

    tests/make-hostile-epubs.py DIR

  nested.epub        a ZIP (no EPUB mimetype) holding another .epub
  size-mismatch.epub central directory says 1000 bytes, local header says
                     2 GB; the entry really inflates to 20 MB
  many-small.epub    40 entries just under 1 MB each, ~1000:1 compressed
  bad-zip64.epub     a ZIP64 extra field whose Size runs past the data

And small, well-formed books for rendering tests (D26):

  vs-with.epub       back-links written "↩︎" (U+21A9 U+FE0E), as Standard
                     Ebooks does, and "↩️" (U+FE0F)
  vs-without.epub    the same text without the variation selectors
  line-art.epub      one page holding a black-on-transparent PNG
"""

import os
import struct
import sys
import zipfile
import zlib

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


OPF = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Test</dc:title>
<dc:language>en</dc:language><dc:identifier id="id">deckreader-test</dc:identifier></metadata>
<manifest><item id="c" href="c.xhtml" media-type="application/xhtml+xml"/>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>%s</manifest>
<spine toc="ncx"><itemref idref="c"/></spine></package>"""

NCX = """<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/>
<docTitle><text>Test</text></docTitle><navMap><navPoint id="p" playOrder="1">
<navLabel><text>Notes</text></navLabel><content src="c.xhtml"/></navPoint></navMap></ncx>"""


def text_epub(path, paragraphs, images=()):
    """A one-chapter book; `images` are (name, PNG bytes) next to it."""
    body = "".join("<p>%s</p>" % p for p in paragraphs)
    chapter = ('<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml">'
               "<head><title>Notes</title></head><body>%s</body></html>" % body)
    items = "".join('<item id="i%d" href="%s" media-type="image/png"/>' % (i, n) for i, (n, _) in enumerate(images))
    epub(path, [("OEBPS/content.opf", OPF % items), ("OEBPS/toc.ncx", NCX), ("OEBPS/c.xhtml", chapter.encode())]
         + [("OEBPS/" + n, data) for n, data in images])


def png_rgba(w, h, pixel):
    """A PNG from pixel(x, y) -> (r, g, b, a)."""
    raw = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


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
    notes = ["%d. A note about chapter %d, as in an endnotes section. <a href=\"#\">\u21a9%s</a>" % (i, i, vs)
             for i in range(1, 13) for vs in ("\ufe0e", "\ufe0f")]
    text_epub(os.path.join(out, "vs-with.epub"), notes)
    text_epub(os.path.join(out, "vs-without.epub"), [n.replace("\ufe0e", "").replace("\ufe0f", "") for n in notes])
    # Black ink (a frame and a cross) on a transparent background, like an
    # engraving in a Standard Ebooks book.
    ink = lambda x, y: (0, 0, 0, 255) if min(x, y, 299 - x, 299 - y) < 8 or abs(x - y) < 5 else (0, 0, 0, 0)
    text_epub(os.path.join(out, "line-art.epub"), ['<img src="art.png" alt="Line art"/>'],
              [("art.png", png_rgba(300, 300, ink))])
    return 0


if __name__ == "__main__":
    sys.exit(main())
