#!/bin/bash
# WildanDev GTPS login service supervisor.
# Usage: run-login.sh {start|stop|status|restart}
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIDFILE="$ROOT/run/login.pid"
LOCKFILE="$ROOT/run/login.lock"
LOGFILE="$ROOT/run/login.log"

mkdir -p "$ROOT/run"

# Serialise start/stop between concurrent invocations.
lock() { exec 9>"$LOCKFILE"; flock -n 9 || { echo "another login operation in progress"; exit 1; }; }

is_running() {
  [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

case "${1:-status}" in
  start)
    lock
    if is_running; then echo "login service already running"; exit 0; fi
    rm -f "$PIDFILE"
    cd "$ROOT"
    nohup python3 login/backend.py >>"$LOGFILE" 2>&1 9>&- &
    echo $! > "$PIDFILE"
    sleep 0.5
    if ! kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      echo "login service failed to start, see $LOGFILE" >&2
      rm -f "$PIDFILE"
      exit 1
    fi
    echo "login service started (pid $(cat "$PIDFILE"))"
    ;;
  stop)
    lock
    is_running || { echo "login service not running"; exit 0; }
    PID="$(cat "$PIDFILE")"
    kill "$PID"
    for _ in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
    if kill -0 "$PID" 2>/dev/null; then
      echo "did not stop cleanly" >&2
      exit 1
    fi
    rm -f "$PIDFILE"
    echo "login service stopped"
    ;;
  status)
    is_running && echo "login service running (pid $(cat "$PIDFILE"))" || echo "login service stopped"
    ;;
  restart)
    "$0" stop; sleep 1; "$0" start
    ;;
  *)
    echo "Usage: $0 {start|stop|status|restart}"; exit 1
    ;;
esac
