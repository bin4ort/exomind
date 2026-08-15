#!/usr/bin/env python3
"""Tiny canned-spec HTTP daemon for exodoc tests (one connection at a time)."""
import signal
import socket
import sys

port = int(sys.argv[1])
spec = open(sys.argv[2], 'rb').read()
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(8)
signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
signal.signal(signal.SIGINT, lambda *a: sys.exit(0))
while True:
    try:
        c, _ = s.accept()
    except OSError:
        break
    try:
        c.recv(65536)
        resp = ("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                "Content-Type: text/plain\r\nConnection: close\r\n\r\n"
                % len(spec)).encode() + spec
        c.sendall(resp)
    except OSError:
        pass
    try:
        c.close()
    except OSError:
        pass
