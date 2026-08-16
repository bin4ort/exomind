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
<div class="result"><a class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fad.example%2Fsponsored&amp;rut=y">Sponsored Ad</a><a class="result__snippet" href="x">buy now</a></div>
</body></html>"""

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/ddg/"):
            body = DDG.encode()
        elif "/html/" in self.path:
            body = PAGE.encode()
        elif self.path.startswith("/mojeek/"):
            body = ('<html><body><ul class="results-standard">'
                    '<li class="result"><a class="title" href="https://mojeek.example/one">Mojeek One</a>'
                    '<p class="s">Mojeek snippet one</p></li>'
                    '</ul></body></html>').encode()
        elif self.path.startswith("/marginalia/"):
            body = ('<html><body><ul><li class="results-item">'
                    '<a class="title" href="https://marg.example/x">Marginalia X</a>'
                    '<div class="description">Marginalia snippet x</div></li>'
                    '</ul></body></html>').encode()
        elif self.path.startswith("/bing/"):
            body = ('<html><body><ol id="b_results"><li class="b_algo">'
                    '<h2><a href="https://bing.example/b">Bing B</a></h2>'
                    '<p>Bing snippet b</p></li></ol></body></html>').encode()
        elif self.path.startswith("/wikipedia/"):
            body = ('["query",["Wiki One","Wiki Two"],["Wiki description one","Wiki description two"],'
                    '["https://wiki.example/One","https://wiki.example/Two"]]').encode()
        elif "/search?" in self.path:
            body = SEARXNG.encode()
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
