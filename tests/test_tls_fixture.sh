#!/usr/bin/env bash
# The TLS terminator the M0-1 probe aims at: scripts/orca/tls-fixture.sh and the
# `tls` profile in server/testing/docker-compose.yml.
#
#   test_tls_fixture.sh isolated   the profile keeps it out of the ordinary rig,
#                                  and its port is bound to loopback only. This
#                                  is the regression that matters: a TLS service
#                                  that started with every `compose.sh up -d`
#                                  would change what every other test talks to,
#                                  and one bound to 0.0.0.0 would put a fixture
#                                  RomM on the LAN.
#   test_tls_fixture.sh cert       the minted certificate is actually usable:
#                                  subjectAltName for the name the probe asks
#                                  for, a private key only this user can read,
#                                  and a refusal rather than a silent bad cert
#                                  when openssl will not honour -addext.
#   test_tls_fixture.sh serves     it really terminates TLS in front of the real
#                                  RomM -- verified against the CA, and once more
#                                  pinned to TLS 1.2, which is the ceiling of
#                                  libnx's SslVersion_Auto and therefore the only
#                                  version the probe's handshake can rely on.
#
# `isolated` and `cert` need nothing but the checkout and openssl, so they never
# skip. `serves` needs Docker and this worktree's RomM, so it skips without one.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE="$REPO_ROOT/server/testing/docker-compose.yml"
FIXTURE="$REPO_ROOT/scripts/orca/tls-fixture.sh"
CERT_NAME="romm.fixture.local"
SKIP=77

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit $SKIP; }

# The romm-tls service block: from `  romm-tls:` to the next key at the same
# indentation, so an assertion cannot be satisfied by another service's line.
tls_service() {
  awk '/^  romm-tls:/ { in_svc = 1; next }
       in_svc && /^  [^ #]+:/ { in_svc = 0 }
       in_svc { print }' "$COMPOSE"
}

phase_isolated() {
  local block
  block="$(tls_service)"
  [ -n "$block" ] || fail "no romm-tls service in $COMPOSE"

  grep -qE '^\s*profiles:.*\btls\b' <<<"$block" ||
    fail "romm-tls is not behind the 'tls' profile; every compose.sh up -d would start it"

  local ports
  ports="$(grep -A2 -E '^\s*ports:' <<<"$block" | grep -oE '"[^"]+"')"
  [ -n "$ports" ] || fail "romm-tls publishes no port"
  grep -q '127.0.0.1:' <<<"$ports" ||
    fail "romm-tls does not bind to 127.0.0.1: $ports"

  # The far end. tests/test_policy.py checks this too, from the other side; the
  # duplication is deliberate, because a TLS front door onto something other
  # than this project's own RomM is the failure both are guarding.
  grep -qE '^\s*UPSTREAM:\s*http://romm:8080\s*$' <<<"$block" ||
    fail "romm-tls does not forward to this project's romm service"

  echo "ok: romm-tls is profile-gated, loopback-bound and points at romm"
}

phase_cert() {
  command -v openssl >/dev/null 2>&1 || skip "no openssl"

  "$FIXTURE" cert >/dev/null || fail "tls-fixture.sh cert failed"

  local cert="$REPO_ROOT/server/testing/tls/generated/server.crt"
  local key="$REPO_ROOT/server/testing/tls/generated/server.key"
  [ -s "$cert" ] || fail "no certificate at $cert"
  [ -s "$key" ] || fail "no key at $key"

  # Without this the probe cannot keep hostname verification on, and the
  # alternative -- turning verification off to make the fixture work -- is
  # exactly the shortcut docs/SECURITY.md exists to prevent becoming a habit.
  openssl x509 -in "$cert" -noout -text | grep -q "DNS:$CERT_NAME" ||
    fail "the fixture certificate has no subjectAltName for $CERT_NAME"

  # GNU `stat` first, and the result validated rather than inferred from an exit
  # status. `-f` is BSD's "file format" and GNU's "filesystem", so on Linux
  # `stat -f '%Lp'` prints a filesystem report to stdout *and then* exits 1 --
  # which means the obvious `stat -f ... || stat -c ...` runs both and the
  # command substitution captures the report with the mode glued on the end.
  # That is what this looked like in CI: "mode ... Type: ext2/ext3 ... 600, not
  # 600".
  local mode
  mode="$(stat -c '%a' "$key" 2>/dev/null)" || true
  case "$mode" in
    ''|*[!0-7]*) mode="$(stat -f '%Lp' "$key" 2>/dev/null)" || true ;;
  esac
  [ "$mode" = "600" ] || fail "the fixture private key is mode '$mode', not 600"

  git -C "$REPO_ROOT" check-ignore -q "$key" ||
    fail "the fixture private key is not gitignored (CLAUDE.md hard rule 5)"

  echo "ok: certificate carries $CERT_NAME, key is 600 and ignored"
}

phase_serves() {
  command -v docker >/dev/null 2>&1 || skip "no docker"
  docker info >/dev/null 2>&1 || skip "docker daemon not running"
  command -v curl >/dev/null 2>&1 || skip "no curl"

  [ -f "$REPO_ROOT/.env" ] || "$REPO_ROOT/scripts/orca/env.sh" >/dev/null
  # shellcheck disable=SC1091
  set -a; . "$REPO_ROOT/.env"; set +a
  [ -n "${TLS_PORT:-}" ] || fail "no TLS_PORT in .env"

  # Printed rather than swallowed: in CI this is a failure, not a skip
  # (ROMMSYNC_REQUIRE_RIG), and a failure whose only symptom is "could not start"
  # costs an afternoon.
  local started
  if ! started="$("$FIXTURE" up 2>&1)"; then
    echo "$started" >&2
    skip "could not start the TLS fixture (is the rig up? compose.sh up -d)"
  fi

  # nginx needs a moment after `up` before it answers, and the first run of this
  # test on a machine also waits for the image.
  local deadline=$((SECONDS + 60)) out=""
  while [ $SECONDS -lt $deadline ]; do
    if out="$("$FIXTURE" check 2>&1)"; then
      grep -q "HTTP 200 (verified)" <<<"$out" ||
        fail "the fixture answered, but not with a verified 200: $out"
      grep -q "HTTP 200 (TLS 1.2)" <<<"$out" ||
        fail "the fixture does not serve TLS 1.2, which is SslVersion_Auto's ceiling: $out"
      echo "ok: TLS terminator serves the fixture RomM, verified and over TLS 1.2"
      return 0
    fi
    sleep 1
  done
  fail "the TLS fixture never answered: $out"
}

case "${1:-}" in
  isolated) phase_isolated ;;
  cert)     phase_cert ;;
  serves)   phase_serves ;;
  *)        echo "usage: $0 {isolated|cert|serves}" >&2; exit 2 ;;
esac
