#!/usr/bin/env bash
set -Eeuo pipefail

HOLD_REAL_BIN="${HOLD_BIN:-./grutty-dynamic}"
case "$HOLD_REAL_BIN" in
  /*) ;;
  *) HOLD_REAL_BIN="$PWD/$HOLD_REAL_BIN" ;;
esac

TEST_ROOT="$(mktemp -d /tmp/grutty-040.XXXXXX)"
cleanup() {
  HOME="$TEST_ROOT/home" HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system" "$HOLD_REAL_BIN" end web --all >/dev/null 2>&1 || true
  HOME="$TEST_ROOT/home" HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system" "$HOLD_REAL_BIN" end port-smoke >/dev/null 2>&1 || true
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

grutty() {
  "$HOLD_REAL_BIN" "$@"
}

extract_full64() {
  sed -n '/^[0-9a-f]\{64\}$/p' | head -n1
}

# Bare foreground: stream the process output, print no id and no startup note.
out=$(grutty -- /bin/sh -c 'echo smoke-out; echo smoke-err >&2' 2>&1)
printf '%s\n' "$out" | grep -qx 'smoke-out' || fail "foreground stdout missing"
printf '%s\n' "$out" | grep -qx 'smoke-err' || fail "foreground stderr missing"
if printf '%s\n' "$out" | grep -q 'grutty  started'; then
  fail "foreground call printed a startup note"
fi
if printf '%s\n' "$out" | grep -Eq '^[0-9a-f]{12}$|^[0-9a-f]{64}$'; then
  fail "foreground call printed an id line"
fi
full=$(basename "$(ls "$HOME/.local/state/hold/"*.log | head -n1)" .log)
[ "${#full}" -eq 64 ] || fail "full 64-hex call id missing from store"
display=${full:0:12}
log="$HOME/.local/state/hold/$full.log"
grep -qx 'smoke-out' "$log" || fail "stdout raw log line missing"
test ! -e "$log.idx" || fail "a log index sidecar was written"
grutty logs --plain "$display" | grep -qx 'smoke-out' || fail "logs --plain did not print raw log"
grutty logs -p "$display" | grep -qx 'smoke-out' || fail "logs -p did not print raw log"

# Named detached call, then redial by name reuses the same call id and log.
named=$(grutty -d --name named-smoke -- /bin/sh -c 'echo named; sleep 0.1' 2>&1 | extract_full64 | cut -c1-12)
[ "${#named}" -eq 12 ] || fail "named call id missing"
sleep 0.3
redial_id=$(grutty -d named-smoke 2>&1 | extract_full64 | cut -c1-12) || fail "redial by call name failed"
[ "$redial_id" = "$named" ] || fail "redial did not reuse existing call id"
sleep 0.3
[ "$(grutty logs --plain named-smoke | grep -c '^named$')" -eq 2 ] ||
  fail "redial did not append to existing raw log"

# rename gives a call a meaningful name.
grutty rename named-smoke renamed-smoke >/dev/null 2>&1 || fail "rename failed"
grutty ps -a | grep -q 'renamed-smoke' || fail "rename did not update the call name"

if grutty -p 8080:80 -- /bin/true >/tmp/grutty-pub.out 2>/tmp/grutty-pub.err; then
  fail "publish unexpectedly succeeded"
fi
grep -q 'does not publish or forward ports' /tmp/grutty-pub.err ||
  fail "publish rejection message missing"
if grutty -v "$TEST_ROOT:/work" -- /bin/true >/tmp/grutty-vol.out 2>/tmp/grutty-vol.err; then
  fail "volume unexpectedly succeeded"
fi
grep -q 'does not mount or remap volumes' /tmp/grutty-vol.err ||
  fail "volume rejection message missing"

server=$(grutty -d --name port-smoke -- python3 -c 'import socket,time; s=socket.socket(); s.bind(("127.0.0.1",0)); s.listen(); time.sleep(5)' 2>&1 | extract_full64 | cut -c1-12)
[ "${#server}" -eq 12 ] || fail "server id missing"
for _ in $(seq 1 30); do
  psout=$(grutty ps 2>/dev/null || true)
  if printf '%s\n' "$psout" | grep -Eq '127\.0\.0\.1:[0-9]+/tcp'; then
    break
  fi
  sleep 0.1
done
printf '%s\n' "$psout" | grep -q '^CALL ID[[:space:]]\+COMMAND[[:space:]]\+CREATED[[:space:]]\+STATUS[[:space:]]\+PORTS[[:space:]]\+NAMES' ||
  fail "ps header is not Docker-shaped"
# Port observation reads /proc: Linux-only (macOS shows an honest empty PORTS).
if [ "$(uname -s)" = Linux ]; then
  printf '%s\n' "$psout" | grep -Eq '127\.0\.0\.1:[0-9]+/tcp' ||
    fail "observed listening port missing from ps"
fi
grutty end port-smoke >/dev/null 2>&1 || true

grutty attach -h 2>&1 | grep -q 'usage: grutty attach <target>' || fail "attach help missing"

rmout=$(grutty -d --rm -- /bin/sh -c 'echo rm; sleep 0.1' 2>&1)
rmfull=$(printf '%s\n' "$rmout" | extract_full64)
[ "${#rmfull}" -eq 64 ] || fail "--rm full id missing"
for _ in $(seq 1 50); do
  [ ! -e "$HOME/.local/state/hold/$rmfull.json" ] && [ ! -e "$HOME/.local/state/hold/$rmfull.log" ] && break
  sleep 0.1
done
[ ! -e "$HOME/.local/state/hold/$rmfull.json" ] || fail "--rm record remained"
[ ! -e "$HOME/.local/state/hold/$rmfull.log" ] || fail "--rm log remained"

echo "PASS: grutty surface smoke"
