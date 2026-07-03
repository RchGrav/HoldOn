#!/usr/bin/env bash
set -Eeuo pipefail

HOLD_REAL_BIN="${HOLD_BIN:-./hold-dynamic}"
case "$HOLD_REAL_BIN" in
  /*) ;;
  *) HOLD_REAL_BIN="$PWD/$HOLD_REAL_BIN" ;;
esac

TEST_ROOT="$(mktemp -d /tmp/hold-040.XXXXXX)"
cleanup() {
  HOME="$TEST_ROOT/home" HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system" "$HOLD_REAL_BIN" stop web --all >/dev/null 2>&1 || true
  HOME="$TEST_ROOT/home" HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system" "$HOLD_REAL_BIN" stop port-smoke >/dev/null 2>&1 || true
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

export HOME="$TEST_ROOT/home"
export HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system"
export HOLD_TEST_INVOKING_HOME="$TEST_ROOT/home"
mkdir -p "$HOME"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

hold() {
  "$HOLD_REAL_BIN" "$@"
}

extract_display() {
  sed -n '/^[0-9a-f]\{12\}$/p' | head -n1
}

extract_full64() {
  sed -n '/^[0-9a-f]\{64\}$/p' | head -n1
}

out=$(hold run -- /bin/sh -c 'echo smoke-out; echo smoke-err >&2' 2>&1)
printf '%s\n' "$out" | grep -qx 'smoke-out' || fail "foreground stdout missing"
printf '%s\n' "$out" | grep -qx 'smoke-err' || fail "foreground stderr missing"
if printf '%s\n' "$out" | grep -q 'hold  started'; then
  fail "docker-shaped foreground run printed a startup note"
fi
if printf '%s\n' "$out" | grep -Eq '^[0-9a-f]{12}$|^[0-9a-f]{64}$'; then
  fail "docker-shaped foreground run printed an id line"
fi
full=$(basename "$(ls "$HOME/.local/state/hold/"*.log | head -n1)" .log)
[ "${#full}" -eq 64 ] || fail "full 64-hex run id missing from store"
display=${full:0:12}
log="$HOME/.local/state/hold/$full.log"
grep -qx 'smoke-out' "$log" || fail "stdout raw log line missing"
test -f "$log.idx" || fail "raw log sidecar index missing"
head -c 7 "$log.idx" | grep -q '^HLOGIDX' || fail "raw log sidecar magic missing"
hold logs --plain "$display" | grep -qx 'smoke-out' || fail "logs --plain did not print raw log"

named=$(hold run -d --name named-smoke -- /bin/sh -c 'echo named; sleep 0.1' 2>&1 | extract_full64 | cut -c1-12)
[ "${#named}" -eq 12 ] || fail "named run id missing"
sleep 0.3
hold start named-smoke >/tmp/hold-restart.out 2>&1 || fail "restart by run name failed"
restart_id=$(extract_display < /tmp/hold-restart.out)
[ "$restart_id" = "$named" ] || fail "restart did not reuse existing run id"
sleep 0.3
[ "$(hold logs --plain named-smoke | grep -c '^named$')" -eq 2 ] ||
  fail "restart did not append to existing raw log"

if hold run -p 8080:80 -- /bin/true >/tmp/hold-pub.out 2>/tmp/hold-pub.err; then
  fail "publish unexpectedly succeeded"
fi
grep -q 'does not publish or forward ports' /tmp/hold-pub.err ||
  fail "publish rejection message missing"
if hold run -v "$TEST_ROOT:/work" -- /bin/true >/tmp/hold-vol.out 2>/tmp/hold-vol.err; then
  fail "volume unexpectedly succeeded"
fi
grep -q 'does not mount or remap volumes' /tmp/hold-vol.err ||
  fail "volume rejection message missing"

server=$(hold run -d --name port-smoke -- python3 -c 'import socket,time; s=socket.socket(); s.bind(("127.0.0.1",0)); s.listen(); time.sleep(5)' 2>&1 | extract_full64 | cut -c1-12)
[ "${#server}" -eq 12 ] || fail "server id missing"
for _ in $(seq 1 30); do
  psout=$(hold ps 2>/dev/null || true)
  if printf '%s\n' "$psout" | grep -Eq '127\.0\.0\.1:[0-9]+/tcp'; then
    break
  fi
  sleep 0.1
done
printf '%s\n' "$psout" | grep -q '^RUN ID[[:space:]]\+PROFILE[[:space:]]\+COMMAND[[:space:]]\+CREATED[[:space:]]\+STATUS[[:space:]]\+PORTS[[:space:]]\+NAMES' ||
  fail "ps header is not Docker-shaped"
printf '%s\n' "$psout" | grep -Eq '127\.0\.0\.1:[0-9]+/tcp' ||
  fail "observed listening port missing from ps"
hold stop port-smoke >/dev/null 2>&1 || true

hold attach -h 2>&1 | grep -q 'usage: hold attach <target>' || fail "attach help missing"

rmout=$(hold run -d --rm -- /bin/sh -c 'echo rm; sleep 0.1' 2>&1)
rmfull=$(printf '%s\n' "$rmout" | extract_full64)
[ "${#rmfull}" -eq 64 ] || fail "--rm full id missing"
for _ in $(seq 1 50); do
  [ ! -e "$HOME/.local/state/hold/$rmfull.json" ] && [ ! -e "$HOME/.local/state/hold/$rmfull.log" ] && break
  sleep 0.1
done
[ ! -e "$HOME/.local/state/hold/$rmfull.json" ] || fail "--rm record remained"
[ ! -e "$HOME/.local/state/hold/$rmfull.log" ] || fail "--rm log remained"

echo "PASS: hold 0.4 Docker-shaped smoke"
