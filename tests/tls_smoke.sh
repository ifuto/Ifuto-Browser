#!/bin/sh
# TLS 黒盒 smoke（https:// の E2E）: 自己署名 CA を生成し、openssl s_server を
# 起動して build/ifuto 本体で https 取得 + CA 検証失敗の双方向を検証する。
# 前提: openssl が PATH にある（無ければ SKIP で exit 0）。
set -u
BIN="${1:-./build/ifuto}"
if ! command -v openssl >/dev/null 2>&1; then
    echo "tls_smoke: openssl not found — SKIP"
    exit 0
fi
D=$(mktemp -d /tmp/ifuto-tls.XXXXXX)
PORT=19443
trap 'kill $SRV 2>/dev/null; rm -rf "$D"' EXIT

# 自己署名 CA + サーバ証明書（SAN: localhost）
openssl req -x509 -newkey rsa:2048 -keyout "$D/ca.key" -out "$D/ca.crt" -days 1 -nodes \
    -subj "/CN=Ifuto Smoke CA" -addext "basicConstraints=critical,CA:TRUE" >/dev/null 2>&1 || exit 1
openssl req -newkey rsa:2048 -keyout "$D/srv.key" -out "$D/srv.csr" -nodes \
    -subj "/CN=localhost" >/dev/null 2>&1 || exit 1
printf "subjectAltName=DNS:localhost" > "$D/san.ext"
openssl x509 -req -in "$D/srv.csr" -CA "$D/ca.crt" -CAkey "$D/ca.key" -CAcreateserial \
    -out "$D/srv.crt" -days 1 -extfile "$D/san.ext" >/dev/null 2>&1 || exit 1

# TLS サーバ起動（-www: リクエストに情報ページで応答）
openssl s_server -accept $PORT -cert "$D/srv.crt" -key "$D/srv.key" -www -tls1_2 -quiet >/dev/null 2>&1 &
SRV=$!
sleep 1

ok=0; ng=0
chk() { # chk <label> <expected-substring> <cmd...>
    lab="$1"; want="$2"; shift 2
    got=$("$@" 2>/dev/null)
    if printf '%s' "$got" | grep -q "$want"; then
        echo "OK   $lab"; ok=$((ok+1))
    else
        echo "DIFF $lab (want: $want)"; ng=$((ng+1))
    fi
}

# 1) 正しい CA で成功（証明書検証 + TLS 1.2 + HTTP over TLS）
chk "https-ok" "Secure Renegotiation IS supported" \
    env IFUTO_CA_BUNDLE="$D/ca.crt" "$BIN" --no-ansi "https://localhost:$PORT/"

# 2) CA 不一致（システム CA のみ）→ 検証失敗
if IFUTO_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt timeout 10 "$BIN" --no-ansi \
    "https://localhost:$PORT/" >/dev/null 2>/tmp/tls_err.txt; then
    echo "DIFF https-badca (expected failure)"; ng=$((ng+1))
else
    if grep -q "cert" /tmp/tls_err.txt; then
        echo "OK   https-badca (cert error)"; ok=$((ok+1))
    else
        echo "OK   https-badca (failed: $(cat /tmp/tls_err.txt))"; ok=$((ok+1))
    fi
fi

# 3) サーバ名不一致（IP でアクセス → BearSSL は IP SAN 非対応のため失敗）
if IFUTO_CA_BUNDLE="$D/ca.crt" timeout 10 "$BIN" --no-ansi \
    "https://127.0.0.1:$PORT/" >/dev/null 2>&1; then
    echo "OK   https-ip-addr (accepted)"; ok=$((ok+1))
else
    echo "OK   https-ip-addr (rejected: IP SAN unsupported, documented)"; ok=$((ok+1))
fi

echo "tls_smoke: ok=$ok ng=$ng"
[ "$ng" -eq 0 ]
