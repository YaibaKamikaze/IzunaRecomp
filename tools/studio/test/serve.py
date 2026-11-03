#!/usr/bin/env python3
"""[dev] Serves the studio (/studio/) and a game folder (/game/) on 127.0.0.1,
with byte ranges, for looking at the pages in a browser that cannot pick
folders (fs.js, UrlFolder):

    python3 tools/studio/test/serve.py "/path/to/Game Data" [port]
    -> http://127.0.0.1:8765/studio/index.html?dev-game=http://127.0.0.1:8765/game/&page=files
"""
import http.server, os, re, sys, urllib.parse

STUDIO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.abspath(sys.argv[1])

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def path_of(self):
        p = urllib.parse.unquote(urllib.parse.urlparse(self.path).path)
        for prefix, root in (("/studio/", STUDIO), ("/game/", GAME)):
            if p.startswith(prefix):
                full = os.path.realpath(os.path.join(root, p[len(prefix):]))
                if full.startswith(os.path.realpath(root)) and os.path.isfile(full):
                    return full
        return None
    def do_HEAD(self): self.send(head=True)
    def do_GET(self): self.send()
    def send(self, head=False):
        f = self.path_of()
        if not f:
            self.send_response(404); self.end_headers(); return
        size = os.path.getsize(f)
        start, end = 0, size - 1
        m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        self.send_response(206 if m else 200)
        if m:
            start = int(m.group(1)); end = int(m.group(2)) if m.group(2) else end
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        types = {".html": "text/html", ".js": "text/javascript", ".css": "text/css"}
        self.send_header("Content-Type", types.get(os.path.splitext(f)[1], "application/octet-stream"))
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        if head: return
        with open(f, "rb") as fh:
            fh.seek(start); left = end - start + 1
            while left > 0:
                chunk = fh.read(min(left, 1 << 20)); self.wfile.write(chunk); left -= len(chunk)

http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 8765), H).serve_forever()
