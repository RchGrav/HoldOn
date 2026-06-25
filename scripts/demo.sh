#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

latest_review_bin() {
  find review-builds -maxdepth 2 -type f -name hold-dynamic 2>/dev/null | sort | tail -n 1 || true
}

hold_bin="${HOLD_BIN:-}"
if [[ -z "$hold_bin" ]]; then
  if [[ -x ./hold ]]; then
    hold_bin="./hold"
  else
    hold_bin="$(latest_review_bin)"
  fi
fi
if [[ -z "$hold_bin" || ! -x "$hold_bin" ]]; then
  echo "No hold binary found. Run: make review-build" >&2
  exit 1
fi
hold_bin="$(realpath "$hold_bin")"
state_file=".review-demo-runs.env"

stop_existing() {
  if [[ -f "$state_file" ]]; then
    # shellcheck disable=SC1090
    source "$state_file" || true
    for id in ${WEB_API_DEMO_ID:-} ${WORKER_DEMO_ID:-}; do
      [[ -n "$id" ]] && "$hold_bin" stop "$id" >/dev/null 2>&1 || true
    done
  fi
}

case "${1:-start}" in
  start)
    stop_existing
    web_id="$($hold_bin run -- examples/workloads/web-api-demo 2>&1 | awk '/started/ {print $3; exit}')"
    worker_id="$($hold_bin run -- examples/workloads/worker-demo 2>&1 | awk '/started/ {print $3; exit}')"
    import_id="$($hold_bin run -- examples/workloads/import-once-demo 2>&1 | awk '/started/ {print $3; exit}')"
    burst_id="$($hold_bin run -- examples/workloads/burst-once-demo 2>&1 | awk '/started/ {print $3; exit}')"
    sleep 1
    cat > "$state_file" <<ENV
export HOLD_BIN='$hold_bin'
export WEB_API_DEMO_ID='$web_id'
export WORKER_DEMO_ID='$worker_id'
export IMPORT_DEMO_ID='$import_id'
export BURST_DEMO_ID='$burst_id'
ENV
    cat <<OUT
Demo workloads are running under normal Hold state.

Binary:
  $hold_bin

Run IDs:
  WEB_API_DEMO_ID=$web_id       live web/API log, writes every second
  WORKER_DEMO_ID=$worker_id     live background worker, writes every 2 seconds
  IMPORT_DEMO_ID=$import_id     completed import job with INFO/WARN/ERROR
  BURST_DEMO_ID=$burst_id       completed 2500-line sparse-match log

Copy/paste these:

  $hold_bin status
  $hold_bin logs $web_id --follow
  $hold_bin logs $worker_id --follow
  $hold_bin logs $import_id
  $hold_bin logs $burst_id

Inside the full-screen log viewer, just type to filter:
  error
  timeout
  db
  payment
  RARE_MATCH

Backspace relaxes the filter. Space excludes lines like the highlighted line. Ctrl-R resets filters. q quits.

Saved variables:
  source $state_file

Stop live demos:
  make demo-stop
OUT
    ;;
  stop)
    stop_existing
    echo "Stopped live demo runs from $state_file if they were still running."
    ;;
  status)
    if [[ -f "$state_file" ]]; then
      # shellcheck disable=SC1090
      source "$state_file"
    fi
    "$hold_bin" status
    ;;
  *)
    echo "usage: scripts/demo.sh [start|stop|status]" >&2
    exit 5
    ;;
esac
