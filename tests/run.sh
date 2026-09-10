#!/bin/bash
# WildanDev GTPS integration test: boots a fresh game server, runs the ENet
# test client through login + world + gameplay + moderation, then cleans up.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
CONF="$ROOT/resources/db.conf"

conf_get() {
  grep "^$1=" "$CONF" | head -n1 | cut -d= -f2- | tr -d '\r'
}
DB_HOST="$(conf_get host)"
DB_USER="$(conf_get user)"
DB_PASS="$(conf_get password)"
DB_NAME="$(conf_get name)"
DB_ARGS=(-h "$DB_HOST" -u "$DB_USER" -p"$DB_PASS")

# Build gate: make's exit status decides, not a grep of its output.
if ! make all tests >/tmp/wildandev-build.log 2>&1; then
  cat /tmp/wildandev-build.log
  echo "BUILD FAILED"
  exit 1
fi

./build/test-units

mariadb "${DB_ARGS[@]}" "$DB_NAME" <<'SQL' 2>/dev/null || true
DELETE FROM players WHERE growid IN ('TestUser','AdminUser');
DELETE FROM worlds WHERE name='TEST';
SQL

# Suites run on an isolated port so a live supervisor is never disturbed.
export WILDANDEV_TEST_PORT=17092
export WILDANDEV_PORT=17092

# Seed a staff account (role 2 = Admin) hashed exactly like the server does:
# PBKDF2-HMAC-SHA256 over the raw salt bytes, stored as
# pbkdf2_sha256$iterations$salthex$hashhex. Widen the hash column first for
# databases created before PBKDF2 (the server does the same on startup).
mariadb "${DB_ARGS[@]}" "$DB_NAME" \
  -e "ALTER TABLE players MODIFY pass_hash VARCHAR(255) NOT NULL; ALTER TABLE players MODIFY salt VARCHAR(255) NOT NULL;" \
  2>/dev/null || true
python3 - <<'EOF' | mariadb "${DB_ARGS[@]}" "$DB_NAME"
import hashlib
salt_hex = b"integrationtestsalt0000000000000".hex()
digest = hashlib.pbkdf2_hmac("sha256", b"adminpass", bytes.fromhex(salt_hex), 100000).hex()
stored = "pbkdf2_sha256$100000$%s$%s" % (salt_hex, digest)
print("INSERT INTO players (growid, pass_hash, salt, role_id) VALUES "
      "('AdminUser', '%s', '%s', 2);" % (stored, salt_hex))
EOF

./build/WildanDev-game > build/integration-server.log 2>&1 &
SERVER_PID=$!
sleep 2

STATUS=0
run_suite() {
  local label="$1"; shift
  echo "=== $label ==="
  if ! "$@"; then
    echo "SUITE FAILED: $label"
    STATUS=1
  fi
}

echo "=== player suite ==="
./build/test-client TestUser secret123 player || STATUS=1

echo "=== bad password suite ==="
./build/test-client TestUser wrongpass badpass || STATUS=1

echo "=== orphan identity suite ==="
./build/test-client OrphanUser somepass orphan || STATUS=1

echo "=== reconnect suite ==="
./build/test-client TestUser secret123 reconnect || STATUS=1

echo "=== admin suite ==="
./build/test-client AdminUser adminpass admin || STATUS=1

kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

echo "--- ban persistence check ---"
mariadb "${DB_ARGS[@]}" -N -e \
  "SELECT CONCAT(growid, '|banned=', is_banned) FROM players WHERE growid='AdminUser';" \
  "$DB_NAME" 2>/dev/null || true

echo "--- persistence check ---"
mariadb "${DB_ARGS[@]}" -N -e \
  "SELECT CONCAT(name, '|owner=', owner, '|bytes=', OCTET_LENGTH(blocks)) FROM worlds WHERE name='TEST';" \
  "$DB_NAME" 2>/dev/null || true
mariadb "${DB_ARGS[@]}" "$DB_NAME" <<'SQL' 2>/dev/null || true
DELETE FROM players WHERE growid IN ('TestUser','AdminUser','OrphanUser');
DELETE FROM worlds WHERE name='TEST';
SQL

echo "--- server log ---"
cat build/integration-server.log

if [ "$STATUS" -ne 0 ]; then
  echo "SUITES FAILED"
  exit 1
fi
echo "ALL SUITES PASSED"
