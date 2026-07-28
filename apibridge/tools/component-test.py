#!/usr/bin/env python3
"""Component test for apibridged - exercises the full HTTP stack (auth,
statscache, httpapi) against a fake cgminer RPC server instead of real
hardware, so unlike apibridge/tools/hw-smoke-test.sh this one is safe to run
in CI (see .github/workflows/build.yml). Stdlib only, no dependencies.

The fake server speaks just enough of cgminer's api.c protocol (one
JSON request per connection, NUL-terminated JSON reply, then close) to
serve canned "summary"/"devs"/"pools"/"stats" responses.

Also checks that docs/openapi.yaml's declared paths match the routes
actually registered in apibridge/httpapi.c and wsapi.c, so the spec can't
silently drift from the code (see check_spec_matches_registered_routes()).

Usage:
    apibridge/tools/component-test.py [--apibridged PATH]
"""
import argparse
import json
import os
import re
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
    "stats": {
        # cgminer's real "stats" STATS array mixes one entry per device
        # with one entry per pool (see api.c's minerstats()) - mirror that
        # shape here so apibridge-cli.py's device-vs-pool filtering has
        # something real to filter.
        "STATUS": [{"STATUS": "S", "Msg": "CGMiner stats"}],
        "STATS": [
            {
                "STATS": 0, "ID": "GSA0", "Serial": "GS-10100119",
                "Temp": 40.7, "CoremV": 270, "Fan": 6990.0, "FanCeiling": False,
            },
            {
                "STATS": 1, "ID": "POOL0", "Pool Calls": 0, "Pool Attempts": 0,
            },
        ],
        "id": 1,
    },
    "version": {
        "STATUS": [{"STATUS": "S", "Msg": "CGMiner versions"}],
        "VERSION": [{"CGMiner": "4.13.6", "API": "3.7"}],
        "id": 1,
    },
    "config": {
        "STATUS": [{"STATUS": "S", "Msg": "CGMiner config"}],
        "CONFIG": [{
            "ASC Count": 1, "PGA Count": 0, "Pool Count": 1,
            "Strategy": "Failover", "Log Interval": 5,
            "Device Code": "GSK GSA GSF GSH GSI ", "OS": "Linux", "Hotplug": "5",
        }],
        "id": 1,
    },
    "coin": {
        "STATUS": [{"STATUS": "S", "Msg": "CGMiner coin"}],
        "COIN": [{
            "Hash Method": "SHA256", "Current Block Time": 1234567890.123456,
            "Current Block Hash": "0" * 64, "LP": True, "Network Difficulty": 90666502495566.98,
        }],
        "id": 1,
    },
    "notify": {
        "STATUS": [{"STATUS": "S", "Msg": "Notify"}],
        "NOTIFY": [{
            "NOTIFY": 0, "Name": "GSF", "ID": 0,
            "Last Well": 1234567890, "Last Not Well": 0, "Reason Not Well": "None",
            "*Thread Fail Init": 0, "*Thread Zero Hash": 0, "*Thread Fail Queue": 0,
            "*Dev Sick Idle 60s": 0, "*Dev Dead Idle 600s": 0, "*Dev Nostart": 0,
            "*Dev Over Heat": 0, "*Dev Thermal Cutoff": 0, "*Dev Comms Error": 0,
            "*Dev Throttle": 0,
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


def _access_denied(command):
    return {"STATUS": [{"STATUS": "E", "Msg": f"Access denied to '{command}' command"}], "id": 1}


class FakeCgminer:
    """Minimal stand-in for cgminer's RPC API: one JSON command per
    connection, NUL-terminated JSON reply (see apibridge/cgclient.c).

    privileged_allowed mimics cgminer's own --api-allow ACL (api.c): when
    False, both "ascset" and "privileged" (both iswritemode=true in
    cgminer's cmds[] table) come back as an Access denied STATUS, exactly
    like a real cgminer started without a W: rule for apibridge's address -
    see APIBRIDGE-README's Phase 2 --api-allow requirement."""

    KNOWN_ASCSET_OPTIONS = {
        "freq", "target", "corev", "setfan", "lockfreq", "unlockfreq", "zeromaxt", "reset",
        "chip", "waitfactor", "usbprop", "require",
    }

    def __init__(self, host, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(5)
        self.sock.settimeout(0.5)
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.privileged_allowed = True

    def _handle_ascset(self, parameter):
        if not self.privileged_allowed:
            return _access_denied("ascset")
        parts = (parameter or "").split(",")
        asc_id = parts[0] if parts else "0"
        option = parts[1] if len(parts) > 1 else ""
        if option in self.KNOWN_ASCSET_OPTIONS:
            return {"STATUS": [{"STATUS": "S", "Msg": f"ASC {asc_id} set OK"}], "id": 1}
        return {"STATUS": [{"STATUS": "E", "Msg": f"Unknown option: {option}"}], "id": 1}

    def _handle_privileged(self):
        if not self.privileged_allowed:
            return _access_denied("privileged")
        return {"STATUS": [{"STATUS": "S", "Msg": "Privileged access OK"}], "id": 1}

    def _handle_asc_enable_disable(self, command, parameter):
        """Mirrors api.c's ascenable()/ascdisable(): success (and the
        already-enabled/already-disabled case) is reported as STATUS "I",
        not "S" - see control.h:control_enable_status_to_http()."""
        if not self.privileged_allowed:
            return _access_denied(command)
        asc_id = parameter or "0"
        verb = "sent enable message" if command == "ascenable" else "set disable flag"
        return {"STATUS": [{"STATUS": "I", "Msg": f"ASC {asc_id} {verb}"}], "id": 1}

    def _serve(self):
        while not self.stop_event.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            try:
                data = conn.recv(4096)
                req = json.loads(data.decode())
                command = req.get("command")
                if command == "ascset":
                    reply = self._handle_ascset(req.get("parameter"))
                elif command == "privileged":
                    reply = self._handle_privileged()
                elif command in ("ascenable", "ascdisable"):
                    reply = self._handle_asc_enable_disable(command, req.get("parameter"))
                else:
                    reply = CANNED_REPLIES.get(
                        command,
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


def spec_paths(openapi_yaml_path):
    """Top-level `paths:` keys from docs/openapi.yaml, e.g. "/api/v1/health".
    Deliberately a regex over the hand-authored YAML rather than a real
    YAML parse - this script is stdlib-only by design (see module
    docstring), and the file's `paths:` entries are always simple strings
    at a fixed 2-space indent, one level below every other top-level
    section (components' nested keys sit at 4+ spaces), so the regex is
    unambiguous for this specific file."""
    with open(openapi_yaml_path, encoding="utf-8") as f:
        text = f.read()
    # Each line of the paths: block is either blank or indented >=2 spaces;
    # stop at the first line that is neither (the next top-level section).
    m = re.search(r"(?m)^paths:\n((?:^(?:  .*)?\n)*)", text)
    if not m:
        return set()
    return set(re.findall(r"(?m)^  (/\S+):", m.group(1)))


def registered_paths():
    """Routes actually wired up via mg_set_request_handler/
    mg_set_websocket_handler in apibridge/httpapi.c and wsapi.c - read
    straight from source (not a second hand-maintained list) so this is a
    real drift check between the spec and the code, not two lists that
    can drift together."""
    paths = set()
    for relpath in ("apibridge/httpapi.c", "apibridge/wsapi.c"):
        with open(os.path.join(REPO_ROOT, relpath), encoding="utf-8") as f:
            text = f.read()
        paths.update(re.findall(r'mg_set_(?:request|websocket)_handler\(\s*ctx,\s*"([^"]+)"', text))
    return paths


def check_spec_matches_registered_routes():
    spec = spec_paths(os.path.join(REPO_ROOT, "docs", "openapi.yaml"))
    registered = registered_paths()
    check("openapi.yaml paths match registered civetweb routes",
          sorted(spec), sorted(registered))


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


def http_post(url, body, token=None, timeout=5):
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        resp_body = e.read().decode(errors="replace")
        try:
            return e.code, json.loads(resp_body)
        except ValueError:
            return e.code, {"error": resp_body}


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


def start_apibridged(apibridged_path, host, cgminer_port, listen_port, token, write_token=None):
    env = dict(os.environ)
    env["CGMINER_APIBRIDGE_TOKEN"] = token
    if write_token:
        env["CGMINER_APIBRIDGE_WRITE_TOKEN"] = write_token
    else:
        env.pop("CGMINER_APIBRIDGE_WRITE_TOKEN", None)
    return subprocess.Popen(
        [apibridged_path,
         "--cgminer-host", host, "--cgminer-port", str(cgminer_port),
         "--listen-port", str(listen_port),
         "--poll-interval-ms", "200"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def stop_apibridged(proc, label):
    global fail
    proc.send_signal(signal.SIGTERM)
    try:
        exit_code = proc.wait(timeout=5)
        check(f"{label} exited cleanly on SIGTERM", exit_code, 0)
    except subprocess.TimeoutExpired:
        print(f"FAIL {label} did not exit within 5s of SIGTERM")
        proc.kill()
        proc.wait()
        fail = 1


def run_readonly_and_control_disabled(apibridged_path, host, cgminer_port, listen_port, token):
    """Phase 1 read-only routes, plus confirming that without a write token
    the Phase 2 control routes stay off (501) rather than silently 401ing -
    see auth_check_write_request()'s unset-token short-circuit."""
    proc = start_apibridged(apibridged_path, host, cgminer_port, listen_port, token)
    try:
        if not wait_for_health(host, listen_port, timeout_s=10):
            print("FAIL apibridged never became healthy", file=sys.stderr)
            print(proc.stdout.read() if proc.stdout else "", file=sys.stderr)
            return False

        # Give it one more poll cycle so the cache is populated from our
        # fake cgminer, not just serving the initial "stale" snapshot.
        time.sleep(0.5)

        base = f"http://{host}:{listen_port}/api/v1"

        status, data = http_get(f"{base}/health")
        check("GET /health status", status, 200)
        check("GET /health cgminer_reachable", data.get("cgminer_reachable"), True)
        check("GET /health control_enabled (no write token)", data.get("control_enabled"), False)
        check("GET /health control_available (no write token)", data.get("control_available"), False)

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

        status, _ = http_get(f"{base}/stats")
        check("GET /stats without token", status, 401)

        status, data = http_get(f"{base}/stats", token=token)
        check("GET /stats with token", status, 200)
        stats_entries = data.get("stats", {}).get("STATS", [])
        gsa0 = next((e for e in stats_entries if e.get("ID") == "GSA0"), {})
        check("GET /stats Fan field", gsa0.get("Fan"), 6990.0)
        check("GET /stats FanCeiling field", gsa0.get("FanCeiling"), False)

        status, _ = http_get(f"{base}/version")
        check("GET /version without token", status, 401)

        status, data = http_get(f"{base}/version", token=token)
        check("GET /version with token", status, 200)
        check("GET /version CGMiner field", data.get("version", {}).get("VERSION", [{}])[0].get("CGMiner"),
              "4.13.6")

        status, data = http_get(f"{base}/config", token=token)
        check("GET /config with token", status, 200)
        check("GET /config ASC Count field", data.get("config", {}).get("CONFIG", [{}])[0].get("ASC Count"), 1)

        status, data = http_get(f"{base}/coin", token=token)
        check("GET /coin with token", status, 200)
        check("GET /coin Network Difficulty field",
              data.get("coin", {}).get("COIN", [{}])[0].get("Network Difficulty"), 90666502495566.98)

        status, data = http_get(f"{base}/notify", token=token)
        check("GET /notify with token", status, 200)
        check("GET /notify Name field", data.get("notify", {}).get("NOTIFY", [{}])[0].get("Name"), "GSF")

        status, _ = http_post(f"{base}/control/enable", {"asc_id": 0})
        check("POST /control/enable without write token configured", status, 501)

        status, _ = http_post(f"{base}/control/disable", {"asc_id": 0})
        check("POST /control/disable without write token configured", status, 501)

        # Mobile WebSocket clients can't always set custom headers on the
        # upgrade handshake, so auth also accepts a ?token= query param
        # (see apibridge/auth.c) - cover that fallback here too.
        status, _ = http_get(f"{base}/summary?token={token}")
        check("GET /summary with query-string token", status, 200)

        status, _ = http_post(f"{base}/control", {"asc_id": 0, "option": "freq", "value": 650}, token=token)
        check("POST /control without write token configured", status, 501)

        return True
    finally:
        stop_apibridged(proc, "apibridged (read-only run)")


def run_control_enabled(apibridged_path, host, cgminer_port, listen_port, token, write_token):
    """Control routes with a write token configured and cgminer's ACL
    granting write access (fake_cgminer.privileged_allowed=True)."""
    proc = start_apibridged(apibridged_path, host, cgminer_port, listen_port, token, write_token)
    try:
        if not wait_for_health(host, listen_port, timeout_s=10):
            print("FAIL apibridged (control run) never became healthy", file=sys.stderr)
            print(proc.stdout.read() if proc.stdout else "", file=sys.stderr)
            return False

        base = f"http://{host}:{listen_port}/api/v1"

        status, data = http_get(f"{base}/health")
        check("GET /health control_enabled (write token set)", data.get("control_enabled"), True)
        check("GET /health control_available (privileged allowed)", data.get("control_available"), True)

        status, _ = http_post(f"{base}/control", {"asc_id": 0, "option": "freq", "value": 650})
        check("POST /control without any token", status, 401)

        status, _ = http_post(f"{base}/control", {"asc_id": 0, "option": "freq", "value": 650}, token=token)
        check("POST /control with read-only token rejected", status, 401)

        status, _ = http_post(f"{base}/control", {"asc_id": 0, "option": "freq", "value": 650},
                               token="wrong-write-token")
        check("POST /control with wrong write token", status, 401)

        status, data = http_post(f"{base}/control", {"asc_id": 0, "option": "freq", "value": 650},
                                  token=write_token)
        check("POST /control freq with write token", status, 200)
        check("POST /control freq relays cgminer's Msg", data.get("message"), "ASC 0 set OK")

        status, _ = http_post(f"{base}/control", {"asc_id": 0, "option": "lockfreq"}, token=write_token)
        check("POST /control lockfreq (no value needed)", status, 200)

        status, _ = http_post(f"{base}/control", {"asc_id": 0, "option": "not-a-real-option"},
                               token=write_token)
        check("POST /control with non-whitelisted option rejected", status, 400)

        status, _ = http_post(f"{base}/control", {"asc_id": 0, "option": "reset"}, token=write_token)
        check("POST /control cannot reach reset via generic option", status, 400)

        status, data = http_post(f"{base}/control/reset", {"asc_id": 0}, token=write_token)
        check("POST /control/reset with write token", status, 200)

        status, data = http_post(f"{base}/control",
                                  {"asc_id": 0, "option": "chip", "chip_index": 3, "value": 650},
                                  token=write_token)
        check("POST /control chip with chip_index and value", status, 200)

        status, _ = http_post(f"{base}/control", {"asc_id": 0, "option": "chip", "value": 650},
                               token=write_token)
        check("POST /control chip without chip_index rejected", status, 400)

        # ascenable/ascdisable report success as STATUS "I", not "S" - this
        # exercises apibridge's control_enable_status_to_http() end to end
        # against the fake cgminer's matching "I" reply (see
        # FakeCgminer._handle_asc_enable_disable()).
        status, data = http_post(f"{base}/control/enable", {"asc_id": 0}, token=write_token)
        check("POST /control/enable with write token (STATUS I -> 200)", status, 200)

        status, data = http_post(f"{base}/control/disable", {"asc_id": 0}, token=write_token)
        check("POST /control/disable with write token (STATUS I -> 200)", status, 200)

        return True
    finally:
        stop_apibridged(proc, "apibridged (control-enabled run)")


def run_control_available_false(apibridged_path, host, cgminer_port, listen_port, token, write_token):
    """cgminer's own ACL denies write access (missing --api-allow W: rule) -
    apibridge should surface this via control_available:false on /health and
    a 403 passthrough of cgminer's Access denied on every control POST,
    rather than crashing or hanging."""
    proc = start_apibridged(apibridged_path, host, cgminer_port, listen_port, token, write_token)
    try:
        if not wait_for_health(host, listen_port, timeout_s=10):
            print("FAIL apibridged (denied run) never became healthy", file=sys.stderr)
            print(proc.stdout.read() if proc.stdout else "", file=sys.stderr)
            return False

        base = f"http://{host}:{listen_port}/api/v1"

        status, data = http_get(f"{base}/health")
        check("GET /health control_available (cgminer ACL denies)", data.get("control_available"), False)

        status, data = http_post(f"{base}/control", {"asc_id": 0, "option": "freq", "value": 650},
                                  token=write_token)
        check("POST /control passes through cgminer's Access denied", status, 403)

        status, _ = http_post(f"{base}/control/enable", {"asc_id": 0}, token=write_token)
        check("POST /control/enable passes through cgminer's Access denied", status, 403)

        return True
    finally:
        stop_apibridged(proc, "apibridged (control-denied run)")


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

    check_spec_matches_registered_routes()

    host = "127.0.0.1"
    token = "component-test-token"
    write_token = "component-test-write-token"

    fake_cgminer = FakeCgminer(host, args.cgminer_port)
    fake_cgminer.start()

    try:
        if not run_readonly_and_control_disabled(args.apibridged, host, args.cgminer_port,
                                                  args.listen_port, token):
            return 1

        fake_cgminer.privileged_allowed = True
        if not run_control_enabled(args.apibridged, host, args.cgminer_port, args.listen_port,
                                    token, write_token):
            return 1

        fake_cgminer.privileged_allowed = False
        if not run_control_available_false(args.apibridged, host, args.cgminer_port, args.listen_port,
                                            token, write_token):
            return 1
    finally:
        fake_cgminer.stop()

    if fail:
        print("\nCOMPONENT TEST: FAILED")
        return 1
    print("\nCOMPONENT TEST: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
