#!/usr/bin/env bash
# The TLS terminator in front of this worktree's RomM, for the M0-1 probe.
#
# The rig speaks plain HTTP because every host test reaches it over loopback
# (docs/TESTING.md). The probe cannot use that: the whole question M0-1 asks is
# whether the Horizon `ssl` service can complete a handshake, so it needs a real
# one to complete. This mints a throwaway certificate and starts the compose
# `tls` profile in front of RomM -- nothing else in the rig changes, and an
# ordinary `compose.sh up -d` still starts neither.
#
#   ./scripts/orca/tls-fixture.sh up       mint the cert if missing, start it
#   ./scripts/orca/tls-fixture.sh down     stop it (the cert survives)
#   ./scripts/orca/tls-fixture.sh cert     (re-)mint the certificate only
#   ./scripts/orca/tls-fixture.sh ini      print the probe's ini for this worktree
#   ./scripts/orca/tls-fixture.sh check    curl it, verifying against the CA
#
# Bound to 127.0.0.1, like every other port this rig publishes. That is enough
# for an emulator running on this machine -- Ryujinx's sockets are the host's --
# and deliberately not enough for a console on the LAN: pointing hardware at
# anything is M8 work behind the M8-1 gate (CLAUDE.md hard rule 1).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

CERT_DIR="server/testing/tls/generated"
CERT="$CERT_DIR/server.crt"
KEY="$CERT_DIR/server.key"

# The name on the certificate. The probe connects to an address and asks for
# this name with SNI, which is what lets hostname verification stay ON against a
# fixture that has no DNS -- see tlsprobe/README.md.
CERT_NAME="romm.fixture.local"

[ -f .env ] || ./scripts/orca/env.sh >/dev/null
set -a; . ./.env; set +a
: "${TLS_PORT:?no TLS_PORT in .env; re-run ./scripts/orca/env.sh}"

mint_cert() {
  command -v openssl >/dev/null 2>&1 || { echo "no openssl" >&2; exit 1; }
  mkdir -p "$CERT_DIR"
  # Self-signed and its own CA: the probe imports this same file through
  # sslContextImportServerPki, which takes CAs or server certs. RSA rather than
  # an EC key because the ssl service's TLS 1.2 suites are the conservative set
  # and this is not the variable under test.
  local log
  log="$(mktemp)"
  if ! openssl req -x509 -newkey rsa:2048 -nodes \
      -keyout "$KEY" -out "$CERT" -days 365 \
      -subj "/CN=$CERT_NAME/O=rommsync-nx test fixture" \
      -addext "subjectAltName=DNS:$CERT_NAME,DNS:localhost,IP:127.0.0.1" \
      -addext "basicConstraints=critical,CA:TRUE" \
      -addext "keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign" \
      >"$log" 2>&1; then
    cat "$log" >&2
    rm -f "$log"
    echo "openssl could not mint the fixture certificate" >&2
    exit 1
  fi
  rm -f "$log"
  chmod 600 "$KEY"

  # macOS ships LibreSSL and Linux ships OpenSSL, and they have disagreed about
  # -addext before. A certificate with no subjectAltName still verifies against
  # nothing and fails hostname verification on the console with a Result that
  # says nothing about why -- so this is checked here, where the message can.
  if ! openssl x509 -in "$CERT" -noout -text | grep -q "DNS:$CERT_NAME"; then
    echo "the minted certificate has no subjectAltName for $CERT_NAME; " \
         "$(openssl version) did not honour -addext" >&2
    exit 1
  fi
  echo "minted $CERT (CN=$CERT_NAME, 365 days)"

  # nginx reads the certificate once, at start-up. Re-minting under a running
  # terminator would leave it serving the old one -- and the next `check` would
  # fail against a certificate that is on disk and correct, which is the most
  # confusing possible way for this to break.
  if [ -n "$(./scripts/orca/compose.sh --profile tls ps -q romm-tls 2>/dev/null)" ]; then
    ./scripts/orca/compose.sh --profile tls restart romm-tls >/dev/null
    echo "restarted romm-tls onto the new certificate"
  fi
}

case "${1:-up}" in
  cert)
    mint_cert
    ;;
  up)
    [ -f "$CERT" ] && [ -f "$KEY" ] || mint_cert
    ./scripts/orca/compose.sh --profile tls up -d romm-tls
    echo "TLS fixture on $TLS_BASE_URL -> http://romm:8080"
    ;;
  down)
    ./scripts/orca/compose.sh --profile tls stop romm-tls
    ./scripts/orca/compose.sh --profile tls rm -f romm-tls
    ;;
  check)
    [ -f "$CERT" ] || { echo "no certificate; run: $0 cert" >&2; exit 1; }
    # --resolve rather than --insecure: this checks that the certificate is
    # actually valid for the name the probe will ask for, which is the half a
    # bare `curl -k` would skip.
    curl -sS --fail --cacert "$CERT" \
      --resolve "$CERT_NAME:$TLS_PORT:127.0.0.1" \
      -o /dev/null -w 'HTTP %{http_code} (verified)\n' \
      "https://$CERT_NAME:$TLS_PORT/api/heartbeat"
    # And again pinned to TLS 1.2, because that is the ceiling of libnx's
    # SslVersion_Auto: a terminator that only speaks 1.3 would fail the probe's
    # handshake for a reason that has nothing to do with the ssl service.
    curl -sS --fail --cacert "$CERT" --tlsv1.2 --tls-max 1.2 \
      --resolve "$CERT_NAME:$TLS_PORT:127.0.0.1" \
      -o /dev/null -w 'HTTP %{http_code} (TLS 1.2)\n' \
      "https://$CERT_NAME:$TLS_PORT/api/heartbeat"
    ;;
  ini)
    cat <<EOF
# sdmc:/switch/rommsync-tlsprobe.ini -- this worktree's fixture.
# The CA is $REPO_ROOT/$CERT;
# copy it to sdmc:/switch/rommsync-fixture-ca.pem next to this file.
host = 127.0.0.1
port = $TLS_PORT
path = /api/heartbeat
sni = $CERT_NAME
ca_pem = sdmc:/switch/rommsync-fixture-ca.pem
verify = 1
EOF
    ;;
  *)
    echo "usage: $0 {up|down|cert|check|ini}" >&2
    exit 2
    ;;
esac
