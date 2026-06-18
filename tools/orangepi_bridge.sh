#!/usr/bin/env bash
set -euo pipefail

APP_DIR="${APP_DIR:-$HOME/voice_ai_mic_server}"
PORT="${PORT:-8787}"
HOST="${HOST:-0.0.0.0}"

cd "$APP_DIR"

pid_file="$APP_DIR/server.pid"
log_file="$APP_DIR/server.log"

is_running() {
  [[ -f "$pid_file" ]] && kill -0 "$(cat "$pid_file")" 2>/dev/null
}

case "${1:-status}" in
  start)
    if is_running; then
      echo "already running: pid $(cat "$pid_file")"
      exit 0
    fi

    mkdir -p tools/recordings tools/sketches
    nohup python3 tools/stt_bridge_openai.py \
      --host "$HOST" \
      --port "$PORT" \
      --config tools/stt_config.json \
      --recordings-dir tools/recordings \
      --sketches-dir tools/sketches \
      > "$log_file" 2>&1 &
    echo $! > "$pid_file"
    echo "started: pid $(cat "$pid_file")"
    ;;

  stop)
    if is_running; then
      kill "$(cat "$pid_file")"
      echo "stopped: pid $(cat "$pid_file")"
    else
      echo "not running"
    fi
    rm -f "$pid_file"
    ;;

  restart)
    "$0" stop
    "$0" start
    ;;

  status)
    if is_running; then
      echo "running: pid $(cat "$pid_file")"
    else
      echo "not running"
    fi
    ;;

  logs)
    tail -n "${LINES:-80}" "$log_file"
    ;;

  *)
    echo "usage: $0 {start|stop|restart|status|logs}" >&2
    exit 2
    ;;
esac
