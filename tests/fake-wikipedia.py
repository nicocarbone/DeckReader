#!/usr/bin/env python3
"""A fake Wikipedia for the unit tests (D37): the reading-list API calls
DeckReader makes, as the ReadingLists extension answers them, and the
articles' pages as Parsoid's HTML, on 127.0.0.1. Pictures under /img/
(a.png). Pages are small but shaped like the real ones: sections,
data-mw attributes with '>' in them, template styles, a navigation box,
formulas as MathML, references and an "External links" section.

    python3 tests/fake-wikipedia.py     prints the port, then serves

The lists come two to a page and the entries two to a page, so that
paging is used (the real API: 10 and up to 100).

Test-only endpoints set it up and report what it saw:
    POST /test/reset  {"lists": [{"id", "name", "default"?}],
                       "entries": {"<list id>": [{"id", "project", "title", "created"}]},
                       "pages": ["Title", ...]  (the others answer 404),
                       "redirects": {"From": "To"}, "not_set_up": bool,
                       "cycle": [list ids whose "next" never ends]}
                      An entry with "deleted": true is sent as one (a tombstone).
                      The page "Svg_first" starts with an SVG picture.
    GET  /test/state  {"page_auth": pages asked for with a token (must be 0),
                       "pages": the titles asked for, "list_calls": n}
"""
import json
import re
import struct
import sys
import threading
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, unquote, urlsplit

TOKEN = "tok-wiki"
API = "/w/rest.php/readinglists/v0"
PER_PAGE = 2

lock = threading.Lock()
state = {"lists": [], "entries": {}, "pages": [], "redirects": {}, "not_set_up": False, "cycle": [],
         "page_auth": 0, "fetched": [], "list_calls": 0}


def png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


A_PNG = png(80, 60, (40, 120, 200))
B_SVG = (b'<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20">'
         b'<rect width="40" height="20" fill="#08f"/></svg>')


def page_html(title, port):
    t = title.replace("_", " ")
    lang = "es" if title.startswith("Buenos") else "en"
    img = f"//127.0.0.1:{port}/img/a.png"
    first = (f'<figure><img src="//127.0.0.1:{port}/img/b.svg" width="200" height="100"/></figure>'
             if title == "Svg_first" else "")
    return f"""<!DOCTYPE html>
<html prefix="dc: http://purl.org/dc/terms/ mw: http://mediawiki.org/rdf/"><head prefix="mwr: //x/"><meta charset="utf-8"/><meta property="dc:modified" content="2026-11-20T10:00:00Z"/><title>{t}</title><style>.mw-parser-output {{ color: red }}</style></head>
<body class="mw-content-ltr mw-parser-output parsoid-body" lang="{lang}" dir="ltr" data-mw-parsoid-version="0.25">
<section data-mw-section-id="0" id="mwAQ"><div class="shortdescription nomobile noexcerpt noprint searchaux" style="display:none">Short</div>
<div role="note" class="hatnote navigation-not-searchable">For other uses, see <a rel="mw:WikiLink" href="./Other">Other</a>.</div>
<style data-mw-deduplicate="TemplateStyles:r1" typeof="mw:Extension/templatestyles">.infobox {{ float: right }}</style>
{first}
<figure class="mw-default-size" typeof="mw:File/Thumb"><a href="./File:A.png" class="mw-file-description"><img src="{img}" srcset="{img} 2x" width="250" height="188" class="mw-file-element"/></a><figcaption>A picture</figcaption></figure>
<p id="mwBg"><b>{t}</b> is <math xmlns="http://www.w3.org/1998/Math/MathML" class="mwe-math-element" display="inline" data-mw='{{"name":"math","body":{{"extsrc":"a>b"}}}}' id="mwEg"><mi>e</mi><mo>=</mo><mn>1</mn></math>, see <a rel="mw:WikiLink" href="./Equality" title="Equality">equality</a>.<sup class="mw-ref reference" id="cite_ref-1" typeof="mw:Extension/ref" data-mw='{{"body":{{"html":"<b>x</b> > y"}}}}'><a href="./{quote(title)}#cite_note-1"><span class="mw-reflink-text">[1]</span></a></sup></p>
</section>
<section data-mw-section-id="1"><h2 id="History">History</h2><p>Some history.</p></section>
<section data-mw-section-id="2"><h2 id="References">References</h2><ol class="mw-references references"><li id="cite_note-1"><span class="mw-cite-backlink"><a href="./{quote(title)}#cite_ref-1">^</a></span> <span class="mw-reference-text">A source.</span></li></ol></section>
<section data-mw-section-id="3"><h2 id="External_links">External links</h2><ul><li><a rel="mw:ExtLink" href="https://example.org">Example</a></li></ul><div class="navbox" role="navigation">Navigation</div></section>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, status, body=b"", ctype="application/json", headers=()):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n).decode()

    @staticmethod
    def paged(items, q):
        start = int(q.get("next", "0") or 0)
        page = items[start:start + PER_PAGE]
        out = {"page": page}
        if start + PER_PAGE < len(items):
            out["next"] = str(start + PER_PAGE)
        return out

    def handle_any(self, method):
        url = urlsplit(self.path)
        path = url.path
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        port = self.server.server_address[1]
        if method == "GET" and path == "/img/a.png":
            return self.send(200, A_PNG, "image/png")
        if method == "GET" and path == "/img/b.svg":
            return self.send(200, B_SVG, "image/svg+xml")
        with lock:
            if method == "POST" and path == "/test/reset":
                cfg = json.loads(self.body())
                state.update(lists=cfg.get("lists", []), entries=cfg.get("entries", {}),
                             pages=cfg.get("pages", []), redirects=cfg.get("redirects", {}),
                             not_set_up=cfg.get("not_set_up", False), cycle=cfg.get("cycle", []),
                             page_auth=0, fetched=[], list_calls=0)
                return self.send(200, {})
            if method == "GET" and path == "/test/state":
                return self.send(200, {"page_auth": state["page_auth"], "pages": state["fetched"],
                                       "list_calls": state["list_calls"]})
            m = re.fullmatch(r"/w/rest\.php/v1/page/([^/]+)/html", path)
            if method == "GET" and m:
                title = unquote(m.group(1))
                if self.headers.get("Authorization"):
                    state["page_auth"] += 1
                state["fetched"].append(title)
                if title in state["redirects"]:
                    to = quote(state["redirects"][title])
                    return self.send(307, b"", "text/html", [("Location", f"/w/rest.php/v1/page/{to}/html")])
                if title not in state["pages"]:
                    return self.send(404, {"errorKey": "rest-nonexistent-title", "httpCode": 404})
                return self.send(200, page_html(title, port), "text/html; charset=utf-8")
            if not path.startswith(API + "/"):
                return self.send(404, {"errorKey": "rest-no-match", "httpCode": 404})
            if self.headers.get("Authorization") != "Bearer " + TOKEN:
                return self.send(401, {"errorKey": "rest-permission-denied-anon", "httpCode": 401,
                                       "httpReason": "Unauthorized"})
            state["list_calls"] += 1
            if state["not_set_up"]:
                return self.send(400, {"errorKey": "readinglists-db-error-not-set-up", "httpCode": 400})
            sub = path[len(API):]
            if method == "GET" and sub in ("/lists", "/lists/"):
                p = self.paged(state["lists"], q)
                out = {"lists": [{"id": l["id"], "name": l["name"], "default": l.get("default", False),
                                  "description": "", "created": "2025-01-01T00:00:00Z",
                                  "updated": "2025-01-01T00:00:00Z"} for l in p["page"]]}
                if "next" in p:
                    out["next"] = p["next"]
                if not q.get("next"):
                    out["continue-from"] = "2026-11-23T00:00:00Z"
                return self.send(200, out)
            m = re.fullmatch(r"/lists/(\d+)/entries/?", sub)
            if method == "GET" and m:
                lid = m.group(1)
                if not any(str(l["id"]) == lid for l in state["lists"]):
                    return self.send(404, {"errorKey": "readinglists-db-error-no-such-list", "httpCode": 404})
                items = state["entries"].get(lid, [])
                cycle = int(lid) in state["cycle"]  # a server whose "next" comes round again
                p = self.paged(items, {} if cycle else q)
                out = {"entries": [dict({"id": e["id"], "listId": int(lid), "project": e["project"],
                                         "title": e["title"], "created": e["created"], "updated": e["created"]},
                                        **({"deleted": True} if e.get("deleted") else {}))
                                   for e in p["page"]]}
                if "next" in p:
                    out["next"] = p["next"]
                if cycle:
                    out["next"] = "again"
                return self.send(200, out)
            return self.send(404, {"errorKey": "rest-no-match", "httpCode": 404})

    def do_GET(self):
        self.handle_any("GET")

    def do_POST(self):
        self.handle_any("POST")


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print(server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
