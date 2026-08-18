#!/usr/bin/env python3
"""mock_research.py - deterministic mock exocrawl for the exoflow suite.

Serves the two ops research-loop.sh uses:
  GET /search?q=..&n=..   -> `idx<TAB>title<TAB>url<TAB>snippet` lines,
                             the query echoed in every result so tests can
                             prove the query reached the engine.
  GET /fetch?url=..       -> a fixed page whose body may include one extra
                             line read from a state file (argv[2]); writing
                             the file is the test's way of changing the
                             world between refreshes.

Everything is deterministic (no timestamps, no counters), so a refresh
with an unchanged state file produces byte-identical pages and the
diffnote step must stay silent - exactly the property the tests assert.
"""
import http.server
import socketserver
import sys
import urllib.parse
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18990
STATE = sys.argv[2] if len(sys.argv) > 2 else "/tmp/research.state"


def state_line():
    try:
        with open(STATE, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/search":
            query = q.get("q", [""])[0]
            n = int(q.get("n", ["10"])[0])
            lines = [
                "%d\tMock result %d for %s\thttp://research.test/p%d\tMock snippet %d."
                % (i + 1, i + 1, query, i + 1, i + 1)
                for i in range(min(n, 3))
            ]
            body = "\n".join(lines) + "\n"
        elif u.path == "/fetch":
            url = q.get("url", [""])[0]
            path = urllib.parse.urlsplit(url).path
            extra = state_line()
            body = "# Mock page %s\nParagraph alpha.\nParagraph beta.\n" % path
            extra = state_line()
            if extra:
                body += extra + "\n"
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        data = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if urllib.parse.urlsplit(self.path).path == "/scrape":
            self.wfile.write(b"ok 0\n")
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("127.0.0.1", PORT), Handler) as srv:
    srv.serve_forever()