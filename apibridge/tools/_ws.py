"""Minimal stdlib-only WebSocket client primitives (RFC 6455 handshake +
frame decoding), shared by ws_probe.py and apibridge-cli.py so neither
needs an external `websockets` package. Not a general-purpose client -
just enough to read text frames off apibridge's read-only /api/v1/stream.
"""
import base64
import hashlib
import os
import socket
import struct

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def connect(host, port, path, token=None, timeout=10):
    key = base64.b64encode(os.urandom(16)).decode()
    target = f"{path}?token={token}" if token else path
    req = (
        f"GET {target} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.sendall(req.encode())

    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed during handshake")
        resp += chunk
    header, _, rest = resp.partition(b"\r\n\r\n")
    status_line = header.split(b"\r\n")[0]
    if b"101" not in status_line:
        raise RuntimeError(f"handshake failed ({status_line.decode(errors='replace')})")

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
