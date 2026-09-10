#!/bin/bash
# WildanDev GTPS database backup (MariaDB dump with timestamp).
# Usage: backup-db.sh [output_dir]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONF="$ROOT/resources/db.conf"

conf_get() {
  # Value after the first '=' on the first matching line; strips CR.
  grep "^$1=" "$CONF" | head -n1 | cut -d= -f2- | tr -d '\r'
}

DB_HOST="$(conf_get host)"
DB_USER="$(conf_get user)"
DB_PASS="$(conf_get password)"
DB_NAME="$(conf_get name)"
if [ -z "$DB_NAME" ]; then
  echo "backup failed: cannot read $CONF" >&2
  exit 1
fi

OUT_DIR="${1:-$ROOT/backups}"
mkdir -p "$OUT_DIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
FILE="$OUT_DIR/${DB_NAME}_${STAMP}.sql.gz"

# pipefail + set -e: a failed dump aborts instead of leaving a silent
# truncated archive behind. Credentials come from db.conf.
trap 'rm -f "$FILE"' ERR
mariadb-dump --single-transaction -h "$DB_HOST" -u "$DB_USER" -p"$DB_PASS" "$DB_NAME" | gzip > "$FILE"
trap - ERR

[ -s "$FILE" ] || { echo "backup failed: empty dump" >&2; rm -f "$FILE"; exit 1; }
echo "backup written to $FILE"
ls -la "$FILE"
