#!/usr/bin/env python3
"""A fake wallabag server for the unit tests (D31): the API calls DeckReader
makes, as wallabag 2.6 answers them, on 127.0.0.1, and the articles'
pictures under /img/ (a.png, b.svg, cover.png; c.avif is a format the
engine can't show, missing.png doesn't exist, r.png redirects to a.png).

    python3 tests/fake-wallabag.py     prints the port, then serves

Test-only endpoints set it up and report what it saw:
    POST /test/reset  {"entries": [{"id", "title", "created_at", "content"?,
                                    "preview_picture"?, "published_by"?}],
                       "broken": [ids], "delay": seconds per article (progress),
                       "max_per_page": a limit on perPage, as some servers have}
    GET  /test/state  {"archived": [...], "list_query": the last one, "list_calls": n,
                       "tokens": n,
                       "picture_auth": pictures asked for with a token (must be 0)}
    GET  /test/stall  sends one byte, then nothing for 5 s (a stalled site)
Anything under /old/ is redirected to the same path without it (a moved
server). The account: client "cid"/"secret", user "reader", password
PASSWORD below (characters that need encoding).
"""
import json
import re
import struct
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

CLIENT_ID, CLIENT_SECRET = "cid", "secret"
USERNAME, PASSWORD = "reader", "pa ss&wörd=1+2%"
TOKEN = "tok-123"

lock = threading.Lock()
state = {"entries": [], "broken": [], "archived": [], "list_query": "", "list_calls": 0, "tokens": 0,
         "delay": 0, "max_per_page": 0, "picture_auth": 0}


def png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


PICTURES = {
    "a.png": (png(64, 32, (200, 80, 40)), "image/png"),
    "cover.png": (png(60, 90, (40, 120, 200)), "image/png"),
    "b.svg": (b'<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20">'
              b'<rect width="40" height="20" fill="#08f"/></svg>', "image/svg+xml"),
    "c.avif": (b"\x00\x00\x00\x1cftypavif" + b"\x00" * 40, "image/avif"),
}


def article_json(entry, port):
    eid = entry["id"]
    return {"is_archived": 0, "is_starred": 0, "user_name": USERNAME, "id": eid,
            "title": entry.get("title"), "url": f"http://127.0.0.1:{port}/articles/{eid}",
            "content": entry.get("content", f"<p>Article {eid}.</p>"),
            "created_at": entry["created_at"], "updated_at": entry["created_at"],
            "published_at": entry.get("published_at"), "published_by": entry.get("published_by"),
            "domain_name": "www.example.com", "language": entry.get("language", "en_GB"),
            "reading_time": entry.get("reading_time", 3), "mimetype": "text/html",
            "preview_picture": entry.get("preview_picture"), "tags": [], "annotations": []}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, status, body=b"", ctype="application/json", headers=()):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
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

    def authorised(self):
        if self.headers.get("Authorization") == "Bearer " + TOKEN:
            return True
        self.send(401, {"error": "invalid_grant", "error_description": "The access token provided is invalid."})
        return False

    def handle_any(self, method):
        url = urlsplit(self.path)
        path = url.path
        port = self.server.server_address[1]
        if path.startswith("/old/"):
            self.send(301, b"", "text/html", [("Location", f"http://127.0.0.1:{port}{path[4:]}")])
            return
        if method == "GET" and path == "/test/stall":
            try:
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", "1000")
                self.end_headers()
                self.wfile.write(b"\x89")
                self.wfile.flush()
                time.sleep(5)
                self.wfile.write(b"\x00" * 999)
            except OSError:
                pass  # the client gave up, as it should
            return
        m = re.fullmatch(r".*/img/([\w.]+)", path)
        if method == "GET" and m:
            with lock:
                if self.headers.get("Authorization"):
                    state["picture_auth"] += 1
            name = m.group(1)
            if name == "r.png":
                return self.send(302, b"", "text/html", [("Location", f"http://127.0.0.1:{port}/img/a.png")])
            if name not in PICTURES:
                return self.send(404, b"<html>Not found</html>", "text/html")
            data, ctype = PICTURES[name]
            return self.send(200, data, ctype)
        with lock:
            if method == "POST" and path == "/test/reset":
                cfg = json.loads(self.body())
                state.update(entries=cfg.get("entries", []), broken=cfg.get("broken", []),
                             delay=cfg.get("delay", 0), max_per_page=cfg.get("max_per_page", 0),
                             archived=[], list_query="", list_calls=0, tokens=0, picture_auth=0)
                return self.send(200, {})
            if method == "GET" and path == "/test/state":
                return self.send(200, {k: state[k] for k in ("archived", "list_query", "list_calls", "tokens",
                                                             "picture_auth")})
            if method == "POST" and path == "/oauth/v2/token":
                f = {k: v[0] for k, v in parse_qs(self.body(), keep_blank_values=True).items()}
                if f.get("grant_type") != "password":
                    return self.send(400, {"error": "unsupported_grant_type"})
                if f.get("client_id") != CLIENT_ID or f.get("client_secret") != CLIENT_SECRET:
                    return self.send(400, {"error": "invalid_client", "error_description": "The client credentials are invalid"})
                if f.get("username") != USERNAME or f.get("password") != PASSWORD:
                    return self.send(400, {"error": "invalid_grant", "error_description": "Invalid username and password combination"})
                state["tokens"] += 1
                return self.send(200, {"access_token": TOKEN, "expires_in": 3600, "token_type": "bearer",
                                       "scope": None, "refresh_token": "refresh-456"})
            if not path.startswith("/api/"):
                return self.send(404, {"error": "not found"})
            if not self.authorised():
                return
            if method == "GET" and path == "/api/entries.json":
                state["list_query"] = url.query
                state["list_calls"] += 1
                q = {k: v[0] for k, v in parse_qs(url.query).items()}
                items = [e for e in state["entries"] if e["id"] not in state["archived"]]
                if q.get("archive") == "1":
                    items = [e for e in state["entries"] if e["id"] in state["archived"]]
                items.sort(key=lambda e: e["created_at"], reverse=q.get("order", "desc") == "desc")
                per = int(q.get("perPage", 30))
                if state["max_per_page"]:
                    per = min(per, state["max_per_page"])
                number = int(q.get("page", 1))
                pages = max(1, -(-len(items) // per))
                out = []
                for e in items[(number - 1) * per:number * per]:
                    item = {"is_archived": 0, "is_starred": 0, "user_name": USERNAME, "id": e["id"],
                            "uid": None, "title": e.get("title"), "url": "https://example.com/%d" % e["id"],
                            "created_at": e["created_at"], "updated_at": e["created_at"],
                            "domain_name": "example.com", "reading_time": 3, "tags": [], "annotations": []}
                    if q.get("detail", "full") == "full":
                        item["content"] = "<p>...</p>"
                    out.append(item)
                return self.send(200, {"page": number, "limit": per, "pages": pages, "total": len(items),
                                       "_links": {}, "_embedded": {"items": out}})
            m = re.fullmatch(r"/api/entries/(\d+)\.json", path)
            if method == "GET" and m:
                eid = int(m.group(1))
                entry = next((e for e in state["entries"] if e["id"] == eid), None)
                if entry is None:
                    return self.send(404, {"error": "not found"})
                if eid in state["broken"]:
                    return self.send(500, b"<html>Internal error</html>", "text/html")
                time.sleep(state["delay"])
                return self.send(200, article_json(entry, port))
            if method == "PATCH" and m:
                eid = int(m.group(1))
                f = {k: v[0] for k, v in parse_qs(self.body()).items()}
                if not any(e["id"] == eid for e in state["entries"]):
                    return self.send(404, {"error": "not found"})
                if f.get("archive") == "1" and eid not in state["archived"]:
                    state["archived"].append(eid)
                return self.send(200, {"id": eid, "is_archived": 1})
            return self.send(404, {"error": "not found"})

    def do_GET(self):
        self.handle_any("GET")

    def do_POST(self):
        self.handle_any("POST")

    def do_PATCH(self):
        self.handle_any("PATCH")


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print(server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
