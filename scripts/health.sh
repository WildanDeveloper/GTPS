#!/bin/bash
# WildanDev GTPS health check: login HTTPS + game UDP port + process status.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONF="$ROOT/resources/db.conf"
FAIL=0

conf_get() {
  grep "^$1=" "$CONF" | head -n1 | cut -d= -f2- | tr -d '\r'
}

curl -sk --max-time 5 https://127.0.0.1:8092/ 2>/dev/null | grep -q "WildanDev login ok" \
  && echo "[OK] login service" || { echo "[FAIL] login service"; FAIL=1; }

if command -v nc >/dev/null 2>&1; then
  echo "ping" | nc -u -w2 127.0.0.1 17091 >/dev/null 2>&1
  echo "[INFO] game UDP port probed (no reply expected for ENet)"
else
  echo "[INFO] nc missing, skipping UDP probe"
fi

"$ROOT/scripts/run-game.sh" status
"$ROOT/scripts/run-login.sh" status

# Database check with real credentials from db.conf (root socket auth is not
# guaranteed).
if mariadb -h "$(conf_get host)" -u "$(conf_get user)" -p"$(conf_get password)" -N \
     -e "SELECT COUNT(*) FROM players;" "$(conf_get name)" >/dev/null 2>&1; then
  echo "[OK] database reachable"
else
  echo "[FAIL] database"
  FAIL=1
fi

exit $FAIL
