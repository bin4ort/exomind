#!/usr/bin/env python3
"""Minimal RFC 6455 client for exosched tests (stdlib only).

Usage: wsclient.py <port> [expect-substring] [timeout] [close-test]
Reads text frames, prints each. Exits 0 when a frame containing the
expected substring arrives, or after a successful close round-trip
(when expect-substring == CLOSE), else 1 on timeout/EOF.
"""
import base64
import os
import socket
import sys
import time

port = int(sys.argv[1])
expect = sys.argv[2] if len(sys.argv) > 2 else None
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
close_test = expect == "CLOSE"

key = base64.b64encode(os.urandom(16)).decode()
s = socket.create_connection(("127.0.0.1", port), timeout=5)
req = (
    "GET /ws HTTP/1.1\r\n"
    "Host: 127.0.0.1:%d\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n" % (port, key)
)
s.sendall(req.encode())
buf = b""
while b"\r\n\r\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        sys.exit(1)
    buf += chunk
if b"101" not in buf.split(b"\r\n")[0]:
    sys.exit(1)
s.settimeout(timeout)
end = time.time() + timeout


def recv_frame(sock):
    hdr = b""
    while len(hdr) < 2:
        chunk = sock.recv(2 - len(hdr))
        if not chunk:
            return None
        hdr += chunk
    b0, b1 = hdr[0], hdr[1]
    opcode = b0 & 0x0F
    n = b1 & 0x7F
    masked = (b1 & 0x80) != 0
    if n == 126:
        ext = b""
        while len(ext) < 2:
            ext += sock.recv(2 - len(ext))
        n = int.from_bytes(ext, "big")
    elif n == 127:
        ext = b""
        while len(ext) < 8:
            ext += sock.recv(8 - len(ext))
        n = int.from_bytes(ext, "big")
    mask = b""
    if masked:
        while len(mask) < 4:
            mask += sock.recv(4 - len(mask))
    payload = b""
    while len(payload) < n:
        chunk = sock.recv(n - len(payload))
        if not chunk:
            return None
        payload += chunk
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


ok = False
if close_test:
    s.sendall(bytes([0x88, 0x02, 0x03, 0xE8]))  # close(1000)
    try:
        op, payload = recv_frame(s)
        if op == 0x8:
            ok = True
    except socket.timeout:
        pass
    s.close()
    sys.exit(0 if ok else 1)
while time.time() < end:
    try:
        frame = recv_frame(s)
    except socket.timeout:
        break
    if frame is None:
        break
    opcode, payload = frame
    if opcode == 0x8:  # close
        s.sendall(bytes([0x88, 0]))
        ok = True
        break
    if opcode == 0x9:  # ping -> pong
        s.sendall(bytes([0x8A, len(payload)]) + payload)
        continue
    if opcode == 0x1:  # text
        line = payload.decode("utf-8", "replace")
        print(line, flush=True)
        if expect and expect in line:
            ok = True
            break
s.close()
if close_test:
    sys.exit(0 if ok else 1)
sys.exit(0 if ok else 1)
