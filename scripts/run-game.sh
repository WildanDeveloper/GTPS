#!/bin/bash
# WildanDev GTPS game server supervisor.
# Usage: run-game.sh {start|stop|status|restart}
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/WildanDev-game"
PIDFILE="$ROOT/run/game.pid"
LOCKFILE="$ROOT/run/game.lock"
LOGFILE="$ROOT/run/game.log"

mkdir -p "$ROOT/run"

# Serialise start/stop between concurrent invocations.
lock() { exec 9>"$LOCKFILE"; flock -n 9 || { echo "another game operation in progress"; exit 1; }; }

is_running() {
  [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

case "${1:-status}" in
  start)
    lock
    if is_running; then echo "game server already running"; exit 0; fi
    rm -f "$PIDFILE"
    [ -x "$BIN" ] || { echo "missing binary, run: make"; exit 1; }
    cd "$ROOT"
    nohup "$BIN" >>"$LOGFILE" 2>&1 9>&- &
    echo $! > "$PIDFILE"
    sleep 0.2
    if ! kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      echo "game server failed to start, see $LOGFILE" >&2
      rm -f "$PIDFILE"
      exit 1
    fi
    echo "game server started (pid $(cat "$PIDFILE"))"
    ;;
  stop)
    lock
    is_running || { echo "game server not running"; exit 0; }
    PID="$(cat "$PIDFILE")"
    kill "$PID"
    for _ in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
    if kill -0 "$PID" 2>/dev/null; then
      echo "did not stop cleanly" >&2
      exit 1
    fi
    rm -f "$PIDFILE"
    echo "game server stopped"
    ;;
  status)
    is_running && echo "game server running (pid $(cat "$PIDFILE"))" || echo "game server stopped"
    ;;
  restart)
    "$0" stop; sleep 1; "$0" start
    ;;
  *)
    echo "Usage: $0 {start|stop|status|restart}"; exit 1
    ;;
esac
