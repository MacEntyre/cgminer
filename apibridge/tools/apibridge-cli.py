#!/usr/bin/env python3
"""Interactive-ish CLI client for a running apibridge instance. Stdlib
only, no dependencies - for poking at a live apibridge by hand while
developing or debugging (see APIBRIDGE-README for the protocol). For a
scripted pass/fail check against real hardware, use hw-smoke-test.sh
instead.

Usage:
    apibridge-cli.py [connection options] <command> [command options]

Connection options:
    --host HOST         apibridge host (default: 127.0.0.1)
    --port PORT          apibridge port (default: 4029)
    --token TOKEN         Bearer token (overrides --token-file/env)
    --token-file PATH      Read token from this file (default: looks for
                          ./apibridge.token, then <repo-root>/apibridge.token)

    Token is also read from the APIBRIDGE_TOKEN environment variable if
    neither --token nor --token-file finds one. /health needs no token.

Commands:
    health                  GET /api/v1/health
    summary                 GET /api/v1/summary
    devs                    GET /api/v1/devs
    pools                   GET /api/v1/pools
    stream [-n N]           Follow /api/v1/stream (Ctrl+C to stop; N frames then exit, default: unlimited)

    All commands accept --json to print the raw API response instead of
    a condensed human-readable summary.

Examples:
    apibridge-cli.py summary
    apibridge-cli.py --host 192.168.1.50 devs --json
    apibridge-cli.py stream -n 5
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request

from _ws import connect, read_frame

# apibridge.token is written next to the cgminer binary, which is normally
# the repo root - not necessarily the caller's cwd, and not the directory
# this script lives in (apibridge/tools/). Try both so `summary` etc. work
# regardless of where you invoke this from.
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT_TOKEN = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", "apibridge.token"))


def resolve_token(args):
    if args.token:
        return args.token, None

    candidates = [args.token_file] if args.token_file else ["apibridge.token", REPO_ROOT_TOKEN]
    for path in candidates:
        try:
            with open(path) as f:
                return f.read().strip(), None
        except OSError:
            continue

    env_token = os.environ.get("APIBRIDGE_TOKEN")
    if env_token:
        return env_token, None

    return None, candidates


def require_token(args):
    token, searched = resolve_token(args)
    if token:
        return token
    print("error: no token found (need one for this endpoint)", file=sys.stderr)
    if searched:
        print(f"  tried: {', '.join(searched)}", file=sys.stderr)
    print("  pass --token TOKEN, --token-file PATH, or set APIBRIDGE_TOKEN", file=sys.stderr)
    sys.exit(1)


def http_get(url, token=None, timeout=10):
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
    except urllib.error.URLError as e:
        print(f"error: cannot reach apibridge at {url}: {e.reason}", file=sys.stderr)
        sys.exit(1)


def fmt_mhs(mhs):
    ghs = float(mhs) / 1000.0
    if ghs >= 1000:
        return f"{ghs / 1000:.2f} TH/s"
    return f"{ghs:.2f} GH/s"


def require_ok(status, data, url):
    if status == 401:
        print(f"error: 401 unauthorized from {url} - check --token/--token-file", file=sys.stderr)
        sys.exit(1)
    if status != 200:
        print(f"error: HTTP {status} from {url}: {data}", file=sys.stderr)
        sys.exit(1)


def cmd_health(args):
    url = f"http://{args.host}:{args.port}/api/v1/health"
    status, data = http_get(url)
    require_ok(status, data, url)
    if args.json:
        print(json.dumps(data, indent=2))
        return
    print(f"bridge:            {data.get('bridge')}")
    print(f"cgminer reachable: {data.get('cgminer_reachable')}")
    print(f"last update age:   {data.get('last_updated_age_s')}s")


def cmd_summary(args):
    url = f"http://{args.host}:{args.port}/api/v1/summary"
    status, data = http_get(url, require_token(args))
    require_ok(status, data, url)
    if args.json:
        print(json.dumps(data, indent=2))
        return
    s = data.get("summary", {}).get("SUMMARY", [{}])[0]
    print(f"stale: {data.get('stale')}  age: {data.get('age_s')}s  elapsed: {s.get('Elapsed')}s")
    print(f"hashrate  av={fmt_mhs(s.get('MHS av', 0))}  5s={fmt_mhs(s.get('MHS 5s', 0))}  "
          f"1m={fmt_mhs(s.get('MHS 1m', 0))}  5m={fmt_mhs(s.get('MHS 5m', 0))}")
    print(f"shares    accepted={s.get('Accepted')}  rejected={s.get('Rejected')}  "
          f"hw_errors={s.get('Hardware Errors')}")


def cmd_devs(args):
    url = f"http://{args.host}:{args.port}/api/v1/devs"
    status, data = http_get(url, require_token(args))
    require_ok(status, data, url)
    if args.json:
        print(json.dumps(data, indent=2))
        return
    devs = data.get("devs", {}).get("DEVS", [])
    print(f"stale: {data.get('stale')}  age: {data.get('age_s')}s  {len(devs)} device(s)")
    print(f"{'ASC':>3} {'Name':<6} {'Status':<8} {'Temp':>7} {'MHS 5s':>12} {'Accepted':>9} {'Rejected':>9} {'HW Err':>7}")
    for d in devs:
        print(f"{d.get('ASC'):>3} {d.get('Name', ''):<6} {d.get('Status', ''):<8} "
              f"{d.get('Temperature', 0):>6.1f}C {fmt_mhs(d.get('MHS 5s', 0)):>12} "
              f"{d.get('Accepted', 0):>9} {d.get('Rejected', 0):>9} {d.get('Hardware Errors', 0):>7}")


def cmd_pools(args):
    url = f"http://{args.host}:{args.port}/api/v1/pools"
    status, data = http_get(url, require_token(args))
    require_ok(status, data, url)
    if args.json:
        print(json.dumps(data, indent=2))
        return
    pools = data.get("pools", {}).get("POOLS", [])
    print(f"stale: {data.get('stale')}  age: {data.get('age_s')}s  {len(pools)} pool(s)")
    for p in pools:
        print(f"[{p.get('POOL')}] {p.get('URL')}  status={p.get('Status')}  "
              f"stratum_active={p.get('Stratum Active')}  diff={p.get('Work Difficulty')}  "
              f"accepted={p.get('Accepted')}  rejected={p.get('Rejected')}")


def cmd_stream(args):
    token = require_token(args)
    try:
        sock, buf = connect(args.host, args.port, "/api/v1/stream", token)
    except (OSError, RuntimeError) as e:
        print(f"error: cannot connect to ws://{args.host}:{args.port}/api/v1/stream: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"connected to ws://{args.host}:{args.port}/api/v1/stream (Ctrl+C to stop)")
    count = 0
    try:
        while args.count == 0 or count < args.count:
            opcode, payload, buf = read_frame(sock, buf)
            if opcode == 0x8:
                print("server closed the connection")
                break
            if opcode != 0x1:
                continue
            count += 1
            envelope = json.loads(payload.decode())
            if args.json:
                print(json.dumps(envelope, indent=2))
                continue
            devs = envelope.get("devs", {}).get("DEVS", [])
            dev_bits = ", ".join(
                f"{d.get('Name')}{d.get('ASC')}: {fmt_mhs(d.get('MHS 5s', 0))} {d.get('Temperature', 0):.1f}C"
                for d in devs
            )
            print(f"[{envelope.get('ts')}] stale={envelope.get('stale')}  {dev_bits}")
    except KeyboardInterrupt:
        print()
    finally:
        sock.close()


def build_parser():
    parser = argparse.ArgumentParser(
        description="CLI client for exercising a running apibridge instance.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4029)
    parser.add_argument("--token")
    parser.add_argument("--token-file")

    sub = parser.add_subparsers(dest="command", required=True)

    for name, fn in (("health", cmd_health), ("summary", cmd_summary),
                      ("devs", cmd_devs), ("pools", cmd_pools)):
        p = sub.add_parser(name)
        p.add_argument("--json", action="store_true")
        p.set_defaults(func=fn)

    p = sub.add_parser("stream")
    p.add_argument("--json", action="store_true")
    p.add_argument("-n", "--count", type=int, default=0,
                    help="stop after N frames (default: run until Ctrl+C)")
    p.set_defaults(func=cmd_stream)

    return parser


def main():
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
