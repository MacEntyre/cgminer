#!/usr/bin/env python3
"""Minimal stdlib-only WebSocket client to sanity-check apibridge's
/api/v1/stream endpoint against a real cgminer instance. No external
dependencies (no `websockets` package) so it runs anywhere Python 3 does.

Usage:
    python3 ws_probe.py <host> <port> <token> [num_frames]
"""
import base64
import hashlib
import os
import socket
import struct
import sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_connect(host, port, path, token):
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path}?token={token} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock = socket.create_connection((host, port), timeout=10)
    sock.sendall(req.encode())

    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += sock.recv(4096)
    header, _, rest = resp.partition(b"\r\n\r\n")
    if b"101" not in header.split(b"\r\n")[0]:
        raise RuntimeError(f"handshake failed:\n{header.decode(errors='replace')}")

    expect_accept = base64.b64encode(
        hashlib.sha1((key + GUID).encode()).digest()
    ).decode()
    if expect_accept.encode() not in header:
        raise RuntimeError("Sec-WebSocket-Accept mismatch")

    return sock, rest


def read_frame(sock, buf):
    while len(buf) < 2:
        buf += sock.recv(4096)
    b0, b1 = buf[0], buf[1]
    opcode = b0 & 0x0F
    masked = b1 & 0x80
    plen = b1 & 0x7F
    idx = 2
    if plen == 126:
        while len(buf) < idx + 2:
            buf += sock.recv(4096)
        plen = struct.unpack(">H", buf[idx:idx + 2])[0]
        idx += 2
    elif plen == 127:
        while len(buf) < idx + 8:
            buf += sock.recv(4096)
        plen = struct.unpack(">Q", buf[idx:idx + 8])[0]
        idx += 8
    mask_key = b""
    if masked:
        while len(buf) < idx + 4:
            buf += sock.recv(4096)
        mask_key = buf[idx:idx + 4]
        idx += 4
    while len(buf) < idx + plen:
        buf += sock.recv(4096)
    payload = buf[idx:idx + plen]
    if masked:
        payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
    buf = buf[idx + plen:]
    return opcode, payload, buf


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    host, port, token = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    num_frames = int(sys.argv[4]) if len(sys.argv) > 4 else 3

    sock, buf = ws_connect(host, port, "/api/v1/stream", token)
    print("handshake OK, waiting for frames...")
    try:
        for i in range(num_frames):
            opcode, payload, buf = read_frame(sock, buf)
            if opcode == 0x8:
                print("server closed connection")
                break
            print(f"--- frame {i} (opcode {opcode:#x}, {len(payload)} bytes) ---")
            print(payload.decode(errors="replace"))
    finally:
        sock.close()


if __name__ == "__main__":
    main()
