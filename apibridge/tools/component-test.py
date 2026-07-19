#!/usr/bin/env python3
"""Component test for apibridged - exercises the full HTTP stack (auth,
statscache, httpapi) against a fake cgminer RPC server instead of real
hardware, so unlike apibridge/tools/hw-smoke-test.sh this one is safe to run
in CI (see .github/workflows/build.yml). Stdlib only, no dependencies.

The fake server speaks just enough of cgminer's api.c protocol (one
JSON request per connection, NUL-terminated JSON reply, then close) to
serve canned "summary"/"devs"/"pools" responses.

Usage:
    apibridge/tools/component-test.py [--apibridged PATH]
"""
import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", ".."))

CANNED_REPLIES = {
    "summary": {
        "STATUS": [{"STATUS": "S", "Msg": "Summary"}],
        "SUMMARY": [{
            "Elapsed": 123, "MHS av": 1500.0, "MHS 5s": 1500.0,
            "MHS 1m": 1500.0, "MHS 5m": 1500.0,
            "Accepted": 10, "Rejected": 1, "Hardware Errors": 0,
        }],
        "id": 1,
    },
    "devs": {
        "STATUS": [{"STATUS": "S", "Msg": "Devices"}],
        "DEVS": [{
            "ASC": 0, "Name": "GSF", "Status": "Alive",
            "Temperature": 55.5, "MHS 5s": 1500.0,
            "Accepted": 10, "Rejected": 1, "Hardware Errors": 0,
        }],
        "id": 1,
    },
    "pools": {
        "STATUS": [{"STATUS": "S", "Msg": "Pools"}],
        "POOLS": [{
            "POOL": 0, "URL": "stratum+tcp://component-test.invalid:3333",
            "Status": "Alive", "Stratum Active": True,
            "Work Difficulty": 1024, "Accepted": 10, "Rejected": 1,
        }],
        "id": 1,
    },
}

fail = 0


def check(desc, got, want):
    global fail
    if got == want:
        print(f"OK   {desc} ({got})")
    else:
        print(f"FAIL {desc} (got {got!r}, want {want!r})")
        fail = 1


class FakeCgminer:
    """Minimal stand-in for cgminer's RPC API: one JSON command per
    connection, NUL-terminated JSON reply (see apibridge/cgclient.c)."""

    def __init__(self, host, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(5)
        self.sock.settimeout(0.5)
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def _serve(self):
        while not self.stop_event.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            try:
                data = conn.recv(4096)
                req = json.loads(data.decode())
                reply = CANNED_REPLIES.get(
                    req.get("command"),
                    {"STATUS": [{"STATUS": "E", "Msg": "invalid command"}], "id": 1})
                conn.sendall(json.dumps(reply).encode() + b"\x00")
            except (OSError, ValueError):
                pass
            finally:
                conn.close()

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.thread.join(timeout=2)
        self.sock.close()


def http_get(url, token=None, timeout=5):
    req = urllib.request.Request(url)
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        try:
            return e.code, json.loads(body)
        except ValueError:
            return e.code, {"error": body}


def wait_for_health(host, port, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            status, _ = http_get(f"http://{host}:{port}/api/v1/health")
            if status == 200:
                return True
        except (urllib.error.URLError, ConnectionError):
            pass
        time.sleep(0.2)
    return False


def main():
    global fail
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apibridged", default=os.path.join(REPO_ROOT, "apibridged"))
    parser.add_argument("--cgminer-port", type=int, default=14028)
    parser.add_argument("--listen-port", type=int, default=14029)
    args = parser.parse_args()

    if not os.path.isfile(args.apibridged) or not os.access(args.apibridged, os.X_OK):
        print(f"error: {args.apibridged} not found or not executable - "
              f"build with --enable-apibridge first", file=sys.stderr)
        return 1

    host = "127.0.0.1"
    token = "component-test-token"

    fake_cgminer = FakeCgminer(host, args.cgminer_port)
    fake_cgminer.start()

    env = dict(os.environ)
    env["CGMINER_APIBRIDGE_TOKEN"] = token
    proc = subprocess.Popen(
        [args.apibridged,
         "--cgminer-host", host, "--cgminer-port", str(args.cgminer_port),
         "--listen-port", str(args.listen_port),
         "--poll-interval-ms", "200"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    try:
        if not wait_for_health(host, args.listen_port, timeout_s=10):
            print("FAIL apibridged never became healthy", file=sys.stderr)
            fail_output = proc.stdout.read() if proc.stdout else ""
            print(fail_output, file=sys.stderr)
            return 1

        # Give it one more poll cycle so the cache is populated from our
        # fake cgminer, not just serving the initial "stale" snapshot.
        time.sleep(0.5)

        base = f"http://{host}:{args.listen_port}/api/v1"

        status, data = http_get(f"{base}/health")
        check("GET /health status", status, 200)
        check("GET /health cgminer_reachable", data.get("cgminer_reachable"), True)

        status, _ = http_get(f"{base}/summary")
        check("GET /summary without token", status, 401)

        status, _ = http_get(f"{base}/summary", token="wrong-token")
        check("GET /summary with wrong token", status, 401)

        status, data = http_get(f"{base}/summary", token=token)
        check("GET /summary with token", status, 200)
        check("GET /summary Accepted field", data.get("summary", {}).get("SUMMARY", [{}])[0].get("Accepted"), 10)

        status, data = http_get(f"{base}/devs", token=token)
        check("GET /devs with token", status, 200)
        check("GET /devs Name field", data.get("devs", {}).get("DEVS", [{}])[0].get("Name"), "GSF")

        status, data = http_get(f"{base}/pools", token=token)
        check("GET /pools with token", status, 200)
        check("GET /pools URL field", data.get("pools", {}).get("POOLS", [{}])[0].get("URL"),
              "stratum+tcp://component-test.invalid:3333")

        # Mobile WebSocket clients can't always set custom headers on the
        # upgrade handshake, so auth also accepts a ?token= query param
        # (see apibridge/auth.c) - cover that fallback here too.
        status, _ = http_get(f"{base}/summary?token={token}")
        check("GET /summary with query-string token", status, 200)

        proc.send_signal(signal.SIGTERM)
        try:
            exit_code = proc.wait(timeout=5)
            check("apibridged exited cleanly on SIGTERM", exit_code, 0)
        except subprocess.TimeoutExpired:
            print("FAIL apibridged did not exit within 5s of SIGTERM")
            proc.kill()
            proc.wait()
            fail = 1

    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        fake_cgminer.stop()

    if fail:
        print("\nCOMPONENT TEST: FAILED")
        return 1
    print("\nCOMPONENT TEST: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
