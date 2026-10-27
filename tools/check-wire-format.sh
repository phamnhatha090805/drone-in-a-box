#!/usr/bin/env bash
# Verify that all three implementations of the dock wire format agree.
#
#   firmware/drone_box/components/auth/auth.c     (C, on the dock)
#   firmware/drone_box/tools/dock_cli.py          (Python, bench client)
#   <app>/wire/.../DockCrypto.kt                   (Kotlin, the app)
#   firmware/.../webserver/index.html              (JavaScript, the setup page)
#
# The app is its own repository. This looks for its `app/` directory next to
# this one (../box-app/app) or inside it (./app); set DOCK_APP_ROOT to point
# somewhere else. Without it, the Kotlin checks skip rather than fail, so a
# firmware-only checkout still passes -- but then nothing is holding the app
# to these vectors, which is the whole point, so the skip says so.
#
# A mismatch between them does not look like a bug. It looks like a 401, or a
# button that silently does nothing -- which is exactly how the %2B encoding
# bug behaved. This makes the agreement checkable instead of assumed.
#
# Two kinds of check:
#   1. the canonical vectors are pinned to the SAME literals in all three test
#      suites, so updating one implementation without the others is caught
#   2. each suite actually passes
#
# Exits non-zero if anything disagrees. Safe to run in CI.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)
FAIL=0
SKIP=0

green() { printf '\033[32m%s\033[0m\n' "$1"; }
red()   { printf '\033[31m%s\033[0m\n' "$1"; }
grey()  { printf '\033[90m%s\033[0m\n' "$1"; }

pass() { green "  PASS  $1"; }
fail() { red   "  FAIL  $1"; FAIL=1; }
skip() { grey  "  SKIP  $1"; SKIP=1; }

# --- 1. the shared vectors ---------------------------------------------------
#
# Defined once, here. Each implementation's tests must pin the same literals.
HTTP_VECTOR='4:POST8:/api/cmd3:c=e1:7'
BLOB_VECTOR='3:BLE14:/ble/provision10:net'          # NUL follows; grep stops there
FRAME_BYTES='41'                                     # 8 nonce + 32 sig + 1 payload

C_TEST="firmware/drone_box/test/host/test_auth.c"
PY_CLI="firmware/drone_box/tools/dock_cli.py"

APP_ROOT="${DOCK_APP_ROOT:-}"
if [ -z "$APP_ROOT" ]; then
    for candidate in "$ROOT/app" "$ROOT/../box-app/app"; do
        if [ -d "$candidate/wire" ]; then APP_ROOT=$(cd "$candidate" && pwd); break; fi
    done
fi
KT_TEST="$APP_ROOT/wire/src/commonTest/kotlin/com/dronebox/app/DockCryptoTest.kt"

echo
echo "Shared vectors pinned in all three suites"

SUITES=("$C_TEST")
if [ -n "$APP_ROOT" ]; then
    SUITES+=("$KT_TEST")
else
    skip "no app checkout found -- set DOCK_APP_ROOT to check the Kotlin vectors"
fi

for f in "${SUITES[@]}"; do
    if [ ! -f "$f" ]; then fail "missing $f"; continue; fi
    if grep -qF "$HTTP_VECTOR" "$f"; then
        pass "HTTP canonical pinned in $(basename "$f")"
    else
        fail "HTTP canonical NOT pinned in $(basename "$f") -- expected $HTTP_VECTOR"
    fi
    if grep -qF "$BLOB_VECTOR" "$f"; then
        pass "blob canonical pinned in $(basename "$f")"
    else
        fail "blob canonical NOT pinned in $(basename "$f")"
    fi
done

if [ -n "$APP_ROOT" ] && [ -f "$KT_TEST" ]; then
    if grep -qF "$FRAME_BYTES" "$KT_TEST"; then
        pass "BLE frame size pinned in DockCryptoTest.kt"
    else
        fail "BLE frame size $FRAME_BYTES not asserted in DockCryptoTest.kt"
    fi
fi

# --- 2. Python implementation, checked directly ------------------------------
echo
echo "Python implementation"

if python3 - "$PY_CLI" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("dock_cli", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

assert m.canonical("POST", "/api/cmd", "c=e", 7) == b"4:POST8:/api/cmd3:c=e1:7", \
    m.canonical("POST", "/api/cmd", "c=e", 7)
assert m.canonical_blob("BLE", "/ble/provision", b"net\x00secret", 1) == \
    b"3:BLE14:/ble/provision10:net\x00secret1:1"
assert len(m.ble_write(bytes(32), "/ble/cmd", b"e", 7)) == 41
PY
then
    pass "dock_cli.py canonical, blob and frame match"
else
    fail "dock_cli.py disagrees with the shared vectors"
fi

# --- 3. JavaScript in the setup page ----------------------------------------
#
# The page cannot use crypto.subtle: that is only exposed in secure contexts,
# and the dock serves plain HTTP. So it carries its own HMAC, and that makes it
# a fourth implementation to keep honest.
echo
echo "JavaScript (setup page)"

PAGE="firmware/drone_box/components/webserver/index.html"
if ! command -v node >/dev/null 2>&1; then
    skip "node not installed; cannot exercise the page's crypto"
elif [ ! -f "$PAGE" ]; then
    fail "missing $PAGE"
else
    TMP=$(mktemp -t wirecheck).mjs
    python3 - "$PAGE" "$TMP" <<'EXTRACT'
import sys
src = open(sys.argv[1]).read()
# Everything from the constant table up to where the client code starts.
js = src.split('const K=[')[1].split('let key=null')[0]
open(sys.argv[2], 'w').write('const K=[' + js + """
const hex=b=>[...b].map(x=>x.toString(16).padStart(2,'0')).join('');
let bad=0;
const eq=(n,g,w)=>{ if(g!==w){ console.log('  mismatch in '+n); bad=1; } };
eq('sha256-empty', hex(sha256(new Uint8Array(0))),
   'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
eq('hmac-rfc4231', hex(hmac(new Uint8Array(20).fill(0x0b), enc.encode('Hi There'))),
   'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7');
eq('canonical', new TextDecoder().decode(canonical('POST','/api/cmd','c=e',7)),
   '4:POST8:/api/cmd3:c=e1:7');
const key=new Uint8Array(32).map((_,i)=>(i*7+1)&0xff);
eq('signature', hex(hmac(key, canonical('POST','/api/cmd','c=e',100))),
   '91083fa3f576614df26bee9058b0fd379924e0d21b5cd0e435faa43f7755ce1f');
process.exit(bad);
""")
EXTRACT
    if node "$TMP" 2>/dev/null; then
        pass "page canonical, HMAC and signature match"
    else
        fail "the setup page's crypto disagrees with the shared vectors"
    fi
    rm -f "$TMP"
fi

# --- 4. C host tests ---------------------------------------------------------
echo
echo "C host tests"

FW="$ROOT/firmware/drone_box"
if [ -x "$FW/build-host/test_auth" ]; then
    if (cd "$FW" && ctest --test-dir build-host --output-on-failure >/dev/null 2>&1); then
        pass "auth + command suites"
    else
        fail "C host tests failed -- run: ctest --test-dir build-host --output-on-failure"
    fi
else
    skip "not built. Run: . \$IDF_ACTIVATE && cmake -S test/host -B build-host && cmake --build build-host"
fi

# --- 5. Kotlin tests ---------------------------------------------------------
echo
echo "Kotlin tests"

if [ -z "$APP_ROOT" ]; then
    skip "no app checkout found -- set DOCK_APP_ROOT to run them"
elif [ -x "$APP_ROOT/gradlew" ]; then
    if (cd "$APP_ROOT" && ./gradlew --quiet :wire:jvmTest >/dev/null 2>&1); then
        pass "DockCryptoTest"
    else
        fail "Kotlin tests failed -- run: cd $APP_ROOT && ./gradlew :wire:jvmTest"
    fi
else
    skip "no Gradle wrapper in $APP_ROOT. Open it in Android Studio and sync, or: gradle wrapper"
fi

# --- verdict -----------------------------------------------------------------
echo
if [ "$FAIL" -ne 0 ]; then
    red "WIRE FORMAT MISMATCH -- the dock and its clients do not agree."
    red "Fix before flashing: this surfaces as a 401 or a dead button, not as an error."
    exit 1
fi
if [ "$SKIP" -ne 0 ]; then
    grey "No mismatches, but some suites were skipped -- see SKIP above."
    exit 0
fi
green "All four implementations agree."
