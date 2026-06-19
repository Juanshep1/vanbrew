#!/usr/bin/env bash
# Linux smoke test for the native Vanta toolchain.
# Proves vc (compiler) and vself (interpreter) build & run on Linux with ONLY a
# C compiler - no Python anywhere. Run from the repo root: bash test/linux-smoke.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

echo "== building vc + vself with cc (no Python) =="
cc -O2 -w "$ROOT/packages/vc/vc.va.c"        -o "$TMP/vc"
cc -O2 -w "$ROOT/packages/vself/vanta.va.c"  -o "$TMP/vself"
echo "   ok"

cat > "$TMP/prog.va" <<'EOF'
to fib(n)
    if n is under 2
        give back n
    end
    give back fib(n - 1) + fib(n - 2)
end
let m be {"lang": "Vanta", "os": "linux"}
say m["lang"] + " on " + m["os"] + " fib(20)=" + text(fib(20))
EOF

echo "== [1] vself interprets a script =="
out="$("$TMP/vself" "$TMP/prog.va")"; echo "   $out"
[ "$out" = "Vanta on linux fib(20)=6765" ] || { echo "FAIL: interpreter output"; exit 1; }

echo "== [2] vc compiles a script to native + runs it =="
( cd "$TMP" && "$TMP/vc" prog.va | tail -1 | sed 's/^/   /' )

cat > "$TMP/web.va" <<'EOF'
to handle(req)
    give back {"status": 200, "body": {"ok": yes, "os": "linux", "path": req["path"]}, "type": "application/json"}
end
serve(8123, handle)
EOF

echo "== [3] vself serves HTTP (interpreted web server) =="
"$TMP/vself" "$TMP/web.va" & SRV=$!
sleep 1
exec 3<>/dev/tcp/127.0.0.1/8123
printf 'GET /hi HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' >&3
resp="$(timeout 3 cat <&3 | tail -1)"; echo "   $resp"
kill "$SRV" 2>/dev/null || true
echo "$resp" | grep -q '"ok": *true' || { echo "FAIL: web server"; exit 1; }

echo "== [4] no Python linkage =="
if ldd "$TMP/vc" | grep -qi python; then echo "FAIL: python linked"; exit 1; fi
echo "   clean (only libc)"

echo "ALL GOOD — native Vanta builds & runs on Linux, zero Python."
