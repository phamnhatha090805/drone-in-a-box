#!/usr/bin/env python3
"""Signing client for the dock controller's HTTP API.

Phase 1 bench testing (spec task T8) needs a client that can produce valid
signatures -- without one, only the negative cases are testable. Stdlib only,
no pip install.

Get the device key from the serial console with the 'K' command, then:

    export DOCK_KEY=<64 hex chars>
    ./dock_cli.py selftest          # runs W13-W17, moves nothing
    ./dock_cli.py cmd e --yes       # signed command, DOES move the actuator
    ./dock_cli.py deadman --yes     # starts a move, then goes silent

The canonical string must match components/auth/auth.c exactly: fields are
length-prefixed, and the query is signed in its URL-encoded form, because that
is what httpd_req_get_url_query_str() hands the firmware.
"""

import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


def canonical(method: str, path: str, query: str, nonce: int) -> bytes:
    """Mirror of auth_canonical(): "4:POST8:/api/cmd3:c=e1:7"."""
    out = b""
    for field in (method, path, query, str(nonce)):
        raw = field.encode()
        out += f"{len(raw)}:".encode() + raw
    return out


def sign(key: bytes, method: str, path: str, query: str, nonce: int) -> str:
    digest = hmac.new(key, canonical(method, path, query, nonce), hashlib.sha256).digest()
    return base64.b64encode(digest).decode()


class Dock:
    def __init__(self, host: str, key: bytes, timeout: float = 4.0):
        self.base, self.key, self.timeout = f"http://{host}", key, timeout
        self._nonce = None

    def _request(self, method, path, query="", headers=None):
        url = f"{self.base}{path}" + (f"?{query}" if query else "")
        req = urllib.request.Request(url, method=method)
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, json.loads(r.read().decode())
        except urllib.error.HTTPError as e:
            body = e.read().decode()
            try:
                return e.code, json.loads(body)
            except json.JSONDecodeError:
                return e.code, {"raw": body}

    def nonce(self) -> int:
        """The dock's replay high-water mark. Unauthenticated by design."""
        _, body = self._request("GET", "/api/nonce")
        return int(body["nonce"])

    def next_nonce(self) -> int:
        if self._nonce is None:
            self._nonce = self.nonce()
        self._nonce += 1
        return self._nonce

    def signed(self, method, path, query="", nonce=None, corrupt=False):
        n = self.next_nonce() if nonce is None else nonce
        sig = sign(self.key, method, path, query, n)
        if corrupt:
            raw = bytearray(base64.b64decode(sig))
            raw[0] ^= 0x01
            sig = base64.b64encode(bytes(raw)).decode()
        return self._request(method, path, query,
                             {"X-Dock-Nonce": str(n), "X-Dock-Sig": sig})

    def unsigned(self, method, path, query=""):
        return self._request(method, path, query)


def canonical_blob(method: str, path: str, payload: bytes, nonce: int) -> bytes:
    """Mirror of auth_canonical_blob(): the payload is bytes, not a C string."""
    out = b""
    for field in (method.encode(), path.encode(), payload, str(nonce).encode()):
        out += f"{len(field)}:".encode() + field
    return out


def ble_write(key: bytes, path: str, payload: bytes, nonce: int) -> bytes:
    """Frame a signed BLE write: nonce (8 LE) || signature (32) || payload."""
    sig = hmac.new(key, canonical_blob("BLE", path, payload, nonce),
                   hashlib.sha256).digest()
    return nonce.to_bytes(8, "little") + sig + payload


def check(name, expect, got_status, detail=""):
    ok = got_status == expect
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: got {got_status}, expected {expect} {detail}")
    return ok


def selftest(dock: Dock) -> int:
    """W13-W17 plus the signed happy path. Uses 's' so nothing moves."""
    print("Phase 1 authorisation self-test (nothing will move)\n")
    results = []

    s, body = dock._request("GET", "/api/nonce")
    results.append(check("W17 /api/nonce is open", 200, s, f"nonce={body.get('nonce')}"))

    s, _ = dock.unsigned("POST", "/api/cmd", "c=e")
    results.append(check("W13 unsigned command refused", 401, s))

    s, _ = dock.unsigned("GET", "/api/status")
    results.append(check("W16 unsigned status refused", 401, s))

    s, _ = dock.signed("POST", "/api/cmd", "c=s", corrupt=True)
    results.append(check("W14 bad signature refused", 401, s))

    n = dock.next_nonce()
    s1, _ = dock.signed("POST", "/api/cmd", "c=s", nonce=n)
    results.append(check("signed command accepted", 200, s1))
    s2, _ = dock.signed("POST", "/api/cmd", "c=s", nonce=n)
    results.append(check("W15 replay refused", 401, s2))

    passed = sum(results)
    print(f"\n{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


def deadman(dock: Dock, seconds: float) -> int:
    """Start a move, poll briefly, then go silent and let the dock stop it."""
    print("Starting an extend, polling for 2 s, then going silent.")
    print("Watch the actuator and the serial log: it should stop ~1.5 s after")
    print("the last poll, logging 'STOP: web client stopped responding'.\n")
    dock.signed("POST", "/api/cmd", "c=e")
    deadline = time.time() + 2.0
    while time.time() < deadline:
        s, body = dock.signed("GET", "/api/status")
        print(f"  poll -> {s} dir={body.get('dir')} dutyA={body.get('dutyA')}")
        time.sleep(0.4)
    print(f"\nSilent now. Expect a stop within ~{seconds} s.")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--key", default=os.environ.get("DOCK_KEY", ""),
                   help="64 hex chars; defaults to $DOCK_KEY")
    p.add_argument("--yes", action="store_true",
                   help="required for anything that can move the actuator")
    sub = p.add_subparsers(dest="action", required=True)
    sub.add_parser("nonce")
    sub.add_parser("status")
    sub.add_parser("selftest")
    sub.add_parser("deadman")
    c = sub.add_parser("cmd")
    c.add_argument("char")

    b = sub.add_parser("blesign", help="hex bytes for a signed BLE write "
                                       "(paste into nRF Connect)")
    b.add_argument("what", choices=["cmd", "provision"])
    b.add_argument("value", help="one command char, or SSID")
    b.add_argument("password", nargs="?", default="")

    args = p.parse_args()

    if args.action == "nonce":
        print(Dock(args.host, b"").nonce())
        return 0

    if len(args.key) != 64:
        print("Need a 64-hex-character device key: press 'K' on the serial "
              "console, then export DOCK_KEY=<key>", file=sys.stderr)
        return 2
    dock = Dock(args.host, bytes.fromhex(args.key))

    if args.action == "status":
        print(json.dumps(dock.signed("GET", "/api/status")[1], indent=2))
        return 0

    if args.action == "selftest":
        return selftest(dock)

    if args.action == "blesign":
        # The nonce comes from the dock over Wi-Fi: APSTA keeps the AP up even
        # when BLE is the transport under test, so this is always reachable.
        nonce = dock.next_nonce()
        if args.what == "cmd":
            path, payload = "/ble/cmd", args.value.encode()
        else:
            path = "/ble/provision"
            payload = args.value.encode() + b"\x00" + args.password.encode()
        frame = ble_write(dock.key, path, payload, nonce)
        print(f"nonce {nonce}, {len(frame)} bytes for characteristic "
              f"{'...5e04' if args.what == 'cmd' else '...5e03'}:\n")
        print(frame.hex())
        return 0

    moving = args.action == "deadman" or (args.action == "cmd" and args.char not in "sp")
    if moving and not args.yes:
        print("This can move the actuator. Re-run with --yes.", file=sys.stderr)
        return 2

    if args.action == "deadman":
        return deadman(dock, 1.5)

    query = "c=" + urllib.parse.quote(args.char, safe="")
    status, body = dock.signed("POST", "/api/cmd", query)
    print(status, json.dumps(body, indent=2))
    return 0 if status == 200 else 1


if __name__ == "__main__":
    sys.exit(main())
