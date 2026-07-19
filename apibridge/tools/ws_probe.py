#!/usr/bin/env python3
"""Minimal stdlib-only WebSocket client to sanity-check apibridge's
/api/v1/stream endpoint against a real cgminer instance. No external
dependencies (no `websockets` package) so it runs anywhere Python 3 does.

Usage:
    python3 ws_probe.py <host> <port> <token> [num_frames]
"""
import sys

from _ws import connect, read_frame


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    host, port, token = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    num_frames = int(sys.argv[4]) if len(sys.argv) > 4 else 3

    sock, buf = connect(host, port, "/api/v1/stream", token)
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
