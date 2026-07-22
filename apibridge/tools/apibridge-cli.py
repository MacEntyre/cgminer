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
    stats                   GET /api/v1/stats (per-device detail, including
                            GekkoScience Fan/FanCeiling - see APIBRIDGE-README)
    stream [-n N]           Follow /api/v1/stream (Ctrl+C to stop; N frames then exit, default: unlimited)
    control                 POST /api/v1/control (or /api/v1/control/reset with --reset) -
                          requires cgminer to have been started with
                          --api-bridge-control (see APIBRIDGE-README's
                          Phase 2 section)

    All commands accept --json to print the raw API response instead of
    a condensed human-readable summary.

Control options (in addition to the connection options above):
    --asc-id N               ASC device index, same as in `devs` (default: 0)
    --option NAME             One of: freq, target, corev, setfan, lockfreq,
                              unlockfreq, zeromaxt (required unless --reset)
    --value N                 Numeric value, required for freq/target/corev/setfan
    --reset                   Hit /api/v1/control/reset instead (device reinit)
    --write-token TOKEN         Write-scoped bearer token (overrides --write-token-file/env)
    --write-token-file PATH      Read write token from this file (default: looks
                              for ./apibridge-write.token, then
                              <repo-root>/apibridge-write.token)

    The write token is separate from the read token above - it is also read
    from the APIBRIDGE_WRITE_TOKEN environment variable if neither
    --write-token nor --write-token-file finds one.

Examples:
    apibridge-cli.py summary
    apibridge-cli.py --host 192.168.1.50 devs --json
    apibridge-cli.py stream -n 5
    apibridge-cli.py control --asc-id 0 --option freq --value 650
    apibridge-cli.py control --asc-id 0 --option zeromaxt
    apibridge-cli.py control --asc-id 0 --reset
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
REPO_ROOT_WRITE_TOKEN = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", "apibridge-write.token"))


def _resolve_token(explicit, explicit_file, default_candidates, env_var):
    if explicit:
        return explicit, None

    candidates = [explicit_file] if explicit_file else default_candidates
    for path in candidates:
        try:
            with open(path) as f:
                return f.read().strip(), None
        except OSError:
            continue

    env_token = os.environ.get(env_var)
    if env_token:
        return env_token, None

    return None, candidates


def _require_token(explicit, explicit_file, default_candidates, env_var, cli_flags):
    token, searched = _resolve_token(explicit, explicit_file, default_candidates, env_var)
    if token:
        return token
    print("error: no token found (need one for this endpoint)", file=sys.stderr)
    if searched:
        print(f"  tried: {', '.join(searched)}", file=sys.stderr)
    print(f"  pass {cli_flags}, or set {env_var}", file=sys.stderr)
    sys.exit(1)


def resolve_token(args):
    return _resolve_token(args.token, args.token_file, ["apibridge.token", REPO_ROOT_TOKEN],
                           "APIBRIDGE_TOKEN")


def require_token(args):
    return _require_token(args.token, args.token_file, ["apibridge.token", REPO_ROOT_TOKEN],
                           "APIBRIDGE_TOKEN", "--token TOKEN, --token-file PATH")


def require_write_token(args):
    return _require_token(args.write_token, args.write_token_file,
                           ["apibridge-write.token", REPO_ROOT_WRITE_TOKEN],
                           "APIBRIDGE_WRITE_TOKEN", "--write-token TOKEN, --write-token-file PATH")


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


def http_post(url, body, token, timeout=10):
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        body_text = e.read().decode(errors="replace")
        try:
            return e.code, json.loads(body_text)
        except ValueError:
            return e.code, {"error": body_text}
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


def cmd_stats(args):
    url = f"http://{args.host}:{args.port}/api/v1/stats"
    status, data = http_get(url, require_token(args))
    require_ok(status, data, url)
    if args.json:
        print(json.dumps(data, indent=2))
        return
    entries = data.get("stats", {}).get("STATS", [])
    # the "stats" RPC command's STATS array also carries a POOL* entry per
    # pool alongside one entry per device - only the device entries are
    # interesting here (and only GSA1/GSA2 ones have Fan/FanCeiling at all).
    devs = [e for e in entries if not str(e.get("ID", "")).startswith("POOL")]
    print(f"stale: {data.get('stale')}  age: {data.get('age_s')}s  {len(devs)} device(s)")
    print(f"{'ID':>6} {'Serial':<14} {'Temp':>7} {'CoremV':>7} {'Fan':>9} {'FanCeiling':>10}")
    for d in devs:
        fan = d.get("Fan")
        fan_str = f"{fan:.0f}rpm" if fan is not None else "-"
        ceiling = d.get("FanCeiling")
        ceiling_str = str(ceiling) if ceiling is not None else "-"
        print(f"{d.get('ID', ''):>6} {d.get('Serial', ''):<14} "
              f"{d.get('Temp', 0):>6.1f}C {d.get('CoremV', 0):>7.0f} "
              f"{fan_str:>9} {ceiling_str:>10}")


def cmd_control(args):
    token = require_write_token(args)

    if args.reset:
        url = f"http://{args.host}:{args.port}/api/v1/control/reset"
        body = {"asc_id": args.asc_id}
    else:
        if not args.option:
            print("error: --option is required unless --reset is given", file=sys.stderr)
            sys.exit(1)
        url = f"http://{args.host}:{args.port}/api/v1/control"
        body = {"asc_id": args.asc_id, "option": args.option}
        if args.value is not None:
            body["value"] = args.value

    status, data = http_post(url, body, token)

    if args.json:
        print(json.dumps(data, indent=2))
        return

    if status == 200:
        print(f"OK: {data.get('message')}")
        return

    detail = data.get("message") or data.get("error") or data
    if status == 401:
        print(f"error: 401 unauthorized from {url} - check --write-token/--write-token-file", file=sys.stderr)
    elif status == 501:
        print("error: control endpoints not enabled on this apibridge "
              "(cgminer needs --api-bridge-control)", file=sys.stderr)
    elif status == 403:
        print(f"error: 403 from {url}: {detail} - cgminer likely needs "
              "--api-allow with a W: rule for apibridge's address", file=sys.stderr)
    else:
        print(f"error: HTTP {status} from {url}: {detail}", file=sys.stderr)
    sys.exit(1)


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
                      ("devs", cmd_devs), ("pools", cmd_pools), ("stats", cmd_stats)):
        p = sub.add_parser(name)
        p.add_argument("--json", action="store_true")
        p.set_defaults(func=fn)

    p = sub.add_parser("stream")
    p.add_argument("--json", action="store_true")
    p.add_argument("-n", "--count", type=int, default=0,
                    help="stop after N frames (default: run until Ctrl+C)")
    p.set_defaults(func=cmd_stream)

    p = sub.add_parser("control")
    p.add_argument("--asc-id", type=int, default=0)
    p.add_argument("--option", help="freq, target, corev, setfan, lockfreq, unlockfreq, or zeromaxt")
    p.add_argument("--value", type=float)
    p.add_argument("--reset", action="store_true", help="hit /api/v1/control/reset instead (device reinit)")
    p.add_argument("--write-token")
    p.add_argument("--write-token-file")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_control)

    return parser


def main():
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
