#!/usr/bin/env python3
"""exocrawl test mock: serves a fake web for hermetic tests."""
import http.server
import json
import sys

PORT = int(sys.argv[1])

PAGE = """<!DOCTYPE html>
<html><head><title>Test Article Title</title></head>
<body>
<nav><a href="/nav1">Navigation</a></nav>
<div class="ad-banner">BUY NOW sponsors everywhere</div>
<h1>Main Heading Here</h1>
<p>First paragraph with an &amp; entity and a <a href="/wiki/thing">link to the thing</a>.</p>
<h2>Section Two</h2>
<ul><li>List item one</li><li>List item two</li></ul>
<pre>code = "verbatim &lt;not escaped&gt;"</pre>
<p>Trailing paragraph &copy; 2026.</p>
<footer>Copyright footer junk</footer>
<img src="/img/logo.png" alt="The Logo">
</body></html>"""

SEARXNG = json.dumps({
    "results": [
        {"url": "https://example.com/a", "title": "Result One", "content": "Snippet for result one."},
        {"url": "https://example.com/b", "title": "Result Two", "content": "Snippet for result two."},
    ]
})

DDG = """<html><body>
<div class="result"><a class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2Fpage&amp;rut=x">DDG Result Title</a><a class="result__snippet" href="x">DDG snippet text here</a></div>
</body></html>"""

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if "/html/" in self.path:
            body = PAGE.encode()
        elif "/search?" in self.path:
            body = SEARXNG.encode()
        elif "/ddg/" in self.path:
            body = DDG.encode()
        elif self.path.startswith("/deny"):
            self.send_response(403)
            self.end_headers()
            self.wfile.write(b"blocked")
            return
        else:
            body = b"not found"
            self.send_response(404)
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
