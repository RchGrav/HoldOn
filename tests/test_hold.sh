#!/usr/bin/env bash
set -Eeuo pipefail

HOLD_REAL_BIN="${HOLD_BIN:-./hold}"
case "$HOLD_REAL_BIN" in
  /*) ;;
  *) HOLD_REAL_BIN="$PWD/$HOLD_REAL_BIN" ;;
esac

FAILS=0
SUITE_ROOT="$(mktemp -d)"
chmod 755 "$SUITE_ROOT"
TEST_ROOT=""
HOME=""
HOLD_TEST_SYSTEM_STATE_DIR=""
ROOT_HOME=""
ACTOR_HOME=""
TEST_USER=""
TEST_UID=""
TEST_GID=""
USER_ACTOR_NEEDS_SUDO=0
ROOT_ACTOR_AVAILABLE=0
SUDO_BIN="$(command -v sudo || true)"
USER_CREATED=0
ROOT_SAFE_HOLD_DIR=""
HOLD_TEST_TIMEOUT="${HOLD_TEST_TIMEOUT:-25}"

# util-linux script(1) and BSD/macOS script(1) disagree on flags; detect once.
SCRIPT_FLAVOR=""
if command -v script >/dev/null 2>&1; then
  if script -qfec true /dev/null </dev/null >/dev/null 2>&1; then
    SCRIPT_FLAVOR=linux
  else
    SCRIPT_FLAVOR=bsd
  fi
fi

pty_run() {
  if [ "$SCRIPT_FLAVOR" = linux ]; then
    script -qfec "$1" /dev/null
  else
    script -q /dev/null /bin/sh -c "$1"
  fi
}

PASSES=0
SKIPS=0
pass() { echo "PASS: $1"; PASSES=$((PASSES + 1)); }
fail() { echo "FAIL: $1"; FAILS=$((FAILS + 1)); }
skip_note() { echo "SKIP: $1"; SKIPS=$((SKIPS + 1)); }
# Call `skip "<reason>"` from inside a test to report SKIP instead of a vacuous
# PASS. It returns sentinel 77, which run_test maps to SKIP (or to FAIL when
# HOLD_REQUIRE_ROOT_TESTS=1, so CI cannot silently no-op the root lane).
skip() { [ -n "${1:-}" ] && echo "  (skipped: $1)" >&2; return 77; }
# An optional program (vim, tmux, ...) is missing: always a plain skip, even
# under HOLD_REQUIRE_ROOT_TESTS=1, which exists to catch skipped root tests.
skip_tool() { echo "  (skipped: $1)" >&2; return 78; }
# A scratch directory the acting user owns: when the suite runs as root, hold
# (and the programs it holds) run as an unprivileged test user who cannot
# write into the root-owned $TEST_ROOT.
actor_dir() {
  local d="$TEST_ROOT/actor"
  mkdir -p "$d" || return 1
  if [ "${USER_ACTOR_NEEDS_SUDO:-0}" -eq 1 ]; then chown "$TEST_UID:$TEST_GID" "$d" || return 1; fi
  printf '%s' "$d"
}

# Refuse to run vacuously: a missing core tool (e.g. ps from procps) must be a
# hard, visible failure, never a quietly-green test.
require_tools() {
  local missing="" t
  for t in ps stat sed awk grep find mktemp head tr cut id chmod kill; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
  done
  if [ -n "$missing" ]; then
    echo "FATAL: required test tools missing:$missing" >&2
    echo "  install them (e.g. 'procps' provides ps) and re-run; the suite refuses to pass vacuously." >&2
    exit 1
  fi
  case "$HOLD_TEST_TIMEOUT" in
    ''|*[!0-9]*|0)
      echo "FATAL: HOLD_TEST_TIMEOUT must be a positive integer number of seconds" >&2
      exit 1
      ;;
  esac
}

suite_cleanup() {
  set +e
  if [ "$USER_CREATED" -eq 1 ] && [ -n "$TEST_USER" ]; then
    userdel "$TEST_USER" >/dev/null 2>&1 || true
  fi
  if [ -n "$ROOT_SAFE_HOLD_DIR" ]; then
    if [ "$(id -u)" -eq 0 ]; then
      rm -rf "$ROOT_SAFE_HOLD_DIR" >/dev/null 2>&1 || true
    elif [ -n "$SUDO_BIN" ]; then
      "$SUDO_BIN" -n rm -rf "$ROOT_SAFE_HOLD_DIR" >/dev/null 2>&1 || true
    fi
  fi
  remove_tree "$SUITE_ROOT"
}
trap suite_cleanup EXIT

remove_tree() {
  local path="$1"
  [ -n "$path" ] || return 0
  [ -e "$path" ] || return 0

  # Fast path: almost every test tree is user-owned and removable as-is. Avoid
  # recursive chmod/chown unless cleanup actually hits root-owned leftovers.
  rm -rf "$path" 2>/dev/null || true
  [ ! -e "$path" ] || [ -L "$path" ] || {
    if [ "$(id -u)" -eq 0 ]; then
      chmod -R u+rwX "$path" 2>/dev/null || true
      rm -rf "$path" || true
    elif [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] && [ -n "$SUDO_BIN" ]; then
      # Root-owned artifacts are expected only in the synthetic system store
      # and root-home fixtures. Normalize those directories instead of walking
      # the whole temp tree on every cleanup.
      while IFS= read -r root_owned_dir; do
        "$SUDO_BIN" -n chmod -R u+rwX "$root_owned_dir" 2>/dev/null || true
        "$SUDO_BIN" -n chown -R "$(id -u):$(id -g)" "$root_owned_dir" 2>/dev/null || true
      done < <(find "$path" -maxdepth 3 -type d \( -name system -o -name root-home \) -print 2>/dev/null || true)
      rm -rf "$path" 2>/dev/null || "$SUDO_BIN" -n rm -rf "$path" || true
    else
      rm -rf "$path" || true
    fi
  }
  return 0
}

setup_suite_actors() {
  if [ "$(id -u)" -eq 0 ]; then
    USER_ACTOR_NEEDS_SUDO=1
    ROOT_ACTOR_AVAILABLE=1
    TEST_USER="holdtest_$$"
    ACTOR_HOME="$SUITE_ROOT/user-home"
    mkdir -p "$ACTOR_HOME" || return 1
    if ! id "$TEST_USER" >/dev/null 2>&1; then
      useradd -M -d "$ACTOR_HOME" -s /bin/sh "$TEST_USER" || return 1
      USER_CREATED=1
    fi
    TEST_UID="$(id -u "$TEST_USER")"
    TEST_GID="$(id -g "$TEST_USER")"
    chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME" || return 1
    chmod 700 "$ACTOR_HOME" || return 1
    # The test user must be able to execute the binary under test, but the
    # checkout may sit in a home it cannot traverse (/home/runner is 0750 on
    # GitHub runners): run a world-readable copy instead.
    if ! sudo -n -u "$TEST_USER" test -x "$HOLD_REAL_BIN" 2>/dev/null &&
       ! su -s /bin/sh "$TEST_USER" -c "test -x '$HOLD_REAL_BIN'" 2>/dev/null; then
      cp "$HOLD_REAL_BIN" "$SUITE_ROOT/hold-under-test" || return 1
      chmod 755 "$SUITE_ROOT/hold-under-test" || return 1
      HOLD_REAL_BIN="$SUITE_ROOT/hold-under-test"
    fi
  else
    USER_ACTOR_NEEDS_SUDO=0
    TEST_USER="$(id -un)"
    TEST_UID="$(id -u)"
    TEST_GID="$(id -g)"
    if [ -n "$SUDO_BIN" ] && "$SUDO_BIN" -n true >/dev/null 2>&1; then
      ROOT_ACTOR_AVAILABLE=1
    else
      ROOT_ACTOR_AVAILABLE=0
    fi
  fi
  export HOLD_REAL_BIN SUDO_BIN TEST_USER TEST_UID TEST_GID USER_ACTOR_NEEDS_SUDO ROOT_ACTOR_AVAILABLE
}

as_user() {
  local env_args
  env_args=(
    "HOME=$ACTOR_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "HOLD_TEST_INVOKING_HOME=$ACTOR_HOME"
    "PATH=$PATH"
  )
  local name
  for name in \
    HOLD_BOOT_ID_PATH \
    HOLD_TEST_HOME_ROOT \
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC \
    HOLD_CALL_ID \
    HOLD_CALL_NAME \
    HOLD_CALL_PARENT \
    TERM; do
    if [ "${!name+x}" = x ]; then
      env_args+=("$name=${!name}")
    fi
  done
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    "$SUDO_BIN" -n -u "$TEST_USER" env "${env_args[@]}" "$@"
  else
    env "${env_args[@]}" "$@"
  fi
}

as_root() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 77
  local env_args
  env_args=(
    "HOME=$ROOT_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "PATH=$PATH"
  )
  local name
  for name in \
    HOLD_BOOT_ID_PATH \
    HOLD_TEST_HOME_ROOT \
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC; do
    if [ "${!name+x}" = x ]; then
      env_args+=("$name=${!name}")
    fi
  done
  if [ "$(id -u)" -eq 0 ]; then
    env "${env_args[@]}" "$@"
  else
    "$SUDO_BIN" -n env "${env_args[@]}" "$@"
  fi
}

as_sudo_from_user() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 77
  local env_args
  env_args=(
    "HOME=$ROOT_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "HOLD_TEST_INVOKING_HOME=$ACTOR_HOME"
    "SUDO_UID=$TEST_UID"
    "SUDO_GID=$TEST_GID"
    "SUDO_USER=$TEST_USER"
    "PATH=$PATH"
  )
  local name
  for name in \
    HOLD_BOOT_ID_PATH \
    HOLD_TEST_HOME_ROOT \
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC; do
    if [ "${!name+x}" = x ]; then
      env_args+=("$name=${!name}")
    fi
  done
  if [ "$(id -u)" -eq 0 ]; then
    env "${env_args[@]}" "$@"
  else
    "$SUDO_BIN" -n env "${env_args[@]}" "$@"
  fi
}

write_user_actor_wrapper() {
  cat > "$TEST_ROOT/user-hold" <<'SH'
#!/usr/bin/env bash
set -Eeuo pipefail
env_args=(
  "HOME=$HOLD_ACTOR_HOME"
  "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
  "HOLD_TEST_INVOKING_HOME=$HOLD_ACTOR_HOME"
  "PATH=$PATH"
)
for name in \
  HOLD_BOOT_ID_PATH \
  HOLD_TEST_HOME_ROOT \
  HOLD_TEST_FAIL_RECORD_WRITE \
  HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
  HOLD_FAKE_SUDO_ARGV \
  HOLD_FAKE_SUDO_RC \
  HOLD_CALL_ID \
  HOLD_CALL_NAME \
  HOLD_CALL_PARENT \
  TERM; do
  if [ "${!name+x}" = x ]; then
    env_args+=("$name=${!name}")
  fi
done
if [ "${HOLD_USER_ACTOR_NEEDS_SUDO:-0}" -eq 1 ]; then
  exec "$HOLD_ACTOR_SUDO_BIN" -n -u "$HOLD_ACTOR_USER" env "${env_args[@]}" "$HOLD_REAL_BIN" "$@"
fi
exec env "${env_args[@]}" "$HOLD_REAL_BIN" "$@"
SH
  chmod +x "$TEST_ROOT/user-hold" || return 1
  HOLD_BIN="$TEST_ROOT/user-hold"
  export HOLD_BIN
}

new_env() {
  TEST_ROOT="$(mktemp -d "$SUITE_ROOT/test.XXXXXX")" || return 1
  chmod 755 "$TEST_ROOT" || return 1
  HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system"
  HOLD_BOOT_ID_PATH="$TEST_ROOT/boot_id"
  printf 'boot-main\n' >"$HOLD_BOOT_ID_PATH" || return 1
  ROOT_HOME="$TEST_ROOT/root-home"
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR" "$ROOT_HOME" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" || return 1
  chmod 700 "$ROOT_HOME" || return 1

  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    find "$ACTOR_HOME" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME" || return 1
    chmod 700 "$ACTOR_HOME" || return 1
  else
    ACTOR_HOME="$TEST_ROOT/home"
    mkdir -p "$ACTOR_HOME" || return 1
    chmod 700 "$ACTOR_HOME" || return 1
  fi

  HOME="$ACTOR_HOME"
  export HOME TEST_ROOT HOLD_TEST_SYSTEM_STATE_DIR HOLD_BOOT_ID_PATH ROOT_HOME ACTOR_HOME
  export HOLD_ACTOR_HOME="$ACTOR_HOME"
  export HOLD_ACTOR_USER="$TEST_USER"
  export HOLD_ACTOR_SUDO_BIN="$SUDO_BIN"
  export HOLD_USER_ACTOR_NEEDS_SUDO="$USER_ACTOR_NEEDS_SUDO"
  write_user_actor_wrapper
}

cleanup_env() {
  local store ids id sys_store
  store="$HOME/.local/state/hold"
  if [ -d "$store" ]; then
    ids=$(find "$store" -maxdepth 1 -type f -name '*.json' -exec basename {} .json \; 2>/dev/null || true)
    for id in $ids; do
      "$HOLD_BIN" kill "$id" >/dev/null 2>&1 || true
    done
  fi
  sys_store="$HOLD_TEST_SYSTEM_STATE_DIR/runs"
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] && [ -d "$sys_store" ]; then
    ids=$(find "$sys_store" -maxdepth 1 -type f -name '*.json' -exec basename {} .json \; 2>/dev/null || true)
    for id in $ids; do
      as_root "$HOLD_REAL_BIN" kill "system:$id" >/dev/null 2>&1 || true
    done
  fi
  remove_tree "$TEST_ROOT"
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ] && [ -n "$ACTOR_HOME" ]; then
    find "$ACTOR_HOME" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
  fi
}

ensure_user_fixture_store() {
  mkdir -p "$HOME/.local/state/hold" || return 1
  chmod 700 "$HOME" "$HOME/.local" "$HOME/.local/state" "$HOME/.local/state/hold" 2>/dev/null || true
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    chown -R "$TEST_UID:$TEST_GID" "$HOME/.local" || return 1
  fi
}

file_mode() {
  stat -c '%a' "$1" 2>/dev/null || stat -f '%Lp' "$1"
}

resolve_path() {
  local path="$1" dir base
  if command -v realpath >/dev/null 2>&1; then
    realpath "$path"
    return
  fi
  dir="$(dirname "$path")"
  base="$(basename "$path")"
  (cd "$dir" && printf '%s/%s\n' "$(pwd -P)" "$base")
}

root_file_exists() {
  as_root test -f "$1"
}

root_path_absent() {
  as_root sh -c '[ ! -e "$1" ]' sh "$1"
}

root_file_mode() {
  as_root stat -c '%a' "$1" 2>/dev/null || as_root stat -f '%Lp' "$1"
}

root_file_owner() {
  as_root stat -c '%u:%g' "$1" 2>/dev/null || as_root stat -f '%u:%g' "$1"
}

sha256_stdin() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 | awk '{print $1}'
  else
    printf '%s\n' 'missing sha256sum or shasum for SHA-256 test helper' >&2
    return 127
  fi
}


root_grep() {
  local pattern="$1" path="$2"
  as_root grep -F -q -- "$pattern" "$path"
}

terminate_pid_tree() {
  local pid="$1" signal_name="${2:-TERM}" child
  [ -n "$pid" ] || return 0
  for child in $(ps -eo pid=,ppid= 2>/dev/null | awk -v p="$pid" '$2 == p { print $1 }'); do
    terminate_pid_tree "$child" "$signal_name"
  done
  kill "-$signal_name" "$pid" 2>/dev/null || true
}

print_timeout_diagnostics() {
  local desc="$1" fn="$2"
  {
    echo "TIMEOUT: $fn ($desc) exceeded ${HOLD_TEST_TIMEOUT}s"
    echo "TEST_ROOT: ${TEST_ROOT:-}"
    echo "--- relevant ps output ---"
    ps -eo pid=,ppid=,pgid=,stat=,etime=,args= 2>/dev/null |
      awk -v root="${TEST_ROOT:-}" -v suite="$SUITE_ROOT" '
        NR == 1 || $0 ~ /hold/ || (root != "" && index($0, root)) || index($0, suite) || $0 ~ /sleep|bash|sh/ {
          print
          shown++
          if (shown >= 120) exit
        }
      ' || true
    echo "--- bounded find listing for TEST_ROOT ---"
    if [ -n "${TEST_ROOT:-}" ] && [ -d "$TEST_ROOT" ]; then
      find "$TEST_ROOT" -maxdepth 4 -print 2>/dev/null | sort | head -n 200 || true
    else
      echo "(TEST_ROOT unavailable)"
    fi
  } >&2
}

run_test() {
  local desc="$1" fn="$2" rc="" pid deadline
  # HOLD_TEST_ONLY=<regex>: run only tests whose function or description match.
  if [ -n "${HOLD_TEST_ONLY:-}" ] && ! [[ "$fn $desc" =~ $HOLD_TEST_ONLY ]]; then return 0; fi
  echo "RUN: $desc"
  new_env || { fail "$desc"; return; }
  export HOME TEST_ROOT HOLD_BIN HOLD_REAL_BIN HOLD_TEST_SYSTEM_STATE_DIR ROOT_HOME ACTOR_HOME
  export TEST_USER TEST_UID TEST_GID USER_ACTOR_NEEDS_SUDO ROOT_ACTOR_AVAILABLE SUDO_BIN
  export HOLD_ACTOR_HOME HOLD_ACTOR_USER HOLD_ACTOR_SUDO_BIN HOLD_USER_ACTOR_NEEDS_SUDO
  set +e
  ( set -Eeuo pipefail; "$fn" ) &
  pid=$!
  deadline=$((SECONDS + HOLD_TEST_TIMEOUT))
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
      print_timeout_diagnostics "$desc" "$fn"
      terminate_pid_tree "$pid" TERM
      sleep 0.5
      terminate_pid_tree "$pid" KILL
      wait "$pid" 2>/dev/null || true
      rc=124
      break
    fi
    sleep 0.1
  done
  if [ -z "$rc" ]; then
    wait "$pid"
    rc=$?
  fi
  set -e
  if [ "$rc" -eq 0 ]; then
    pass "$desc"
  elif [ "$rc" -eq 78 ]; then
    skip_note "$desc"
  elif [ "$rc" -eq 77 ]; then
    if [ "${HOLD_REQUIRE_ROOT_TESTS:-0}" -eq 1 ]; then
      fail "$desc (skipped, but HOLD_REQUIRE_ROOT_TESTS=1)"
    else
      skip_note "$desc"
    fi
  else
    fail "$desc"
  fi
  cleanup_env
}

setup_suite_actors
require_tools

extract_id() {
  sed -n -e '/^[0-9a-f]\{12\}$/p' -e '/^[0-9a-f]\{64\}$/p' -e 's/^hold: id=\([0-9a-f][0-9a-f]*\).*/\1/p' | head -n1 | cut -c1-12
}

find_prefixed_artifact() {
  local store="$1" id="$2" suffix="$3" path count
  [ -n "$store" ] && [ -n "$id" ] && [ -d "$store" ] || return 1
  if [ -e "$store/$id$suffix" ]; then
    printf '%s\n' "$store/$id$suffix"
    return 0
  fi
  count=$(find "$store" -maxdepth 1 -type f -name "$id*$suffix" 2>/dev/null | wc -l)
  [ "$count" -eq 1 ] || return 1
  path=$(find "$store" -maxdepth 1 -type f -name "$id*$suffix" -print -quit 2>/dev/null)
  [ -n "$path" ] || return 1
  printf '%s\n' "$path"
}

record_path() {
  local id="$1" store="${2:-$HOME/.local/state/hold}"
  find_prefixed_artifact "$store" "$id" ".json"
}

log_path() {
  local id="$1" store="${2:-$HOME/.local/state/hold}"
  find_prefixed_artifact "$store" "$id" ".log"
}

record_exists() {
  record_path "$1" "${2:-$HOME/.local/state/hold}" >/dev/null 2>&1
}

log_exists() {
  log_path "$1" "${2:-$HOME/.local/state/hold}" >/dev/null 2>&1
}

root_find_prefixed_artifact() {
  local store="$1" id="$2" suffix="$3"
  as_root sh -c '
store=$1
id=$2
suffix=$3
[ -n "$store" ] && [ -n "$id" ] && [ -d "$store" ] || exit 1
if [ -e "$store/$id$suffix" ]; then
  printf "%s\n" "$store/$id$suffix"
  exit 0
fi
count=$(find "$store" -maxdepth 1 -type f -name "$id*$suffix" 2>/dev/null | wc -l)
[ "$count" -eq 1 ] || exit 1
find "$store" -maxdepth 1 -type f -name "$id*$suffix" -print -quit 2>/dev/null
' sh "$store" "$id" "$suffix"
}

root_record_path() {
  root_find_prefixed_artifact "$2" "$1" ".json"
}

root_log_path() {
  root_find_prefixed_artifact "$2" "$1" ".log"
}

root_record_exists() {
  root_record_path "$1" "$2" >/dev/null 2>&1
}

root_log_exists() {
  root_log_path "$1" "$2" >/dev/null 2>&1
}

record_pgid() {
  local id="$1" store="${2:-$HOME/.local/state/hold}" record
  record=$(record_path "$id" "$store") || return 1
  sed -n 's/.*"pgid":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$record" | head -n1
}

record_sid() {
  local id="$1" store="${2:-$HOME/.local/state/hold}" record
  record=$(record_path "$id" "$store") || return 1
  sed -n 's/.*"sid":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$record" | head -n1
}

record_name() {
  local id="$1" store="${2:-$HOME/.local/state/hold}" record
  record=$(record_path "$id" "$store") || return 1
  sed -n 's/.*"name": "\([^"]*\)".*/\1/p' "$record" | head -n1
}

# Poll until a call's record reports the exited state, so purge/redial (which
# require an ended call) do not race the supervisor's finish write.
record_ended_soon() {
  local id="$1" store="${2:-$HOME/.local/state/hold}" record tries
  for tries in $(seq 1 60); do
    record=$(record_path "$id" "$store") || return 1
    grep -q '"state": "exited"' "$record" && return 0
    sleep 0.05
  done
  return 1
}


pid_dead_enough() {
  local p="$1" st
  if ! kill -0 "$p" 2>/dev/null; then
    return 0
  fi
  st=$(ps -o stat= -p "$p" 2>/dev/null | tr -d ' ' | cut -c1 || true)
  [ "$st" = "Z" ] || ! kill -0 "$p" 2>/dev/null
}

pgid_terminated() {
  local g="$1" tries stats
  for tries in $(seq 1 40); do
    if ! kill -0 "-$g" 2>/dev/null; then
      return 0
    fi
    stats=$(ps -o stat= -g "$g" 2>/dev/null | tr -d " " || true)
    if [ -n "$stats" ] && ! printf "%s\n" "$stats" | grep -qv "^Z"; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

path_absent_soon() {
  local path="$1" tries
  for tries in $(seq 1 40); do
    [ ! -e "$path" ] && return 0
    sleep 0.05
  done
  return 1
}

test_lifecycle() {
  local out id lines
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  # Detached bare form prints exactly the 64-hex call id, nothing else.
  printf '%s\n' "$out" | grep -Eqx '[0-9a-f]{64}' || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf '%s\n' "$id" | grep -Eq '^[0-9a-f]{12}$' || return 1
  "$HOLD_BIN" list | grep -Eq "^$id[[:space:]].*Up "
  "$HOLD_BIN" end "$id" >/dev/null
  record_ended_soon "$id" || return 1
  "$HOLD_BIN" list -a | grep -Eq "^$id[[:space:]].*Exited"
  "$HOLD_BIN" purge >/dev/null
  lines=$("$HOLD_BIN" list -a | wc -l)
  [ "$lines" -eq 1 ]
}


test_kill_subcommand() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  pgid=$(record_pgid "$id")
  [ -n "$pgid" ] || return 1
  "$HOLD_BIN" kill "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_group_kill_children() {
  local out id pgid children
  out=$("$HOLD_BIN" -d bash -c 'sleep 600 & sleep 601 & wait' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  sleep 0.2
  children=$(ps -eo pid=,pgid=,args= | awk -v g="$pgid" '$2==g && $1!=g && $3 ~ /^sleep$/ {print $1}')
  [ -n "$children" ] || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  sleep 0.2
  for p in $children; do
    pid_dead_enough "$p" || return 1
  done
  return 0
}

test_exec_failure_no_record() {
  local rc count
  set +e
  "$HOLD_BIN" nonexistent_binary_xyz >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  if [ -d "$HOME/.local/state/hold" ]; then
    count=$(find "$HOME/.local/state/hold" -maxdepth 1 -type f -name '*.json' | wc -l)
  else
    count=0
  fi
  [ "$count" -eq 0 ] || return 1
  count=$(find "$HOME/.local/state/hold" -maxdepth 1 -type f 2>/dev/null | wc -l)
  [ "$count" -eq 0 ]
}

test_fast_exit_record_exited() {
  local out id record
  out=$("$HOLD_BIN" -d /bin/sh -c 'exit 7' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  record=$(record_path "$id") || return 1
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 7' "$record" && break
    sleep 0.1
  done
  grep -q '"state": "exited"' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"exit_code": 7' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"ended_at": "' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps -a | grep -Eq "^$id[[:space:]].*Exited \\(7\\)"
}

test_exec_replacement_remains_controllable() {
  local out id pgid
  out=$("$HOLD_BIN" -d bash -c 'exec sleep 300' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  "$HOLD_BIN" list | grep -Eq "^$id[[:space:]].*Up " || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_corrupt_record_handling() {
  ensure_user_fixture_store || return 1
  printf 'garbage\n' > "$HOME/.local/state/hold/badbad00cafe0000000000000000000000000000000000000000000000000000.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^badbad00cafe' "$TEST_ROOT/list.out"
  ! grep -Eq '^0[[:space:]]' "$TEST_ROOT/list.out"
  grep -q 'warning: skipping corrupt record badbad00cafe0000000000000000000000000000000000000000000000000000.json' "$TEST_ROOT/list.err"
  "$HOLD_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/hold/badbad00cafe0000000000000000000000000000000000000000000000000000.json" ]
}

test_deep_json_record_rejected_not_crashed() {
  ensure_user_fixture_store || return 1
  local json i
  json='{"junk":'
  for i in $(seq 1 80); do json="${json}["; done
  json="${json}0"
  for i in $(seq 1 80); do json="${json}]"; done
  json="${json},\"version\":1,\"id\":\"abc12445cafe\",\"pid\":12345,\"pgid\":12345,\"sid\":12345,\"start_unix_ns\":0,\"argv\":[\"x\"],\"cmdline_display\":\"x\",\"uid\":0,\"gid\":0,\"proc_starttime_ticks\":0,\"exe_dev\":0,\"exe_ino\":0}"
  printf '%s\n' "$json" > "$HOME/.local/state/hold/abc12445cafe0000000000000000000000000000000000000000000000000000.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12445cafe' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12445cafe0000000000000000000000000000000000000000000000000000.json' "$TEST_ROOT/list.err"
}

test_symlinked_record_rejected() {
  ensure_user_fixture_store || return 1
  cat > "$TEST_ROOT/record-target.json" <<'JSON'
{"version":1,"id":"abc12545cafe0000000000000000000000000000000000000000000000000000","pid":12345,"pgid":12345,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  ln -s "$TEST_ROOT/record-target.json" "$HOME/.local/state/hold/abc12545cafe0000000000000000000000000000000000000000000000000000.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12545cafe' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12545cafe0000000000000000000000000000000000000000000000000000.json' "$TEST_ROOT/list.err"
}

test_symlinked_log_rejected() {
  local out id log rc
  out=$("$HOLD_BIN" -d /bin/echo original-log-line 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  log=$(log_path "$id") || return 1
  sleep 0.2
  [ -f "$log" ] || return 1
  printf 'symlink-secret\n' > "$TEST_ROOT/log-target" || return 1
  rm -f "$log" || return 1
  ln -s "$TEST_ROOT/log-target" "$log" || return 1
  set +e
  "$HOLD_BIN" logs "$id" -p >"$TEST_ROOT/dump.out" 2>"$TEST_ROOT/dump.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || return 1
  ! grep -q 'symlink-secret' "$TEST_ROOT/dump.out" || return 1
  grep -q 'failed to open log' "$TEST_ROOT/dump.err"
}

test_invalid_pgid_record() {
  ensure_user_fixture_store || return 1
  cat > "$HOME/.local/state/hold/abc12345cafe0000000000000000000000000000000000000000000000000000.json" <<'JSON'
{"version":1,"id":"abc12345cafe0000000000000000000000000000000000000000000000000000","pid":12345,"pgid":0,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12345cafe' "$TEST_ROOT/list.out"
}

test_orphan_log_cleanup() {
  ensure_user_fixture_store || return 1
  : > "$HOME/.local/state/hold/a1b2c3d4e5f60000000000000000000000000000000000000000000000000000.log" || return 1
  : > "$HOME/.local/state/hold/deadbeefcafe0000000000000000000000000000000000000000000000000000.log" || return 1
  "$HOLD_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/hold/a1b2c3d4e5f60000000000000000000000000000000000000000000000000000.log" ] && [ ! -e "$HOME/.local/state/hold/deadbeefcafe0000000000000000000000000000000000000000000000000000.log" ]
}

test_id_sanitization() {
  local rc
  for bad in '../../etc/passwd' 'AABBCC' 'hello!' ''; do
    set +e
    "$HOLD_BIN" stop "$bad" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 5 ] || return 1
  done
}

test_print_signal_output() {
  local out id pgid got
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  got=$("$HOLD_BIN" stop --print "$id") || return 1
  [ "$got" = "kill -TERM -- -$pgid" ]
  got=$("$HOLD_BIN" kill --print "$id") || return 1
  [ "$got" = "kill -KILL -- -$pgid" ]
}

test_signal_refuses_tampered_live_group_identity() {
  local out id rec real_pgid real_sid decoy_out decoy_id decoy_pgid decoy_sid rc
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  real_pgid=$(record_pgid "$id")
  real_sid=$(record_sid "$id")
  [ -n "$id" ] && [ -n "$real_pgid" ] && [ -n "$real_sid" ] || return 1

  decoy_out=$("$HOLD_BIN" -d sleep 300 2>&1) || { "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  decoy_id=$(printf '%s\n' "$decoy_out" | extract_id)
  decoy_pgid=$(record_pgid "$decoy_id")
  decoy_sid=$(record_sid "$decoy_id")
  [ -n "$decoy_id" ] && [ -n "$decoy_pgid" ] && [ -n "$decoy_sid" ] || {
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    [ -n "$decoy_id" ] && "$HOLD_BIN" stop "$decoy_id" >/dev/null 2>&1 || true
    return 1
  }
  [ "$decoy_pgid" != "$real_pgid" ] && [ "$decoy_sid" != "$real_sid" ] || {
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    "$HOLD_BIN" stop "$decoy_id" >/dev/null 2>&1 || true
    return 1
  }

  rec=$(record_path "$id") || return 1
  cp "$rec" "$rec.bak" || return 1
  sed -i.tmp \
    -e "s/\"pgid\":[[:space:]]*[0-9][0-9]*/\"pgid\":$decoy_pgid/" \
    -e "s/\"sid\":[[:space:]]*[0-9][0-9]*/\"sid\":$decoy_sid/" \
    "$rec" || return 1
  rm -f "$rec.tmp"
  set +e
  "$HOLD_BIN" stop --print "$id" >/dev/null 2>"$TEST_ROOT/tampered-live-print.err"
  rc=$?
  set -e
  mv "$rec.bak" "$rec" 2>/dev/null || true
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  "$HOLD_BIN" stop "$decoy_id" >/dev/null 2>&1 || true
  [ "$rc" -eq 2 ] || { echo "--print on tampered live pgid/sid: rc=$rc (want 2 refused)" >&2; return 1; }
  grep -Eq 'process group/session differs|cannot be signaled' "$TEST_ROOT/tampered-live-print.err" || {
    cat "$TEST_ROOT/tampered-live-print.err" >&2
    return 1
  }
}

test_stop_multiple_ids() {
  local out1 out2 id1 id2 pgid1 pgid2 rc
  out1=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  out2=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid1=$(record_pgid "$id1")
  pgid2=$(record_pgid "$id2")
  [ -n "$id1" ] && [ -n "$id2" ] && [ -n "$pgid1" ] && [ -n "$pgid2" ] || return 1
  set +e
  "$HOLD_BIN" stop "$id1" "$id2" >/dev/null
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || return 1
  pgid_terminated "$pgid1" || return 1
  pgid_terminated "$pgid2"
}

test_argument_edges() {
  local rc out
  set +e
  printf '' | "$HOLD_BIN" >"$TEST_ROOT/bare-hold.out" 2>"$TEST_ROOT/bare-hold.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/bare-hold.out" "$TEST_ROOT/bare-hold.err" >&2; return 1; }
  grep -q 'PLACE A CALL' "$TEST_ROOT/bare-hold.out" || { cat "$TEST_ROOT/bare-hold.out" >&2; return 1; }
  set +e
  "$HOLD_BIN" purge -all >/dev/null 2>"$TEST_ROOT/prune-flag.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  grep -q "unknown flag '-all'" "$TEST_ROOT/prune-flag.err" || return 1
  set +e
  "$HOLD_BIN" end >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || return 1
  "$HOLD_BIN" --help >/dev/null || return 1
  "$HOLD_BIN" help >/dev/null || return 1
  "$HOLD_BIN" help --help >/dev/null || return 1
  "$HOLD_BIN" help targets | grep -q 'call id, leading id prefix, or call name' || return 1
  "$HOLD_BIN" help scripting | grep -q 'Exit codes:' || return 1
  "$HOLD_BIN" end -h | grep -q 'usage: hold end' || return 1
  out=$("$HOLD_BIN" --version) || return 1
  printf '%s\n' "$out" | grep -Eq '^(dev(-[0-9a-f]{7,40}(-dirty)?)?|[0-9a-f]{7,40}|v?[0-9]+\.[0-9]+\.[0-9]+.*)$'
  set +e
  "$HOLD_BIN" -l >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  set +e
  "$HOLD_BIN" --list >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  "$HOLD_BIN" -- sleep 1 >/dev/null
}

test_special_chars_args() {
  local out id json
  out=$("$HOLD_BIN" -d echo "hello world" "it's" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(record_path "$id") || return 1
  [ -f "$json" ] || return 1
  grep -Fq '"hello world"' "$json"
  grep -Fq '"it'"'"'s"' "$json"
}

test_log_capture() {
  local out id log
  out=$("$HOLD_BIN" -d bash -c 'echo out; echo err >&2; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  log=$(log_path "$id") || return 1
  sleep 0.4
  [ -f "$log" ] || return 1
  [ ! -e "$log.idx" ] || { echo "a log index sidecar was written" >&2; return 1; }
  ! grep -q '"log"' "$log" || { cat "$log" >&2; return 1; }
  grep -q '^out$' "$log" && grep -q '^err$' "$log" || return 1
  [ "$("$HOLD_BIN" logs "$id" | wc -l | tr -d ' ')" -eq 2 ] || return 1
  [ "$("$HOLD_BIN" logs "$id" -n 1 | wc -l | tr -d ' ')" -eq 1 ] || return 1
  [ -z "$("$HOLD_BIN" logs "$id" -n 0)" ] || return 1
  set +e; "$HOLD_BIN" logs "$id" --filter out >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "removed --filter flag was accepted" >&2; return 1; }
}

# logs -n N prints exactly the newest N lines; -f follows to the end and
# drains the last output written before exit.
test_logs_tail_and_follow() {
  "$HOLD_BIN" -d --name lines -- /bin/sh -c 'i=1; while [ $i -le 50 ]; do echo "line $i"; i=$((i+1)); done; printf "no-newline-tail"' >/dev/null || return 1
  "$HOLD_BIN" wait lines --timeout 5000 >/dev/null || return 1
  [ "$("$HOLD_BIN" logs lines -n 3)" = "$(printf 'line 49\nline 50\nno-newline-tail')" ] || { "$HOLD_BIN" logs lines -n 3 | od -c | tail -3 >&2; return 1; }
  [ "$("$HOLD_BIN" logs lines --tail=1)" = "no-newline-tail" ] || return 1
  [ "$("$HOLD_BIN" logs lines -n 1000 | wc -l | tr -d ' ')" -eq 50 ] || return 1
  "$HOLD_BIN" -d --name follower -- /bin/sh -c 'echo first; sleep 1; echo last' >/dev/null || return 1
  out=$("$HOLD_BIN" logs follower -f) || return 1
  [ "$out" = "$(printf 'first\nlast')" ] || { printf '%s\n' "$out" >&2; return 1; }
}

test_start_follow_short_form() {
  local out
  # -f keeps the foreground call streaming; foreground prints no id of its own.
  out=$("$HOLD_BIN" -f bash -c 'echo follow-short' 2>&1) || return 1
  printf '%s\n' "$out" | grep -q 'follow-short'
}


test_tail_verb_existing_id() {
  local out id tailed
  out=$("$HOLD_BIN" -d bash -c 'sleep 0.3; echo from_tail_id; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  tailed=$("$HOLD_BIN" tail "$id" 2>&1) || return 1
  printf '%s\n' "$tailed" | grep -q 'from_tail_id'
}

test_persistent_stale_records() {
  local out id store bootfile oldboot list_out stale_id stale_log
  bootfile="$TEST_ROOT/fake_boot_id"
  printf 'boot-a\n' >"$bootfile" || return 1
  out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" -d bash -c 'echo stale-line; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  store="$HOME/.local/state/hold"
  stale_log=$(log_path "$id" "$store") || return 1
  [ -f "$stale_log" ] || return 1
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list >/dev/null || return 1
  printf 'boot-b\n' >"$bootfile" || return 1
  list_out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list -a) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Stale"
  set +e
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop "$id" >/dev/null 2>"$TEST_ROOT/stop.err"
  [ "$?" -eq 2 ] || return 1
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" kill "$id" >/dev/null 2>"$TEST_ROOT/kill.err"
  [ "$?" -eq 2 ] || return 1
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop --print "$id" >/dev/null 2>"$TEST_ROOT/print.err"
  [ "$?" -eq 2 ] || return 1
  set -e
  grep -q 'stale' "$TEST_ROOT/stop.err"
  grep -q 'stale' "$TEST_ROOT/kill.err"
  grep -q 'stale' "$TEST_ROOT/print.err"
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" tail "$id" >"$TEST_ROOT/tail.out" 2>&1 || return 1
  grep -q 'stale-line' "$TEST_ROOT/tail.out"
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" logs "$id" -p >"$TEST_ROOT/dump.out" 2>&1 || return 1
  grep -q 'stale-line' "$TEST_ROOT/dump.out"
  record_exists "$id" "$store" && log_exists "$id" "$store"
}


test_boot_unavailable_does_not_force_stale() {
  local out id bootfile list_out rc pgid got
  bootfile="$TEST_ROOT/fake_boot_id"
  printf 'boot-a\n' >"$bootfile" || return 1
  out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" -d sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  pgid=$(record_pgid "$id")
  [ -n "$pgid" ] || return 1
  rm -f "$bootfile"
  list_out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Up " || return 1
  ! printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Stale" || return 1
  set +e
  got=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop --print "$id" 2>"$TEST_ROOT/missing-boot-print.err")
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "stop --print with missing boot source: rc=$rc (want 0)" >&2; cat "$TEST_ROOT/missing-boot-print.err" >&2; return 1; }
  [ "$got" = "kill -TERM -- -$pgid" ] || { printf 'got: %s\nwant: kill -TERM -- -%s\n' "$got" "$pgid" >&2; return 1; }
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop "$id" >/dev/null
}

test_leader_zombie_group_still_running() {
  local id list_out
  id=$("$HOLD_BIN" -d bash -c 'sleep 60 & exit 0' 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  sleep 0.3
  list_out=$("$HOLD_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Up " || return 1
  "$HOLD_BIN" end "$id" >/dev/null || return 1
}

test_tail_finished_log_prints_existing_output() {
  local out id tailed
  out=$("$HOLD_BIN" -d bash -c 'echo finished-tail-line' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  tailed=$("$HOLD_BIN" tail "$id" 2>&1) || return 1
  printf '%s\n' "$tailed" | grep -q 'finished-tail-line'
}

test_console_does_not_require_external_attach_tool() {
  local fake_path out id store record sock
  fake_path="$TEST_ROOT/no-tools"
  mkdir -p "$fake_path" || return 1
  out=$(as_user /usr/bin/env PATH="$fake_path" "$HOLD_REAL_BIN" --console /bin/sh -c 'read line; echo "native:$line"' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  printf 'ping\n' | as_user /usr/bin/env PATH="$fake_path" "$HOLD_REAL_BIN" console "$id" >"$TEST_ROOT/console-native.out" 2>"$TEST_ROOT/console-native.err" || {
    cat "$TEST_ROOT/console-native.out" "$TEST_ROOT/console-native.err" >&2
    return 1
  }
  grep -q 'native:ping' "$TEST_ROOT/console-native.out" || { cat "$TEST_ROOT/console-native.out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_console_reports_non_console_run() {
  local out id rc
  out=$("$HOLD_BIN" -d sleep 30 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$HOLD_BIN" console "$id" >"$TEST_ROOT/console-none.out" 2>"$TEST_ROOT/console-none.err"
  rc=$?
  [ "$rc" -eq 0 ] || return 1
  grep -q "has no console" "$TEST_ROOT/console-none.err" || { cat "$TEST_ROOT/console-none.err" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_console_round_trip_and_log_tee() {
  local out id store record sock
  out=$("$HOLD_BIN" --console /bin/sh -c 'read line; echo "got:$line"' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  grep -q '"console_sock": "' "$record" || { cat "$record" >&2; return 1; }
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" list >"$TEST_ROOT/console-list.out" || return 1
  grep -Eq "^$id[[:space:]].*Up " "$TEST_ROOT/console-list.out" || {
    cat "$TEST_ROOT/console-list.out" >&2
    return 1
  }
  printf 'ping\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/console.out" 2>"$TEST_ROOT/console.err" || {
    cat "$TEST_ROOT/console.out" "$TEST_ROOT/console.err" >&2
    return 1
  }
  grep -q 'got:ping' "$TEST_ROOT/console.out" || { cat "$TEST_ROOT/console.out" >&2; return 1; }
  sleep 0.2
  logf=$(log_path "$id" "$store") || return 1
  grep -q 'got:ping' "$logf" || { cat "$logf" >&2; return 1; }
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_console_exit_code_is_recorded() {
  local out id store record rc
  out=$("$HOLD_BIN" --console /bin/sh -c 'read line; echo "console-exit:$line"; exit 7' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1

  set +e
  printf 'done\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/console-exit.out" 2>"$TEST_ROOT/console-exit.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || [ "$rc" -eq 7 ] || {
    echo "console attach rc=$rc (want 0 or target exit 7)" >&2
    cat "$TEST_ROOT/console-exit.out" "$TEST_ROOT/console-exit.err" >&2
    return 1
  }
  grep -q 'console-exit:done' "$TEST_ROOT/console-exit.out" || { cat "$TEST_ROOT/console-exit.out" >&2; return 1; }

  for _ in $(seq 1 50); do
    grep -q '"exit_code": 7' "$record" && break
    sleep 0.1
  done
  grep -q '"state": "exited"' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"exit_code": 7' "$record" || { cat "$record" >&2; return 1; }
  ! grep -q '"ExitCode"' "$record" || { echo "legacy Docker key in native record" >&2; cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps -a | grep -Eq "^$id[[:space:]].*Exited \(7\)" || return 1
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
}

test_end_console_call_signals_target() {
  # 'hold end' on a console call must end the HELD group, not the broker. The
  # target ignores SIGHUP so an incidental PTY hangup cannot masquerade as the
  # group signal: under the pre-anchoring bug the record named the broker, the
  # group survived, and the record fabricated Exited (0). No red test caught this
  # because the best-effort 'stop' cleanups elsewhere (2015-2037, 2068) silently
  # leaked the target; this test is the pin.
  local out id store record pgid sock got ok _
  out=$("$HOLD_BIN" --console /bin/sh -c 'trap "" HUP; sleep 45' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  pgid=$(record_pgid "$id" "$store") || return 1
  [ -n "$pgid" ] || return 1
  # Target-anchoring: pid==pgid==sid names the held sh/sleep, not a hold broker.
  # Under the bug the recorded group is the broker's and holds no sleep.
  ps -o args= -g "$pgid" >"$TEST_ROOT/end-signal-group.txt" 2>/dev/null || true
  grep -q 'sleep 45' "$TEST_ROOT/end-signal-group.txt" || {
    echo "record pgid $pgid is not the held group:" >&2
    cat "$TEST_ROOT/end-signal-group.txt" >&2
    return 1
  }
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  # The honest --print surface names the real group.
  got=$("$HOLD_BIN" stop --print "$id") || return 1
  [ "$got" = "kill -TERM -- -$pgid" ] || {
    echo "stop --print: [$got] want [kill -TERM -- -$pgid]" >&2
    return 1
  }
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  pgid_terminated "$pgid" || { echo "held group $pgid survived end" >&2; return 1; }
  record_ended_soon "$id" "$store" || { cat "$record" >&2; return 1; }
  # Poll the record; never grep list before this (the zombie window renders
  # Exited (0) transiently until the broker's mark lands with 143).
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 143' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 143:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 15' "$record" || { cat "$record" >&2; return 1; }
  path_absent_soon "$sock" || { ls -la "$(dirname "$sock")" >&2 || true; return 1; }
  "$HOLD_BIN" ps -a | grep -Eq "^$id[[:space:]].*Exited \(143\)" || { "$HOLD_BIN" ps -a >&2; return 1; }
}

test_end_console_call_escalates_to_kill() {
  # SPEC 'end: TERM, then KILL' holds for consoles too. The leader traps both TERM
  # and HUP and keeps a child alive across TERM, so only the KILL escalation can
  # bring the group down; the record must reach exit_code 137 (128+SIGKILL).
  local out id store record pgid ok _
  out=$("$HOLD_BIN" --console /bin/sh -c 'trap "" TERM HUP; while :; do sleep 30; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  pgid=$(record_pgid "$id" "$store") || return 1
  [ -n "$pgid" ] || return 1
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  pgid_terminated "$pgid" || { echo "held group $pgid survived end escalation" >&2; return 1; }
  record_ended_soon "$id" "$store" || { cat "$record" >&2; return 1; }
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 137' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 137:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 9' "$record" || { cat "$record" >&2; return 1; }
}

test_console_broker_term_forwards_to_target() {
  # A raw SIGTERM to the broker pid itself (not routed through 'hold end') must
  # forward to the held group and clean up the socket. The record no longer names
  # the broker, so without the forwarder a bare kill would orphan the target.
  local out id store record pgid sock broker ok _
  out=$("$HOLD_BIN" --console /bin/sh -c 'trap "" HUP; sleep 45' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  pgid=$(record_pgid "$id" "$store") || return 1
  [ -n "$pgid" ] || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  # The broker is the parent of the target group leader (it forked the target).
  broker=$(ps -o ppid= -p "$pgid" 2>/dev/null | tr -d ' ')
  [ -n "$broker" ] && [ "$broker" -gt 1 ] || { echo "could not find broker for pgid $pgid" >&2; return 1; }
  kill -TERM "$broker" || return 1
  pgid_terminated "$pgid" || { echo "held group $pgid survived broker TERM" >&2; return 1; }
  record_ended_soon "$id" "$store" || { cat "$record" >&2; return 1; }
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 143' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 143 after broker TERM:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 15' "$record" || { cat "$record" >&2; return 1; }
  path_absent_soon "$sock" || { ls -la "$(dirname "$sock")" >&2 || true; return 1; }
}

test_console_socket_lives_in_store_dir() {
  # The console socket must live inside the store's console directory, never in
  # /tmp, even though the harness's store path is longer than the AF_UNIX
  # sun_path limit (~104). This exercises the relative-bind path.
  local out id store record sock
  out=$("$HOLD_BIN" --console /bin/sh -c 'while read line; do [ "$line" = noop ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  case "$sock" in
    # macOS TMPDIR round-trips through the /private symlink; accept both spellings.
    "$store/console/$id"*.sock|"/private$store/console/$id"*.sock) ;;
    *)
    echo "console socket not in store console dir (got: $sock)" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
      ;;
  esac
  if [ ! -S "$sock" ]; then
    echo "no socket present at $sock" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  fi
  # The socket must be owner-only (0600): it is an exec channel into the run.
  sock_mode=$(file_mode "$sock") || { "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  if [ "$sock_mode" != 600 ]; then
    echo "console socket mode=$sock_mode (want 600)" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  fi
  # The relative path must also be connectable when the store path is long.
  printf 'noop\n' | "$HOLD_BIN" console "$id" >/dev/null 2>"$TEST_ROOT/console-store.err" || {
    cat "$TEST_ROOT/console-store.err" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  return 0
}

test_console_target_runs_in_caller_cwd() {
  # Relative-bind temporarily chdir()s into the console dir to bind the socket;
  # it must restore the cwd so the launched process still runs in the caller's
  # working directory, not the console directory.
  local out id store logf workdir i ok
  workdir="$TEST_ROOT/console-cwd"
  mkdir -p "$workdir" || return 1
  : > "$workdir/sentinel-file" || return 1
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    chown -R "$TEST_UID:$TEST_GID" "$workdir" || return 1
  fi

  out=$( cd "$workdir" && "$HOLD_BIN" --console /bin/sh -c \
    'if [ -e sentinel-file ]; then echo CWD_OK; else echo CWD_BAD; fi; sleep 30' 2>&1 ) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  logf=$(log_path "$id" "$store") || return 1
  ok=0
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if grep -q CWD_OK "$logf" 2>/dev/null; then ok=1; break; fi
    if grep -q CWD_BAD "$logf" 2>/dev/null; then ok=0; break; fi
    sleep 0.2
  done
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  if [ "$ok" -ne 1 ]; then
    echo "target did not run in caller cwd; log:" >&2
    cat "$logf" >&2 2>/dev/null || true
    return 1
  fi
  return 0
}

build_console_protocol_helper() {
  local helper="$TEST_ROOT/console_protocol_helper"
  [ -x "$helper" ] && { printf '%s\n' "$helper"; return 0; }
  cat >"$TEST_ROOT/console_protocol_helper.c" <<'EOF'
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int write_all(int fd, const void *buf, size_t n) {
  const unsigned char *p = (const unsigned char *)buf;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    p += (size_t)w;
    n -= (size_t)w;
  }
  return 0;
}

static int write_frame(int fd, unsigned char type, const char *payload) {
  size_t len = payload ? strlen(payload) : 0;
  unsigned char header[3];
  if (len > UINT16_MAX) return -1;
  header[0] = type;
  header[1] = (unsigned char)((len >> 8) & 0xff);
  header[2] = (unsigned char)(len & 0xff);
  if (write_all(fd, header, sizeof(header)) != 0) return -1;
  return len == 0 ? 0 : write_all(fd, payload, len);
}

int main(int argc, char **argv) {
  if (argc != 4) return 64;
  const char *sock_path = argv[1];
  const char *input = argv[2];
  const char *expect = argv[3];

  /* Connect via a short name relative to the socket's directory, mirroring
   * hold's own client, so a long store path stays within sun_path. */
  const char *slash = strrchr(sock_path, '/');
  if (!slash || !slash[1]) return 3;
  char dir[4096];
  size_t dlen = (size_t)(slash - sock_path);
  if (dlen >= sizeof(dir)) return 3;
  memcpy(dir, sock_path, dlen);
  dir[dlen] = '\0';
  const char *name = slash + 1;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 2;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(name) >= sizeof(addr.sun_path)) return 3;
  strcpy(addr.sun_path, name);
  if (chdir(dir) != 0) return 4;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 4;
  if (write_all(fd, "holdv1\0\0", 8) != 0) return 5;
  if (write_frame(fd, 'D', input) != 0) return 6;

  char seen[8192];
  size_t seen_len = 0;
  memset(seen, 0, sizeof(seen));
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    int sr = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (sr < 0 && errno == EINTR) continue;
    if (sr <= 0) return 7;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return 8;
    if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) return 9;
    size_t copy = (size_t)n;
    if (copy > sizeof(seen) - seen_len - 1) copy = sizeof(seen) - seen_len - 1;
    memcpy(seen + seen_len, buf, copy);
    seen_len += copy;
    seen[seen_len] = '\0';
    if (strstr(seen, expect)) {
      if (strcmp(expect, "attach denied") == 0) {
        close(fd);
        return 0;
      }
      break;
    }
  }

  if (write_frame(fd, 'X', NULL) != 0) return 10;
  close(fd);
  return 0;
}
EOF
  "${CC:-cc}" -std=c99 -Wall -Wextra -Werror "$TEST_ROOT/console_protocol_helper.c" -o "$helper"
  printf '%s\n' "$helper"
}

test_console_rejects_unrelated_peer_uid_before_replay() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  [ -n "$SUDO_BIN" ] || skip "sudo unavailable"
  id nobody >/dev/null 2>&1 || skip "no nobody user"
  local attacker_uid
  attacker_uid=$(id -u nobody)
  [ "$attacker_uid" != "0" ] && [ "$attacker_uid" != "$TEST_UID" ] || skip "nobody is not an unrelated user"
  "$SUDO_BIN" -n -u nobody true >/dev/null 2>&1 || skip "cannot run as nobody"

  local out id record sock helper rc log
  out=$("$HOLD_BIN" --console /bin/sh -c 'echo replay-secret; while read line; do echo "got:$line"; [ "$line" = done ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -S "$sock" ] || return 1
  log=$(log_path "$id") || return 1

  # Deliberately relax filesystem traversal and socket mode in this temp store
  # so the kernel accepts the unrelated connection attempt; the broker's peer
  # credential check must still reject it before replaying output or forwarding
  # input.
  chmod 0711 "$ACTOR_HOME" "$ACTOR_HOME/.local" "$ACTOR_HOME/.local/state" \
    "$HOME/.local/state/hold" "$HOME/.local/state/hold/console" || return 1
  chmod 0666 "$sock" || return 1

  helper=$(build_console_protocol_helper) || return 1
  mkdir -p "$TEST_ROOT/nobody-home" || return 1
  chmod 755 "$TEST_ROOT/nobody-home" || return 1
  set +e
  "$SUDO_BIN" -n -u nobody env HOME="$TEST_ROOT/nobody-home" "$helper" "$sock" 'steal
' 'attach denied' >"$TEST_ROOT/unauth-console.out" 2>"$TEST_ROOT/unauth-console.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/unauth-console.out" "$TEST_ROOT/unauth-console.err" >&2; return 1; }
  grep -q 'attach denied' "$TEST_ROOT/unauth-console.out" || { cat "$TEST_ROOT/unauth-console.out" >&2; return 1; }
  ! grep -q 'replay-secret' "$TEST_ROOT/unauth-console.out" || { echo "unauthorized attach received replay" >&2; return 1; }
  sleep 0.2
  ! grep -q 'got:steal' "$log" || { cat "$log" >&2; return 1; }

  printf 'done\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/authorized-console.out" 2>"$TEST_ROOT/authorized-console.err" || {
    cat "$TEST_ROOT/authorized-console.out" "$TEST_ROOT/authorized-console.err" >&2
    return 1
  }
  grep -q 'got:done' "$TEST_ROOT/authorized-console.out" || { cat "$TEST_ROOT/authorized-console.out" >&2; return 1; }
}

test_console_can_reattach_after_detach() {
  local out id store record sock helper
  out=$("$HOLD_BIN" --console /bin/sh -c 'while read line; do echo "seen:$line"; [ "$line" = done ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }

  helper=$(build_console_protocol_helper) || return 1
  as_user "$helper" "$sock" 'first
' 'seen:first' >"$TEST_ROOT/console-first.out" 2>"$TEST_ROOT/console-first.err" || {
    cat "$TEST_ROOT/console-first.out" "$TEST_ROOT/console-first.err" >&2
    return 1
  }
  grep -q 'seen:first' "$TEST_ROOT/console-first.out" || { cat "$TEST_ROOT/console-first.out" >&2; return 1; }

  printf 'done\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/console-second.out" 2>"$TEST_ROOT/console-second.err" || {
    cat "$TEST_ROOT/console-second.out" "$TEST_ROOT/console-second.err" >&2
    return 1
  }
  grep -q 'seen:first' "$TEST_ROOT/console-second.out" || { cat "$TEST_ROOT/console-second.out" >&2; return 1; }
  grep -q 'seen:done' "$TEST_ROOT/console-second.out" || { cat "$TEST_ROOT/console-second.out" >&2; return 1; }
  sleep 0.2
  logf=$(log_path "$id" "$store") || return 1
  grep -q 'seen:first' "$logf" || { cat "$logf" >&2; return 1; }
  grep -q 'seen:done' "$logf" || { cat "$logf" >&2; return 1; }
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_save_marks_record_and_is_idempotent() {
  local id name out
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  [ -n "$name" ] || { echo "run has no generated name" >&2; return 1; }
  # First save: stdout empty, stderr confirms, record flagged.
  out=$("$HOLD_BIN" save "$id" 2>"$TEST_ROOT/save1.err") || { echo "save rc nonzero" >&2; cat "$TEST_ROOT/save1.err" >&2; return 1; }
  [ -z "$out" ] || { echo "save wrote to stdout: [$out]" >&2; return 1; }
  grep -qF "hold: saved $id ($name)" "$TEST_ROOT/save1.err" || { cat "$TEST_ROOT/save1.err" >&2; return 1; }
  grep -q '"saved": true' "$(record_path "$id")" || { echo "record not marked saved" >&2; return 1; }
  # Second save is idempotent: still exit 0, same note, no stdout.
  out=$("$HOLD_BIN" save "$name" 2>"$TEST_ROOT/save2.err") || { echo "idempotent save rc nonzero" >&2; return 1; }
  [ -z "$out" ] || { echo "idempotent save wrote to stdout: [$out]" >&2; return 1; }
  grep -qF "hold: saved $id ($name)" "$TEST_ROOT/save2.err" || { cat "$TEST_ROOT/save2.err" >&2; return 1; }
}

test_purge_sweep_skips_saved() {
  local saved_id plain_id
  saved_id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  plain_id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$saved_id" ] && [ -n "$plain_id" ] || return 1
  "$HOLD_BIN" save "$saved_id" >/dev/null 2>&1 || return 1
  record_ended_soon "$saved_id" || return 1
  record_ended_soon "$plain_id" || return 1
  "$HOLD_BIN" purge >/dev/null 2>&1 || return 1
  record_exists "$saved_id" || { echo "sweeping purge removed a saved call" >&2; return 1; }
  ! record_exists "$plain_id" || { echo "sweeping purge left an unsaved ended call" >&2; return 1; }
  # -a (include stale) must also skip the saved call.
  "$HOLD_BIN" purge -a >/dev/null 2>&1 || return 1
  record_exists "$saved_id" || { echo "purge -a removed a saved call" >&2; return 1; }
}

test_purge_targeted_refuses_saved_exact_wording() {
  local id name pfx rc
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  # Let the reaper's exit stamp land before saving, so save is the last writer
  # and the saved flag cannot be lost to the finish-write.
  record_ended_soon "$id" || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  # Refusal by name: line 1 identifies by name, line 2 echoes the typed token.
  set +e; "$HOLD_BIN" purge "$name" >"$TEST_ROOT/refuse.out" 2>"$TEST_ROOT/refuse.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "targeted refusal rc=$rc (want 2)" >&2; cat "$TEST_ROOT/refuse.err" >&2; return 1; }
  [ ! -s "$TEST_ROOT/refuse.out" ] || { echo "refusal wrote to stdout" >&2; return 1; }
  printf "hold: '%s' is saved — purging a saved call requires --force\n  hold purge %s --force\n" "$name" "$name" >"$TEST_ROOT/refuse.want"
  diff -u "$TEST_ROOT/refuse.want" "$TEST_ROOT/refuse.err" || { echo "refusal wording mismatch (by name)" >&2; return 1; }
  # Refusal by id prefix: line 2 echoes exactly the prefix the user typed.
  pfx=$(printf '%s' "$id" | cut -c1-6)
  set +e; "$HOLD_BIN" purge "$pfx" 2>"$TEST_ROOT/refuse2.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "prefix refusal rc=$rc (want 2)" >&2; return 1; }
  printf "hold: '%s' is saved — purging a saved call requires --force\n  hold purge %s --force\n" "$name" "$pfx" >"$TEST_ROOT/refuse2.want"
  diff -u "$TEST_ROOT/refuse2.want" "$TEST_ROOT/refuse2.err" || { echo "refusal wording mismatch (by prefix)" >&2; return 1; }
  record_exists "$id" || { echo "refused purge still removed the call" >&2; return 1; }
}

test_purge_force_removes_saved_ended() {
  local id name
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || return 1
  # The copy-paste command from the refusal must succeed on an ended saved call.
  "$HOLD_BIN" purge "$name" --force >/dev/null 2>&1 || { echo "force purge of saved ended call failed" >&2; return 1; }
  ! record_exists "$id" || { echo "force did not remove the saved call" >&2; return 1; }
}

test_purge_force_removes_live_saved() {
  local id name pgid
  id=$("$HOLD_BIN" -d sleep 300 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  pgid=$(record_pgid "$id") || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  # --force must end the live call and remove it despite the saved flag.
  # Retry briefly: under load the TERM->KILL escalation can outlive one
  # purge invocation by a scheduler beat; a real regression still fails
  # every attempt for the full window.
  local tries=0
  while :; do
    "$HOLD_BIN" purge "$name" --force >/dev/null 2>&1
    if ! record_exists "$id" && pgid_terminated "$pgid"; then
      return 0
    fi
    tries=$((tries + 1))
    [ "$tries" -lt 20 ] || break
    sleep 0.1
  done
  ! record_exists "$id" || { echo "force did not remove the live saved call" >&2; return 1; }
  pgid_terminated "$pgid" || { echo "force purge left the process group alive" >&2; return 1; }
  return 1
}

test_rename_of_saved_call_keeps_protection() {
  local id newname rc
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || return 1
  "$HOLD_BIN" rename "$id" keptsafe >/dev/null 2>&1 || { echo "rename failed" >&2; return 1; }
  grep -q '"saved": true' "$(record_path "$id")" || { echo "rename cleared the saved flag" >&2; return 1; }
  set +e; "$HOLD_BIN" purge keptsafe 2>"$TEST_ROOT/rename-refuse.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "renamed saved call not protected: rc=$rc" >&2; return 1; }
  grep -qF "hold: 'keptsafe' is saved" "$TEST_ROOT/rename-refuse.err" || { cat "$TEST_ROOT/rename-refuse.err" >&2; return 1; }
  "$HOLD_BIN" purge keptsafe --force >/dev/null 2>&1 || true
}

test_rename_saves_an_unsaved_call() {
  local id rc
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  # Naming a call declares intent to keep it: rename implies save.
  "$HOLD_BIN" rename "$id" nowkept 2>"$TEST_ROOT/rename-save.err" || { echo "rename failed" >&2; return 1; }
  grep -qF "(saved)" "$TEST_ROOT/rename-save.err" || { cat "$TEST_ROOT/rename-save.err" >&2; return 1; }
  grep -q '"saved": true' "$(record_path "$id")" || { echo "rename did not save the call" >&2; return 1; }
  set +e; "$HOLD_BIN" purge nowkept 2>"$TEST_ROOT/rename-save-refuse.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "renamed call not protected: rc=$rc" >&2; return 1; }
  "$HOLD_BIN" purge nowkept --force >/dev/null 2>&1 || true
}

test_redial_honors_recorded_foreground_mode() {
  local out
  # A brief tail on the recipe so the foreground stream is deterministically
  # observable before the child exits (siblings below do the same); the test
  # pins redial honoring the foreground MODE, not zero-latency capture.
  "$HOLD_BIN" --name fgredial -- /bin/sh -c 'echo fg-redial-hit; sleep 0.2' >/dev/null 2>&1 || return 1
  # Foreground recipe: redial streams the output and prints no id of its own.
  out=$("$HOLD_BIN" fgredial 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -q 'fg-redial-hit' || { echo "foreground redial did not stream output: [$out]" >&2; return 1; }
  ! printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "foreground redial printed a detached id" >&2; return 1; }
  # An explicit -d on the redial overrides the recipe and detaches.
  out=$("$HOLD_BIN" -d fgredial 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "-d override did not detach: [$out]" >&2; return 1; }
}

test_redial_honors_recorded_detached_mode() {
  local id out
  id=$("$HOLD_BIN" -d --name dredial /bin/sh -c 'echo d-redial; sleep 0.1' 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  # Detached recipe: bare redial detaches and prints the 64-hex id.
  out=$("$HOLD_BIN" dredial 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "detached redial did not print a bare id: [$out]" >&2; return 1; }
}

test_redial_honors_recorded_console_mode() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  local sm id out
  pty_run "$HOLD_BIN -it --name itredial /bin/sh -c 'echo it-live-hit; sleep 0.4'" >/dev/null 2>&1 || true
  sm=$("$HOLD_BIN" inspect itredial 2>/dev/null | sed -n 's/.*"tty": \(true\).*/\1/p' | head -n1)
  [ "$sm" = "true" ] || { echo "itredial recipe tty=$sm (want true)" >&2; return 1; }
  id=$("$HOLD_BIN" inspect itredial 2>/dev/null | sed -n 's/.*"id": "\([0-9a-f]*\)".*/\1/p' | head -n1)
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  # Console recipe: bare redial replays on a PTY (attaches) rather than detaching.
  out=$(pty_run "$HOLD_BIN itredial" 2>&1) || true
  printf '%s\n' "$out" | grep -q 'it-live-hit' || { printf '%s\n' "$out" >&2; echo "console redial did not replay on a PTY" >&2; return 1; }
  ! printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "console redial detached instead of attaching" >&2; return 1; }
  record_ended_soon "$id" || return 1
  sm=$("$HOLD_BIN" inspect itredial 2>/dev/null | sed -n 's/.*"tty": \(true\).*/\1/p' | head -n1)
  [ "$sm" = "true" ] || { echo "console redial rewrote recipe tty=$sm (want true)" >&2; return 1; }
}

test_prune_by_id() {
  local out1 out2 id1 id2 store
  out1=$("$HOLD_BIN" -d true 2>&1) || return 1
  out2=$("$HOLD_BIN" -d true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/hold"
  "$HOLD_BIN" prune "$id1" >/dev/null || return 1
  ! record_exists "$id1" "$store" && ! log_exists "$id1" "$store" || return 1
  record_exists "$id2" "$store" && log_exists "$id2" "$store"
}

test_prune_all_keeps_running() {
  local out1 out2 id1 id2 store
  out1=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  out2=$("$HOLD_BIN" -d true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/hold"
  "$HOLD_BIN" prune all >/dev/null || return 1
  record_exists "$id1" "$store" || return 1
  ! record_exists "$id2" "$store" || return 1
  ! log_exists "$id2" "$store" || return 1
}

test_transactional_record_write_failure() {
  local rc pids
  set +e
  HOLD_TEST_FAIL_RECORD_WRITE=1 "$HOLD_BIN" -d bash -c 'exec -a hold_txn_test_sleep sleep 60' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  # Detect a leaked process via ps (a required tool) rather than pgrep with
  # `|| true`, which would pass vacuously if pgrep were absent.
  pids=$(ps -eo pid=,args= | awk '/hold_txn_test_sleep/ && !/awk/ {print $1}')
  [ -z "$pids" ]
}


make_fake_sudo() {
  mkdir -p "$TEST_ROOT/fakebin" || return 1
  cat > "$TEST_ROOT/fakebin/sudo" <<'SH'
#!/usr/bin/env bash
: "${HOLD_FAKE_SUDO_ARGV:?}"
printf '%s\n' "$@" > "$HOLD_FAKE_SUDO_ARGV"
exit "${HOLD_FAKE_SUDO_RC:-77}"
SH
  chmod 755 "$TEST_ROOT/fakebin" "$TEST_ROOT/fakebin/sudo" || return 1
  export HOLD_FAKE_SUDO_ARGV="$TEST_ROOT/sudo.argv"
  : > "$HOLD_FAKE_SUDO_ARGV" || return 1
  chmod 0666 "$HOLD_FAKE_SUDO_ARGV" || return 1
  export HOLD_FAKE_SUDO_RC=77
  export PATH="$TEST_ROOT/fakebin:$PATH"
}

write_public_index_fixture() {
  local id="$1" state="${2:-running}" started="${3:-2026-06-15T18:42:11Z}" alias="${4:-}"
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  local alias_json=""
  if [ -n "$alias" ]; then
    alias_json="\"alias\":\"$alias\","
  fi
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<JSON
{"id":"$id","root_managed":true,"requires_elevation":true,${alias_json}"state_hint":"$state","started_at":"$started","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
}


test_cap_flags_are_rejected() {
  # The grants-era capability flags were deleted; like -p/-v they are
  # rejected honestly rather than accepted and ignored.
  set +e
  "$HOLD_BIN" --cap-add NET_BIND_SERVICE -- true >/dev/null 2>"$TEST_ROOT/cap-add.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "cap-add: rc=$rc (want 5)" >&2; cat "$TEST_ROOT/cap-add.err" >&2; return 1; }
  grep -q 'does not manage capabilities' "$TEST_ROOT/cap-add.err" || { cat "$TEST_ROOT/cap-add.err" >&2; return 1; }
  set +e
  "$HOLD_BIN" --cap-drop=ALL -- true >/dev/null 2>"$TEST_ROOT/cap-drop.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "cap-drop: rc=$rc (want 5)" >&2; cat "$TEST_ROOT/cap-drop.err" >&2; return 1; }
  grep -q 'does not manage capabilities' "$TEST_ROOT/cap-drop.err" || { cat "$TEST_ROOT/cap-drop.err" >&2; return 1; }
}

test_docker_rm_removes_run_artifacts_after_exit() {
  local out id store json log
  store="$HOME/.local/state/hold"
  out=$("$HOLD_BIN" -d --rm -- /bin/sh -c 'echo rm-gone; sleep 0.3' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(record_path "$id" "$store") || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  log=$(log_path "$id" "$store") || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  [ -f "$json" ] || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  [ -f "$log" ] || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  sleep 0.1
  [ -f "$json" ] || { echo "--rm removed record before process exit" >&2; return 1; }
  path_absent_soon "$json" || { echo "--rm did not remove record $json" >&2; cat "$json" >&2 2>/dev/null || true; return 1; }
  path_absent_soon "$log" || { echo "--rm did not remove log $log" >&2; return 1; }
}

test_env_flag_is_recipe_data_only() {
  local out id store fake record log
  store="$HOME/.local/state/hold"
  fake="$TEST_ROOT/fake-home"
  mkdir -p "$fake" || return 1
  out=$("$HOLD_BIN" -d -e HOME="$fake" -e HOLD_MARKER=recipe -- /bin/sh -c 'echo "home:$HOME marker:$HOLD_MARKER"' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id" "$store") || { echo "-e HOME relocated the record store" >&2; return 1; }
  [ -f "$record" ] || { echo "-e HOME relocated the record store" >&2; return 1; }
  [ ! -d "$fake/.local/state/hold" ] || { echo "-e HOME leaked into hold's own environment" >&2; return 1; }
  record_ended_soon "$id" || return 1
  log=$(log_path "$id" "$store") || return 1
  grep -q "home:$fake marker:recipe" "$log" || { cat "$log" >&2; return 1; }
  out=$("$HOLD_BIN" -d -e PATH=/nonexistent -- sleep 0.1 2>&1) || { echo "-e PATH broke invoking-PATH command resolution" >&2; printf '%s\n' "$out" >&2; return 1; }
}


test_raw_start_does_not_steal_trailing_system() {
  local out id log
  out=$("$HOLD_BIN" -d sh -c 'printf "arg:%s\n" "$1"' sh --system 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  "$HOLD_BIN" logs --plain "$id" >"$TEST_ROOT/raw-start.log" || return 1
  grep -q '^arg:--system$' "$TEST_ROOT/raw-start.log"
}

test_public_root_index_list_is_redacted() {
  write_public_index_fixture abc12345cafe0000000000000000000000000000000000000000000000000000 running 2026-06-15T18:42:11Z || return 1
  # Public root-index rows appear with -a in redacted form: the USER and COMMAND
  # cells both read the literal "hidden" (exists-but-not-yours-to-see, not "-"),
  # and the STATUS reflects the projected state.
  "$HOLD_BIN" list -a >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  # Projected rows speak the table's Docker phrasing: "Up <age>", not the raw hint.
  grep -Eq '^abc12345cafe[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*[[:space:]]Up ' "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

test_public_root_index_list_reads_projected_state() {
  local id=deadbeefcafe0000000000000000000000000000000000000000000000000000
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<'JSON'
{
  "id": "deadbeefcafe0000000000000000000000000000000000000000000000000000",
  "root_managed": true,
  "state_hint": "exited",
  "created_at": "2026-06-15T18:42:10Z",
  "started_at": "2026-06-15T18:42:11Z",
  "ended_at": "2026-06-15T18:42:12Z",
  "exit_code": 7,
  "running": false,
  "argv": ["secret"],
  "cmdline_display": "secret command"
}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
  # The projected State.Status ("exited") drives the STATUS column; USER and
  # COMMAND stay redacted to the literal "hidden". Ended public rows need -a.
  "$HOLD_BIN" list -a >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  grep -Eq '^deadbeefcafe[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*[[:space:]]exited([[:space:]]|$)' "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

# `hold list` is the ledger: a user's own calls, live and past, by default, with
# a USER column naming the caller; -l/--live narrows it to the running ones.
test_list_default_is_full_ledger_live_narrows() {
  local live_out ended_out live_id ended_id me
  me="$TEST_USER" # the acting user (as root, the suite acts as a test user)
  live_out=$("$HOLD_BIN" -d --name ledger_live sleep 300 2>&1) || return 1
  live_id=$(printf '%s\n' "$live_out" | extract_id); [ -n "$live_id" ] || return 1
  ended_out=$("$HOLD_BIN" -d --name ledger_past /bin/sh -c 'exit 4' 2>&1) || return 1
  ended_id=$(printf '%s\n' "$ended_out" | extract_id); [ -n "$ended_id" ] || return 1
  record_ended_soon "$ended_id" || return 1
  # Default view: the USER column, then both the live and the past call, each
  # attributed to the calling user.
  "$HOLD_BIN" list >"$TEST_ROOT/ledger.out" 2>&1 || return 1
  grep -Eq "^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" "$TEST_ROOT/ledger.out" || { cat "$TEST_ROOT/ledger.out" >&2; return 1; }
  grep -Eq "^$live_id[[:space:]]+$me[[:space:]].*Up " "$TEST_ROOT/ledger.out" || { cat "$TEST_ROOT/ledger.out" >&2; return 1; }
  grep -Eq "^$ended_id[[:space:]]+$me[[:space:]].*Exited \\(4\\)" "$TEST_ROOT/ledger.out" || { cat "$TEST_ROOT/ledger.out" >&2; return 1; }
  # -l (and its --live long form) drop the past call, keep the live one.
  local flag
  for flag in -l --live; do
    "$HOLD_BIN" list "$flag" >"$TEST_ROOT/live.out" 2>&1 || return 1
    grep -Eq "^$live_id[[:space:]].*Up " "$TEST_ROOT/live.out" || { cat "$TEST_ROOT/live.out" >&2; return 1; }
    ! grep -q "^$ended_id" "$TEST_ROOT/live.out" || { echo "$flag still showed the past call" >&2; cat "$TEST_ROOT/live.out" >&2; return 1; }
  done
  "$HOLD_BIN" stop "$live_id" >/dev/null 2>&1 || true
}

# `hold list <name>` narrows the ledger to the call whose NAME matches. Regression
# for the filter matching the deleted profile-era alias field, which nothing set,
# so a named lookup of a real call printed an empty table.
test_list_name_filter_matches_call_name() {
  local out webby_id other_id
  out=$("$HOLD_BIN" -d --name webby sleep 300 2>&1) || return 1
  webby_id=$(printf '%s\n' "$out" | extract_id); [ -n "$webby_id" ] || return 1
  out=$("$HOLD_BIN" -d --name other_call sleep 300 2>&1) || return 1
  other_id=$(printf '%s\n' "$out" | extract_id); [ -n "$other_id" ] || return 1
  "$HOLD_BIN" list webby >"$TEST_ROOT/named.out" 2>&1 || return 1
  grep -Eq "^$webby_id[[:space:]].*[[:space:]]webby$" "$TEST_ROOT/named.out" || { cat "$TEST_ROOT/named.out" >&2; return 1; }
  ! grep -q 'other_call' "$TEST_ROOT/named.out" || { echo "name filter leaked another call" >&2; cat "$TEST_ROOT/named.out" >&2; return 1; }
  "$HOLD_BIN" stop "$webby_id" >/dev/null 2>&1 || true
  "$HOLD_BIN" stop "$other_id" >/dev/null 2>&1 || true
}

# list -a renders the observed ports root projected into a global call's public
# entry, in the same redacted row (USER and COMMAND both "hidden").
test_list_all_renders_global_ports_projection() {
  local id=facefeed12340000000000000000000000000000000000000000000000000000
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<'JSON'
{"id":"facefeed12340000000000000000000000000000000000000000000000000000","root_managed":true,"name":"global_web","state_hint":"running","created_at":"2026-06-15T18:42:11Z","observed_ports":"127.0.0.1:8080/tcp, [::]:9090/tcp","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
  "$HOLD_BIN" list -a >"$TEST_ROOT/list.out" 2>&1 || return 1
  # The redacted row shows the generated name and the projected ports; USER and
  # COMMAND read "hidden" and the command line never appears.
  grep -Eq "^${id:0:12}[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*127\\.0\\.0\\.1:8080/tcp.*global_web$" "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

# `hold list -s/--system` (and the equivalent `--system list`) is exactly the
# redacted global view: no personal calls leak into it.
test_system_list_is_redacted_global_only() {
  local out personal_id gid=beadfeed56780000000000000000000000000000000000000000000000000000 form
  out=$("$HOLD_BIN" -d --name my_personal sleep 300 2>&1) || return 1
  personal_id=$(printf '%s\n' "$out" | extract_id); [ -n "$personal_id" ] || return 1
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" <<'JSON'
{"id":"beadfeed56780000000000000000000000000000000000000000000000000000","root_managed":true,"name":"global_only","state_hint":"running","created_at":"2026-06-15T18:42:11Z","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" || return 1
  # -s, --system, and the pre-command --system spelling all mean the same view.
  for form in "list -s" "list --system" "--system list"; do
    # shellcheck disable=SC2086
    "$HOLD_BIN" $form >"$TEST_ROOT/system-list.out" 2>&1 || { echo "form: $form" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
    grep -Eq "^${gid:0:12}[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*global_only$" "$TEST_ROOT/system-list.out" || { echo "form: $form" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
    ! grep -q "^$personal_id" "$TEST_ROOT/system-list.out" || { echo "form '$form' leaked a personal call" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
    ! grep -q 'my_personal' "$TEST_ROOT/system-list.out" || { echo "form: $form" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
  done
  "$HOLD_BIN" stop "$personal_id" >/dev/null 2>&1 || true
}

# ps is Docker's machine-wide running view: running calls from both scopes, the
# global ones redacted, no USER column; -a adds ended; non-Docker flags reject.
test_ps_is_docker_machine_wide_running_view() {
  local live_out live_id ended_out ended_id gid=c1cadafeed990000000000000000000000000000000000000000000000000000 rc
  live_out=$("$HOLD_BIN" -d --name ps_live sleep 300 2>&1) || return 1
  live_id=$(printf '%s\n' "$live_out" | extract_id); [ -n "$live_id" ] || return 1
  ended_out=$("$HOLD_BIN" -d --name ps_past /bin/sh -c 'exit 2' 2>&1) || return 1
  ended_id=$(printf '%s\n' "$ended_out" | extract_id); [ -n "$ended_id" ] || return 1
  record_ended_soon "$ended_id" || return 1
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" <<'JSON'
{"id":"c1cadafeed990000000000000000000000000000000000000000000000000000","root_managed":true,"name":"ps_global","state_hint":"running","created_at":"2026-06-15T18:42:11Z","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" || return 1
  # ps: Docker's header (no USER column), running personal + global, past hidden.
  "$HOLD_BIN" ps >"$TEST_ROOT/ps.out" 2>&1 || return 1
  grep -Eq "^CALL ID[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" "$TEST_ROOT/ps.out" || { cat "$TEST_ROOT/ps.out" >&2; return 1; }
  grep -Eq "^$live_id[[:space:]].*Up .*ps_live$" "$TEST_ROOT/ps.out" || { cat "$TEST_ROOT/ps.out" >&2; return 1; }
  grep -Eq "^${gid:0:12}[[:space:]]+hidden[[:space:]].*ps_global$" "$TEST_ROOT/ps.out" || { cat "$TEST_ROOT/ps.out" >&2; return 1; }
  ! grep -q "^$ended_id" "$TEST_ROOT/ps.out" || { echo "ps default showed an ended call" >&2; cat "$TEST_ROOT/ps.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/ps.out"
  # ps -a includes the ended personal call.
  "$HOLD_BIN" ps -a >"$TEST_ROOT/ps-a.out" 2>&1 || return 1
  grep -Eq "^$ended_id[[:space:]].*Exited \\(2\\)" "$TEST_ROOT/ps-a.out" || { cat "$TEST_ROOT/ps-a.out" >&2; return 1; }
  # ps speaks only Docker: -l and -s are rejected as unknown flags.
  local badflag
  for badflag in -l -s -u; do
    set +e; "$HOLD_BIN" ps "$badflag" >/dev/null 2>"$TEST_ROOT/ps-bad.err"; rc=$?; set -e
    [ "$rc" -eq 1 ] || { echo "ps $badflag: rc=$rc (want 1)" >&2; return 1; }
    grep -q "unknown flag '$badflag'" "$TEST_ROOT/ps-bad.err" || { cat "$TEST_ROOT/ps-bad.err" >&2; return 1; }
  done
  "$HOLD_BIN" stop "$live_id" >/dev/null 2>&1 || true
}

# `hold purge -s` as a normal user re-execs through sudo (so sudo can prompt),
# passing exactly `<self> purge --system` (plus any state flags).
test_purge_system_reexecs_through_sudo() {
  make_fake_sudo || return 1
  export HOLD_FAKE_SUDO_RC=0
  # The re-exec replaces the process with (fake) sudo, which records its argv,
  # one entry per line: the self binary path, then purge, then --system.
  set +e
  "$HOLD_BIN" purge -s >/dev/null 2>&1
  set -e
  grep -qx 'purge' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing 'purge'" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  grep -qx -- '--system' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing '--system'" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  grep -q 'hold' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing the self binary path" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  ! grep -qx -- '--all' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv carried an unrequested --all" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  # -a is forwarded as a state widener when requested.
  : > "$HOLD_FAKE_SUDO_ARGV"
  set +e
  "$HOLD_BIN" purge -s -a >/dev/null 2>&1
  set -e
  grep -qx -- '--all' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing forwarded '--all'" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
}

# As root, `list` is the global working view; `list -a` also walks every user's
# personal store and adds a USER column so ownership is clear.
test_root_list_all_labels_owner_and_walks_homes() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  local homes="$TEST_ROOT/homes" uid owner
  uid=$(id -u); owner=$(id -un)
  # A fabricated personal store under a test home root, holding one call owned by
  # the invoking user (so its USER resolves via /etc/passwd). The record shape
  # mirrors the minimal valid record, with a live-looking pgid so it is a row.
  mkdir -p "$homes/somebody/.local/state/hold" || return 1
  cat > "$homes/somebody/.local/state/hold/aa11bb22cc330000000000000000000000000000000000000000000000000000.json" <<JSON
{"version":1,"id":"aa11bb22cc330000000000000000000000000000000000000000000000000000","pid":2,"pgid":2,"sid":2,"start_unix_ns":0,"argv":["peer-cmd"],"cmdline_display":"peer-cmd","uid":$uid,"gid":$uid,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0,"name":"peer_call"}
JSON
  chmod -R 755 "$homes" || return 1
  # A global call in the system store (root's own view).
  local out gid_call
  out=$(as_root env HOLD_TEST_HOME_ROOT="$homes" "$HOLD_REAL_BIN" -d --name global_root sleep 300 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  gid_call=$(printf '%s\n' "$out" | extract_id); [ -n "$gid_call" ] || { printf '%s\n' "$out" >&2; return 1; }
  # Plain root list: the global call only, no personal rows.
  as_root env HOLD_TEST_HOME_ROOT="$homes" "$HOLD_REAL_BIN" list >"$TEST_ROOT/root-list.out" 2>&1 || return 1
  grep -Eq "^$gid_call[[:space:]].*global_root$" "$TEST_ROOT/root-list.out" || { cat "$TEST_ROOT/root-list.out" >&2; return 1; }
  ! grep -q 'peer_call' "$TEST_ROOT/root-list.out" || { echo "plain root list walked personal stores" >&2; cat "$TEST_ROOT/root-list.out" >&2; return 1; }
  # Root list -a: USER column (second, after CALL ID); the peer call attributed
  # to its record owner, the global call to its recorded invoking user. Under
  # sudo that is the invoking account; run directly as root it would be "root".
  # In both cases the label equals `id -un` in this harness, so assert that.
  as_root env HOLD_TEST_HOME_ROOT="$homes" "$HOLD_REAL_BIN" list -a >"$TEST_ROOT/root-list-a.out" 2>&1 || return 1
  grep -Eq "^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" "$TEST_ROOT/root-list-a.out" || { cat "$TEST_ROOT/root-list-a.out" >&2; return 1; }
  grep -Eq "^aa11bb22cc33[[:space:]]+$owner[[:space:]].*peer_call$" "$TEST_ROOT/root-list-a.out" || { cat "$TEST_ROOT/root-list-a.out" >&2; return 1; }
  grep -Eq "^$gid_call[[:space:]]+$owner[[:space:]].*global_root$" "$TEST_ROOT/root-list-a.out" || { cat "$TEST_ROOT/root-list-a.out" >&2; return 1; }
  as_root "$HOLD_REAL_BIN" stop "system:$gid_call" >/dev/null 2>&1 || true
}

test_user_local_wins_over_public_root_collision() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  # A root-public entry that collides on the call's display prefix: the local
  # record must win so stop resolves it without a sudo re-exec.
  local pub_id="${id}0000000000000000000000000000000000000000000000000000"
  write_public_index_fixture "$pub_id" running 2026-06-15T18:42:11Z || return 1
  make_fake_sudo || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  [ ! -s "$HOLD_FAKE_SUDO_ARGV" ] || return 1
  pgid_terminated "$pgid" || return 1
  record_exists "$pub_id" "$HOLD_TEST_SYSTEM_STATE_DIR/public"
}

test_explicit_user_target() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  "$HOLD_BIN" stop "user:$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}


test_tail_ctrl_c_detaches_from_tail_and_keeps_run() {
  command -v setsid >/dev/null 2>&1 || skip "setsid not available"
  local out id pgid tail_pid rc
  out=$("$HOLD_BIN" -d bash -c 'while :; do echo tail-still-running; sleep 1; done' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  setsid "$HOLD_BIN" tail "$id" >"$TEST_ROOT/tail.out" 2>"$TEST_ROOT/tail.err" &
  tail_pid=$!
  sleep 0.3
  kill -INT "-$tail_pid" 2>/dev/null || kill -INT "$tail_pid" 2>/dev/null || true
  set +e
  wait "$tail_pid"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || return 1
  kill -0 "-$pgid" 2>/dev/null || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}


test_long_command_list_truncates_instead_of_skips() {
  local out id long_arg list_out
  long_arg=$(printf 'x%.0s' $(seq 1 140))
  out=$("$HOLD_BIN" -d /bin/echo "$long_arg" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  list_out=$("$HOLD_BIN" list -a) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]" || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*\.\.\."
}

test_normal_start_writes_user_local_state() {
  local out id mode
  out=$("$HOLD_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(record_path "$id") || return 1
  log=$(log_path "$id") || return 1
  ! root_record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  ! root_log_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs" || return 1
  ! record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  mode=$(file_mode "$json") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(file_mode "$log") || return 1
  [ "$mode" = 600 ] || return 1
  python3 - "$json" <<'PY' || { cat "$json" >&2; return 1; }
import json, sys
obj = json.load(open(sys.argv[1], encoding='utf-8'))
required_top = {'saved', 'version', 'id', 'run_id', 'pid', 'pgid', 'sid', 'start_unix_ns', 'created_unix_ns', 'argv', 'name', 'state', 'started_at', 'created_at', 'log_path', 'boot_id', 'proc_starttime_ticks', 'exe_dev', 'exe_ino'}
missing = required_top - set(obj)
if missing:
    raise SystemExit(f'missing native record fields: {sorted(missing)}')
for legacy in ('Id', 'State', 'Config', 'Path', 'Args', 'LogPath', 'SessionMode', 'CapAdd'):
    if legacy in obj:
        raise SystemExit(f'legacy Docker-shaped key present in native record: {legacy}')
if not obj.get('argv'):
    raise SystemExit('record argv missing/empty')
observed_cwd = (obj.get('observed') or {}).get('cwd')
if observed_cwd and (not observed_cwd.startswith('/') or (observed_cwd != '/' and observed_cwd.endswith('/'))):
    raise SystemExit(f'observed cwd is not a normalized absolute path: {observed_cwd!r}')
PY
}

test_native_record_is_readable_when_hand_authored() {
  # A minimal hand-authored native record (the one schema since 0.7) loads and
  # renders; unknown/legacy keys are ignored, never fatal.
  local id store record logdir logpath idxpath ps_out
  id=fe4a4b8fbc063e25cd4bcb6798791b1dd03caca62f6a0b4c1304f28940992c52
  store="$HOME/.local/state/hold"
  mkdir -p "$store" || return 1
  logdir="$HOME/.local/state/hold-logs/$id"
  mkdir -p "$logdir" || return 1
  logpath="$logdir/$id.log"
  idxpath="$logpath.idx"
  : >"$logpath" || return 1
  : >"$idxpath" || return 1
  record="$store/$id.json"
  cat >"$record" <<JSON
{
  "saved": false,
  "version": 1,
  "id": "$id",
  "run_id": "$id",
  "pid": 999999,
  "pgid": 999999,
  "sid": 999999,
  "start_unix_ns": 0,
  "created_unix_ns": 0,
  "argv": ["/usr/bin/python3", "-m", "http.server", "8080"],
  "name": "adjective_noun",
  "state": "exited",
  "exit_code": 7,
  "started_at": "2026-06-28T17:30:47Z",
  "created_at": "2026-06-28T17:30:47Z",
  "ended_at": "2026-06-28T17:30:48Z",
  "log_path": "$logpath",
  "log_idx_path": "$idxpath",
  "LegacyIgnoredKey": {"nested": true},
  "uid": $(id -u),
  "gid": $(id -g),
  "proc_starttime_ticks": 0,
  "exe_dev": 0,
  "exe_ino": 0
}
JSON
  chmod 600 "$record" || return 1
  # As root the suite acts as a test user, who must own what it reads.
  if [ "${USER_ACTOR_NEEDS_SUDO:-0}" -eq 1 ]; then chown -R "$TEST_UID:$TEST_GID" "$HOME/.local" || return 1; fi
  ps_out=$("$HOLD_BIN" ps -a 2>"$TEST_ROOT/native-reader.err") || {
    cat "$TEST_ROOT/native-reader.err" >&2
    return 1
  }
  ! grep -q 'skipping corrupt record' "$TEST_ROOT/native-reader.err" || { cat "$TEST_ROOT/native-reader.err" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -q '^fe4a4b8fbc06' || { printf '%s\n' "$ps_out" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -q '"/usr/bin/python3 -m http' || { printf '%s\n' "$ps_out" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -q 'adjective_noun' || { printf '%s\n' "$ps_out" >&2; return 1; }
}

test_root_start_writes_system_store_and_public_unknown() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id mode list_out
  out=$(as_root "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s
' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  log=$(root_log_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs") || return 1
  public_json=$(record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public") || return 1
  ! record_exists "$id" "$ROOT_HOME/.local/state/hold" || return 1
  mode=$(root_file_mode "$json") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(root_file_mode "$log") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(file_mode "$public_json") || return 1
  [ "$mode" = 644 ] || return 1
  grep -Eq '"state_hint": "(running|exited)"' "$public_json" || { cat "$public_json" >&2; return 1; }
  grep -q '"pid": [1-9]' "$public_json" || { cat "$public_json" >&2; return 1; }
  grep -q '"started_at": "' "$public_json" || { cat "$public_json" >&2; return 1; }
  matched=0
  for _ in $(seq 1 30); do
    as_root cat "$json" >"$TEST_ROOT/root-private-state.json" || return 1
    if python3 - "$TEST_ROOT/root-private-state.json" "$public_json" >"$TEST_ROOT/root-public-state-compare.err" 2>&1 <<'PY'
import json, sys
private = json.load(open(sys.argv[1], encoding='utf-8'))
public = json.load(open(sys.argv[2], encoding='utf-8'))
projection_keys = {'id', 'state_hint', 'started_at', 'created_at', 'pid', 'pgid', 'sid', 'running'}
missing_projection = projection_keys - set(public)
if missing_projection:
    raise SystemExit(f'public projection missing keys: {sorted(missing_projection)}')
# The projection must never leak what only root may see.
for forbidden in ('argv', 'env', 'cmdline_display', 'invoked_by_user', 'observed', 'Config'):
    if forbidden in public:
        raise SystemExit(f'public projection leaks private key: {forbidden}')
if public['id'] != private['id']:
    raise SystemExit(f'public/private id drift: {public["id"]!r} != {private["id"]!r}')
for key in ('pid', 'pgid', 'sid'):
    if public[key] != private[key]:
        raise SystemExit(f'public/private drift for {key}: {private[key]!r} != {public[key]!r}')
if public.get('state_hint') != private.get('state'):
    raise SystemExit(f'state drift: public {public.get("state_hint")!r} != private {private.get("state")!r}')
PY
    then
      matched=1
      break
    fi
    sleep 0.1
  done
  [ "$matched" = 1 ] || { cat "$TEST_ROOT/root-public-state-compare.err" "$TEST_ROOT/root-private-state.json" "$public_json" >&2; return 1; }
  list_out=$("$HOLD_BIN" list -a) || return 1
  printf '%s
' "$list_out" | grep -Eq "^$id[[:space:]].*(Up|Exited|running|exited)"
}

test_sudo_start_writes_system_store_with_invoking_metadata() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id json
  out=$(as_sudo_from_user "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  root_log_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs" || return 1
  record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  ! record_exists "$id" "$ACTOR_HOME/.local/state/hold" || return 1
  root_grep '"invoked_by_uid": '"$TEST_UID" "$json" || return 1
  root_grep '"invoked_by_gid": '"$TEST_GID" "$json" || return 1
  root_grep '"invoked_by_user": "'"$TEST_USER"'"' "$json" || return 1
  root_grep '"invoked_via_sudo": true' "$json"
}

make_home_executable() {
  local app="$ACTOR_HOME/bin/home-tool"
  mkdir -p "$ACTOR_HOME/bin" || return 1
  cat > "$app" <<'SH'
#!/bin/sh
printf 'home-tool\n'
SH
  chmod 755 "$app" || return 1
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ]; then
    as_root chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME/bin" "$app" || return 1
  fi
  printf '%s\n' "$app"
}

make_sleeping_home_executable() {
  local app="$ACTOR_HOME/bin/home-tool"
  mkdir -p "$ACTOR_HOME/bin" || return 1
  cat > "$app" <<'SH'
#!/bin/sh
printf 'home-tool\n'
sleep 5
SH
  chmod 755 "$app" || return 1
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ]; then
    as_root chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME/bin" "$app" || return 1
  fi
  printf '%s\n' "$app"
}

assert_home_system_start_is_user_local() {
  local out="$1" id json log owner
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(root_record_path "$id" "$ACTOR_HOME/.local/state/hold") || return 1
  log=$(root_log_path "$id" "$ACTOR_HOME/.local/state/hold") || return 1
  root_file_exists "$json" || return 1
  root_file_exists "$log" || return 1
  ! root_record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  ! root_log_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs" || return 1
  ! record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  owner=$(root_file_owner "$json") || return 1
  [ "$owner" = "$TEST_UID:$TEST_GID" ] || return 1
  owner=$(root_file_owner "$log") || return 1
  [ "$owner" = "$TEST_UID:$TEST_GID" ] || return 1
}

test_sudo_system_start_of_home_executable_uses_user_store() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local app out
  app=$(make_home_executable) || return 1
  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d "$app" 2>&1) || return 1
  # Files materialize through a sudo child; poll briefly instead of asserting
  # one instant. A real placement bug fails every attempt in the window.
  local sudo_tries=0
  while :; do
    if assert_home_system_start_is_user_local "$out"; then
      return 0
    fi
    sudo_tries=$((sudo_tries + 1))
    [ "$sudo_tries" -lt 20 ] || break
    sleep 0.1
  done
  assert_home_system_start_is_user_local "$out"
}

test_sudo_system_start_of_home_argv_paths_uses_user_store() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local cfg out id
  cfg="$ACTOR_HOME/app.toml"
  as_user sh -c 'printf "%s\n" "config" >"$1"' sh "$cfg" || return 1

  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d /bin/sh -c 'sleep 5' "$cfg" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id); [ -n "$id" ] && as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true

  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d /bin/sh -c 'sleep 5' --config="$cfg" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id); [ -n "$id" ] && as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true

  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d /bin/sh -c 'sleep 5' --config "$cfg" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id); [ -n "$id" ] && as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true
}


test_sudo_context_can_stop_unique_user_local_run() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}


test_docker_shaped_cli_flags_and_rm() {
  local out id id2
  out=$("$HOLD_BIN" -d --name docker-web -e HOLD_DOCKER_ENV=ok -- /bin/sh -c 'echo "$HOLD_DOCKER_ENV"; sleep 0.1' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  # Poll for the captured line: under load the logger can flush later than a
  # fixed 0.2s; a real capture regression still fails the whole window.
  local logs_tries=0
  while :; do
    "$HOLD_BIN" logs -n 1 --plain "$id" >"$TEST_ROOT/docker-logs-tail.out" 2>/dev/null || true
    grep -q '^ok$' "$TEST_ROOT/docker-logs-tail.out" && break
    logs_tries=$((logs_tries + 1))
    [ "$logs_tries" -lt 20 ] || { cat "$TEST_ROOT/docker-logs-tail.out" >&2; return 1; }
    sleep 0.1
  done
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/docker-inspect.json" || return 1
  python3 - "$TEST_ROOT/docker-inspect.json" "$id" <<'PY' || { cat "$TEST_ROOT/docker-inspect.json" >&2; return 1; }
import json, sys
obj = json.load(open(sys.argv[1], encoding='utf-8'))
if not isinstance(obj, list) or len(obj) != 1:
    raise SystemExit('inspect must return a one-object JSON array')
record = obj[0]
if not str(record.get('id', '')).startswith(sys.argv[2]):
    raise SystemExit('inspect record id mismatch')
if record.get('name') != 'docker-web':
    raise SystemExit('inspect record did not include run name')
if 'ok\n' in json.dumps(record):
    raise SystemExit('inspect returned log text instead of structured record data')
PY
  # The 0.1s child may still be live when the log line lands; poll until
  # ps -a reports it Exited rather than sleeping a fixed beat.
  local ps_tries=0
  while :; do
    "$HOLD_BIN" ps -a >"$TEST_ROOT/docker-ps.out" || return 1
    grep -Eq "^$id[[:space:]]+.*Exited.*docker-web$" "$TEST_ROOT/docker-ps.out" && break
    ps_tries=$((ps_tries + 1))
    [ "$ps_tries" -lt 20 ] || { cat "$TEST_ROOT/docker-ps.out" >&2; return 1; }
    sleep 0.1
  done
  grep -Eq "^CALL ID[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES" "$TEST_ROOT/docker-ps.out" || { cat "$TEST_ROOT/docker-ps.out" >&2; return 1; }

  out=$("$HOLD_BIN" -d --name docker-direct /bin/sh -c 'echo direct; sleep 5' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id2=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id2" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" rm --force docker-direct >/dev/null || return 1
  set +e; "$HOLD_BIN" inspect "$id2" >"$TEST_ROOT/docker-rm-inspect.out" 2>&1; rc=$?; set -e
  [ "$rc" -ne 0 ] || { cat "$TEST_ROOT/docker-rm-inspect.out" >&2; return 1; }
}

# The ps table humanizes CREATED Docker-style, phrases STATUS as Up/Exited, and
# annotates a saved call inline on STATUS.
test_ps_table_humanized_status_and_saved() {
  local out id run_out exit_id ps_out
  out=$("$HOLD_BIN" -d --name ps-humanized -- /bin/sh -c 'sleep 30' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  run_out=$("$HOLD_BIN" -d --name ps-exiter -- /bin/sh -c 'exit 5' 2>&1) || return 1
  exit_id=$(printf '%s\n' "$run_out" | extract_id)
  [ -n "$exit_id" ] || return 1
  record_ended_soon "$exit_id" || return 1
  # Age both calls past a second so CREATED reads a whole-unit Docker age
  # ("1 second ago") rather than the sub-second "Less than a second ago".
  sleep 1.2

  ps_out=$("$HOLD_BIN" ps -a) || return 1
  # Header is exact, in the CALL ID..NAMES order with no PROFILE column.
  printf '%s\n' "$ps_out" | grep -Eq "^CALL ID[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" ||
    { printf '%s\n' "$ps_out" >&2; return 1; }
  # CREATED is humanized: never a raw ISO stamp, always a Docker-style age.
  printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*(About a minute|[0-9]+ (seconds?|minutes?|hours?|days?|weeks?|months?)) ago" ||
    { printf '%s\n' "$ps_out" >&2; return 1; }
  ! printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*[0-9]{4}-[0-9]{2}-[0-9]{2}T" || return 1
  # STATUS phrasing: Up ... for the live call, Exited (5) ... ago for the ended one.
  printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*Up [0-9A-Za-z]" || { printf '%s\n' "$ps_out" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -Eq "^$exit_id[[:space:]].*Exited \(5\) .*ago" || { printf '%s\n' "$ps_out" >&2; return 1; }

  # save marks the live call; ps annotates protection inline on STATUS.
  "$HOLD_BIN" save ps-humanized >/dev/null 2>&1 || return 1
  ps_out=$("$HOLD_BIN" ps -a) || return 1
  printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*Up .*\(saved\)" || { printf '%s\n' "$ps_out" >&2; return 1; }

  # list carries the same Up/Exited/(saved) STATUS phrasing as ps, but its own
  # header adds the USER column in Docker's IMAGE slot (second, after CALL ID).
  list_out=$("$HOLD_BIN" list -a) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" ||
    { printf '%s\n' "$list_out" >&2; return 1; }
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Up .*\(saved\)" || { printf '%s\n' "$list_out" >&2; return 1; }
  printf '%s\n' "$list_out" | grep -Eq "^$exit_id[[:space:]].*Exited \(5\) .*ago" || { printf '%s\n' "$list_out" >&2; return 1; }

  "$HOLD_BIN" end ps-humanized >/dev/null 2>&1 || true
}

# Content-sized columns must not shear: every data cell begins exactly under its
# header, whether the command is short or long enough to be ellipsized.
test_ps_table_columns_do_not_shear() {
  local short_out long_out ps_out long_arg
  short_out=$("$HOLD_BIN" -d --name ps-short -- /bin/sh -c 'sleep 30' 2>&1) || return 1
  long_arg=$(printf 'y%.0s' $(seq 1 120))
  long_out=$("$HOLD_BIN" -d --name ps-long -- /bin/sh -c "echo $long_arg; sleep 30" 2>&1) || return 1
  [ -n "$(printf '%s\n' "$short_out" | extract_id)" ] || return 1
  [ -n "$(printf '%s\n' "$long_out" | extract_id)" ] || return 1

  ps_out=$("$HOLD_BIN" ps -a) || return 1
  printf '%s\n' "$ps_out" | awk '
    NR == 1 {
      for (label = 1; label <= 4; label++) { }
      cols["COMMAND"] = index($0, "COMMAND")
      cols["CREATED"] = index($0, "CREATED")
      cols["STATUS"]  = index($0, "STATUS")
      next
    }
    {
      for (name in cols) {
        pos = cols[name]
        # The gutter before the column is a space and the cell itself is not,
        # so the column starts exactly where its header does.
        if (substr($0, pos - 1, 1) != " " || substr($0, pos, 1) == " ") {
          printf("shear at %s on: %s\n", name, $0) > "/dev/stderr"
          exit 1
        }
      }
    }
  ' || return 1
  # The long command is present and ellipsized rather than dropped.
  printf '%s\n' "$ps_out" | grep -Eq 'ps-long$' || return 1
  printf '%s\n' "$ps_out" | grep -Eq '\.\.\."' || return 1

  "$HOLD_BIN" end ps-short ps-long >/dev/null 2>&1 || true
}

# hold ports lists the bound listeners of a call's process group, one per line.
test_ports_lists_bound_listener() {
  local out id ports_out tries rc
  [ "$(uname -s)" = Linux ] || skip "process observation (ports) is Linux-only"
  out=$("$HOLD_BIN" -d --name ports-smoke -- python3 -c 'import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",0)); s.listen(); print(s.getsockname()[1]); time.sleep(30)' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  ports_out=""
  for tries in $(seq 1 30); do
    ports_out=$("$HOLD_BIN" ports ports-smoke 2>/dev/null || true)
    printf '%s\n' "$ports_out" | grep -Eq '^127\.0\.0\.1:[0-9]+/tcp$' && break
    sleep 0.1
  done
  printf '%s\n' "$ports_out" | grep -Eq '^127\.0\.0\.1:[0-9]+/tcp$' || { printf '%s\n' "$ports_out" >&2; return 1; }

  # No target is a usage error.
  set +e; "$HOLD_BIN" ports >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 5 ] || return 1

  "$HOLD_BIN" end ports-smoke >/dev/null 2>&1 || true
}

# hold stats --no-stream prints one plain frame with the documented columns.
test_stats_single_shot_shape() {
  local out id stats_out
  out=$("$HOLD_BIN" -d --name stats-smoke -- /bin/sh -c 'sleep 30' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  stats_out=$("$HOLD_BIN" stats --no-stream stats-smoke) || return 1
  printf '%s\n' "$stats_out" | grep -Eq "^CALL ID[[:space:]]+NAME[[:space:]]+CPU %[[:space:]]+MEM \(RSS\)[[:space:]]+PIDS$" ||
    { printf '%s\n' "$stats_out" >&2; return 1; }
  printf '%s\n' "$stats_out" | grep -Eq "^$id[[:space:]]+stats-smoke[[:space:]]+[0-9.]+%[[:space:]]+[0-9.]+(B|KiB|MiB|GiB)[[:space:]]+[0-9]+$" ||
    { printf '%s\n' "$stats_out" >&2; return 1; }

  "$HOLD_BIN" end stats-smoke >/dev/null 2>&1 || true
}

# inspect reports where a live call's fds actually point. A child that redirects
# its own stdout to a file must show that file under Stdio.Stdout.
test_inspect_reports_stdio_targets() {
  [ "$(uname -s)" = Linux ] || skip "process observation (stdio fd targets) is Linux-only"
  local redir out id
  redir="$(actor_dir)/inspect-stdio.out" || return 1
  out=$("$HOLD_BIN" -d --name stdio-smoke -- /bin/sh -c "exec >\"$redir\"; sleep 30" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  "$HOLD_BIN" inspect stdio-smoke >"$TEST_ROOT/inspect-stdio.json" || return 1
  python3 - "$TEST_ROOT/inspect-stdio.json" "$redir" <<'PY' || { cat "$TEST_ROOT/inspect-stdio.json" >&2; return 1; }
import json, sys
obj = json.load(open(sys.argv[1], encoding='utf-8'))
record = obj[0]
stdio = record.get('Stdio')
if not isinstance(stdio, dict):
    raise SystemExit('inspect did not report a Stdio object for a live call')
for key in ('Stdin', 'Stdout', 'Stderr'):
    if key not in stdio:
        raise SystemExit('Stdio missing key ' + key)
if stdio['Stdout'] != sys.argv[2]:
    raise SystemExit('Stdout fd target %r did not match the redirect %r' % (stdio['Stdout'], sys.argv[2]))
PY
  "$HOLD_BIN" end stdio-smoke >/dev/null 2>&1 || true
}

test_docker_bare_launch_foreground_follows_output_by_default() {
  local out id
  # Bare foreground streams the process output and prints no id of its own.
  out=$("$HOLD_BIN" -- /bin/sh -c 'echo bare-foreground-ok' 2>"$TEST_ROOT/docker-bare-foreground.err") || {
    cat "$TEST_ROOT/docker-bare-foreground.err" >&2
    return 1
  }
  grep -q '^bare-foreground-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }
  if grep -Eq '^[0-9a-f]{12}$|^[0-9a-f]{64}$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi

  # Detached prints exactly the bare 64-hex id and hides the process output.
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo bare-detached-hidden; sleep 0.1' 2>"$TEST_ROOT/docker-bare-detached.err") || {
    cat "$TEST_ROOT/docker-bare-detached.err" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  if grep -q '^bare-detached-hidden$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}


test_docker_run_foreground_follows_output_by_default() {
  local out id
  out=$("$HOLD_BIN" -- /bin/sh -c 'echo docker-foreground-ok' 2>"$TEST_ROOT/docker-foreground.err") || {
    cat "$TEST_ROOT/docker-foreground.err" >&2
    return 1
  }
  grep -q '^docker-foreground-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }
  # Docker parity: foreground run prints only the process output, no id line.
  if grep -Eq '^[0-9a-f]{12}$|^[0-9a-f]{64}$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi

  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo docker-detached-hidden; sleep 0.1' 2>"$TEST_ROOT/docker-detached.err") || {
    cat "$TEST_ROOT/docker-detached.err" >&2
    return 1
  }
  # Docker parity: detach prints the full 64-hex id alone.
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  if grep -q '^docker-detached-hidden$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}

test_docker_unsupported_options_fail_loudly() {
  local rc
  "$HOLD_BIN" --detach-keys ctrl-p,ctrl-q true >/dev/null 2>"$TEST_ROOT/docker-detach-keys-default.err" || {
    cat "$TEST_ROOT/docker-detach-keys-default.err" >&2
    return 1
  }

  "$HOLD_BIN" --detach-keys ctrl-a true >/dev/null 2>"$TEST_ROOT/docker-detach-keys-custom.err" || {
    cat "$TEST_ROOT/docker-detach-keys-custom.err" >&2
    return 1
  }

  set +e
  "$HOLD_BIN" --detach-keys ctrl-not-a-key true >/dev/null 2>"$TEST_ROOT/docker-detach-keys-invalid.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "invalid --detach-keys: rc=$rc (want 5)" >&2; return 1; }
  grep -q "invalid --detach-keys sequence" "$TEST_ROOT/docker-detach-keys-invalid.err" || { cat "$TEST_ROOT/docker-detach-keys-invalid.err" >&2; return 1; }

  "$HOLD_BIN" prune all >/dev/null 2>&1 || true
  "$HOLD_BIN" ps -a >"$TEST_ROOT/docker-unsupported-ps.out" || return 1
  ! grep -q '/true' "$TEST_ROOT/docker-unsupported-ps.out" || { cat "$TEST_ROOT/docker-unsupported-ps.out" >&2; return 1; }
}


test_docker_restart_policy_restarts_failures() {
  local out id count got
  out=$("$HOLD_BIN" -d --restart on-failure:2 --restart-delay 0 -- /bin/sh -c 'mkdir -p "$HOME"; echo attempt >> "$HOME/restart-count"; c=$(wc -l < "$HOME/restart-count"); c=$((c)); echo restart-attempt-$c; [ "$c" -lt 3 ] && exit 7 || sleep 1' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  for _ in $(seq 1 30); do
    count=$(wc -l <"$HOME/restart-count" 2>/dev/null || echo 0)
    [ "$count" -ge 3 ] && break
    sleep 0.1
  done
  count=$(wc -l <"$HOME/restart-count" 2>/dev/null || echo 0)
  [ "$count" -eq 3 ] || { echo "restart attempts: $count" >&2; "$HOLD_BIN" logs "$id" --plain -n 50 >&2 || true; return 1; }
  got=$("$HOLD_BIN" logs "$id" --plain -n 50) || return 1
  printf '%s\n' "$got" | grep -q 'restart-attempt-1' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | grep -q 'restart-attempt-2' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | grep -q 'restart-attempt-3' || { printf '%s\n' "$got" >&2; return 1; }
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/restart-run.json" || return 1
  grep -q '"restart": "on-failure:2"' "$TEST_ROOT/restart-run.json" || { cat "$TEST_ROOT/restart-run.json" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
}

test_purge_never_follows_stored_paths() {
  local out id store json victim
  store="$HOME/.local/state/hold"
  victim="$TEST_ROOT/victim.file"
  printf 'victim' >"$victim"
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo x' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  json=$(record_path "$id" "$store") || return 1
  local full log
  full=$(basename "$json" .json)
  log="$store/$full.log"
  [ -f "$log" ] || return 1
  # Point the record's log_path at an innocent file; purge must derive its
  # unlink targets from the layout and warn, never follow the stored string.
  sed -i.bak "s|\"log_path\": \"[^\"]*\"|\"log_path\": \"$victim\"|" "$json" && rm -f "$json.bak"
  "$HOLD_BIN" purge "$id" >/dev/null 2>"$TEST_ROOT/purge-derive.err" || { cat "$TEST_ROOT/purge-derive.err" >&2; return 1; }
  [ -f "$victim" ] || { echo "purge followed a stored path and deleted the victim" >&2; return 1; }
  [ ! -f "$log" ] || { echo "purge left the store-resident log behind" >&2; return 1; }
  grep -q 'outside the store; left untouched' "$TEST_ROOT/purge-derive.err" || { cat "$TEST_ROOT/purge-derive.err" >&2; return 1; }
}

test_usage_errors_go_to_stderr_and_help_names_both_views() {
  local rc out err
  set +e
  out=$("$HOLD_BIN" -d 2>"$TEST_ROOT/flags-only.err")
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "flags-only rc=$rc want 5" >&2; return 1; }
  [ -z "$out" ] || { echo "usage error leaked to stdout" >&2; printf '%s\n' "$out" >&2; return 1; }
  grep -q '^usage: hold' "$TEST_ROOT/flags-only.err" || { cat "$TEST_ROOT/flags-only.err" >&2; return 1; }
  out=$("$HOLD_BIN" 2>/dev/null)
  printf '%s\n' "$out" | grep -q 'hold ps ' || { echo "help missing a distinct hold ps line" >&2; return 1; }
  printf '%s\n' "$out" | grep -q 'ps is an alias' && { echo "help still claims ps is an alias" >&2; return 1; }
  return 0
}

test_crash_leftover_tmp_does_not_block_rewrites() {
  local out id store
  store="$HOME/.local/state/hold"
  out=$("$HOLD_BIN" -d -- sleep 30 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  local json full
  json=$(record_path "$id" "$store") || return 1
  full=$(basename "$json" .json)
  # A crashed writer's legacy fixed-name tomb must not block future rewrites.
  : >"$store/.$full.tmp"
  "$HOLD_BIN" rename "$id" tombproof >/dev/null 2>&1 || { echo "rename blocked by leftover tmp" >&2; return 1; }
  grep -q '"name": "tombproof"' "$json" || { cat "$json" >&2; return 1; }
  "$HOLD_BIN" kill "$id" >/dev/null 2>&1 || true
}

test_purge_sweep_respects_creation_reserve() {
  local store fid
  store="$HOME/.local/state/hold"
  ensure_user_fixture_store || return 1
  fid=$(printf '1234567890abcdef%.0s' 1 2 3 4)
  echo orphan >"$store/$fid.log"
  : >"$store/.$fid.reserve"
  "$HOLD_BIN" purge >/dev/null 2>&1 || true
  [ -f "$store/$fid.log" ] || { echo "sweep ate a mid-creation log" >&2; return 1; }
  command -v python3 >/dev/null 2>&1 || { rm -f "$store/$fid.log" "$store/.$fid.reserve"; return 0; }
  # A stale reserve is a dead start's litter: the orphan collection proceeds.
  python3 -c 'import os,sys,time; os.utime(sys.argv[1], (time.time()-1200,)*2)' "$store/.$fid.reserve"
  "$HOLD_BIN" purge >/dev/null 2>&1 || true
  [ ! -f "$store/$fid.log" ] || { echo "sweep kept an orphan log after the reserve aged out" >&2; return 1; }
  [ ! -f "$store/.$fid.reserve" ] || { echo "stale reserve not collected" >&2; return 1; }
}

test_logs_plain_dumps_whole_log_and_tail_is_last_n() {
  local out id got
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'seq 1 200' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record_ended_soon "$id" || return 1
  got=$("$HOLD_BIN" logs "$id" --plain | wc -l)
  [ "$got" -eq 200 ] || { echo "plain dump lines=$got want 200" >&2; return 1; }
  got=$("$HOLD_BIN" logs "$id" --plain -n 10)
  [ "$(printf '%s\n' "$got" | wc -l)" -eq 10 ] || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | head -n1 | grep -qx '191' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | tail -n1 | grep -qx '200' || { printf '%s\n' "$got" >&2; return 1; }
  # Docker's --tail composes with --follow: last N, then the live stream
  # (which for an ended call is nothing).
  got=$("$HOLD_BIN" logs "$id" --plain -f -n 5)
  [ "$(printf '%s\n' "$got" | wc -l)" -eq 5 ] || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | head -n1 | grep -qx '196' || { printf '%s\n' "$got" >&2; return 1; }
}

test_docker_foreground_propagates_exit_code() {
  local rc
  set +e
  "$HOLD_BIN" -- /bin/sh -c 'exit 7' >/dev/null 2>"$TEST_ROOT/fg-exit.err"
  rc=$?
  set -e
  [ "$rc" -eq 7 ] || { echo "foreground rc=$rc, want 7" >&2; cat "$TEST_ROOT/fg-exit.err" >&2; return 1; }
  set +e
  "$HOLD_BIN" --rm -- /bin/sh -c 'exit 5' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "--rm foreground rc=$rc, want 5" >&2; return 1; }
  "$HOLD_BIN" -- true >/dev/null 2>&1 || { echo "clean foreground exit not 0" >&2; return 1; }
}

test_restart_final_status_recorded() {
  local out id record
  out=$("$HOLD_BIN" -d --restart on-failure:1 --restart-delay 0 -- /bin/sh -c 'exit 3' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record_ended_soon "$id" || { echo "restart call never marked exited" >&2; return 1; }
  record=$(record_path "$id") || return 1
  grep -q '"exit_code": 3' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" list -a | grep -F "Exited (3)" >/dev/null || { "$HOLD_BIN" list -a >&2; return 1; }
}

test_end_restart_call_records_term_status() {
  local out id record
  out=$("$HOLD_BIN" -d --restart always --restart-delay 0 -- sleep 30 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.3
  "$HOLD_BIN" end "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || { echo "ended restart call never marked exited" >&2; return 1; }
  record=$(record_path "$id") || return 1
  grep -q '"term_signal": ' "$record" || { cat "$record" >&2; return 1; }
}

test_unwitnessed_death_renders_exited_unknown() {
  local out id record pid ppid tries
  out=$("$HOLD_BIN" -d -- sleep 30 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id") || return 1
  pid=$(sed -n 's/.*"pid": \([0-9]*\).*/\1/p' "$record" | head -n1)
  [ -n "$pid" ] || { cat "$record" >&2; return 1; }
  ppid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')
  [ -n "$ppid" ] || return 1
  # Kill the waiter first so no final frame is ever written, then the call.
  kill -9 "$ppid" 2>/dev/null || true
  kill -9 "-$pid" 2>/dev/null || true
  for tries in $(seq 1 40); do
    "$HOLD_BIN" list -a | grep -F "Exited (?)" >/dev/null && return 0
    sleep 0.05
  done
  "$HOLD_BIN" list -a >&2
  cat "$record" >&2
  return 1
}

test_docker_restart_validation_and_tty_gate() {
  local rc
  set +e
  "$HOLD_BIN" --restart nonsense true >/dev/null 2>"$TEST_ROOT/restart-invalid.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "invalid restart rc=$rc" >&2; return 1; }
  grep -q 'invalid --restart policy' "$TEST_ROOT/restart-invalid.err" || { cat "$TEST_ROOT/restart-invalid.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" --restart-delay 1 true >/dev/null 2>"$TEST_ROOT/restart-delay-alone.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "restart-delay without restart rc=$rc" >&2; return 1; }
  grep -q -- '--restart-delay requires --restart' "$TEST_ROOT/restart-delay-alone.err" || { cat "$TEST_ROOT/restart-delay-alone.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" -t --restart always true >/dev/null 2>"$TEST_ROOT/restart-tty.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "restart tty rc=$rc" >&2; return 1; }
  grep -q -- '--restart with --tty/-t is not supported yet' "$TEST_ROOT/restart-tty.err" || { cat "$TEST_ROOT/restart-tty.err" >&2; return 1; }
}


test_start_existing_run_appends_retained_log() {
  local out id log before_count after_count restart_out
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo restart-append-marker' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  log=$(log_path "$id") || return 1
  for _ in $(seq 1 30); do
    before_count=$(grep -c '^restart-append-marker$' "$log" 2>/dev/null || true)
    [ "$before_count" -ge 1 ] && break
    sleep 0.1
  done
  [ "${before_count:-0}" -ge 1 ] || { cat "$log" >&2; return 1; }
  restart_out=$("$HOLD_BIN" -d "$id" 2>&1) || {
    printf '%s\n' "$restart_out" >&2
    return 1
  }
  printf '%s\n' "$restart_out" | grep -q "${id:0:12}" || { printf '%s\n' "$restart_out" >&2; return 1; }
  for _ in $(seq 1 30); do
    after_count=$(grep -c '^restart-append-marker$' "$log" 2>/dev/null || true)
    [ "$after_count" -ge 2 ] && break
    sleep 0.1
  done
  [ "${after_count:-0}" -ge 2 ] || { cat "$log" >&2; return 1; }
}


test_docker_publish_and_volume_rejected() {
  local rc
  set +e
  "$HOLD_BIN" -p 8080:3000 -- true >"$TEST_ROOT/docker-publish.out" 2>"$TEST_ROOT/docker-publish.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/docker-publish.out" "$TEST_ROOT/docker-publish.err" >&2; return 1; }
  grep -q 'does not publish or forward ports' "$TEST_ROOT/docker-publish.err" || { cat "$TEST_ROOT/docker-publish.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" -v "$TEST_ROOT:/work" -- true >"$TEST_ROOT/docker-volume.out" 2>"$TEST_ROOT/docker-volume.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/docker-volume.out" "$TEST_ROOT/docker-volume.err" >&2; return 1; }
  grep -q 'does not mount or remap volumes' "$TEST_ROOT/docker-volume.err" || { cat "$TEST_ROOT/docker-volume.err" >&2; return 1; }
}


test_docker_interactive_stdin_pipes_to_child() {
  local out id
  out=$(printf 'stdin-i-ok\n' | "$HOLD_BIN" -i -f -- /bin/cat 2>"$TEST_ROOT/docker-i.err") || {
    cat "$TEST_ROOT/docker-i.err" >&2
    return 1
  }
  grep -q '^stdin-i-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }

  out=$(printf 'stdin-no-i\n' | "$HOLD_BIN" -f -- /bin/cat 2>"$TEST_ROOT/docker-no-i.err") || {
    cat "$TEST_ROOT/docker-no-i.err" >&2
    return 1
  }
  if grep -q '^stdin-no-i$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}

test_docker_tty_foreground_attaches_and_detaches() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local id rc
  cat >"$TEST_ROOT/docker-tty-child.sh" <<'EOF'
#!/bin/sh
IFS= read line
printf 'tty:%s\n' "$line"
sleep 30
EOF
  chmod +x "$TEST_ROOT/docker-tty-child.sh"
  set +e
  python3 - <<'PY' | pty_run "$HOLD_BIN -it $TEST_ROOT/docker-tty-child.sh" >"$TEST_ROOT/docker-tty.out" 2>"$TEST_ROOT/docker-tty.err"
import sys, time
out = sys.stdout.buffer
time.sleep(0.5)
out.write(b"hello-tty\n")
out.flush()
time.sleep(0.5)
out.write(b"\x10\x11")
out.flush()
time.sleep(0.1)
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "run -it rc=$rc" >&2; cat "$TEST_ROOT/docker-tty.out" "$TEST_ROOT/docker-tty.err" >&2; return 1; }
  grep -q 'tty:hello-tty' "$TEST_ROOT/docker-tty.out" || { cat "$TEST_ROOT/docker-tty.out" >&2; return 1; }
  # Docker parity: foreground -it prints no id; the detached run is found via listing.
  # The lone running call is the detached -it child; take the first table row's
  # CALL ID (the Docker-shaped COMMAND column ellipsizes the long temp path).
  id=$("$HOLD_BIN" list | awk 'NR==2 {print $1; exit}')
  [ -n "$id" ] || { "$HOLD_BIN" list >&2; return 1; }
  record=$(record_path "$id") || { find "$HOME/.local/state/hold" -maxdepth 1 -type f -print >&2 || true; return 1; }
  grep -q '"console_sock": "' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps >"$TEST_ROOT/docker-tty-ps.out" || return 1
  grep -Eq "^$id[[:space:]]+.*Up " "$TEST_ROOT/docker-tty-ps.out" || { cat "$TEST_ROOT/docker-tty-ps.out" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_docker_tty_custom_detach_keys() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local id rc
  cat >"$TEST_ROOT/docker-tty-custom-child.sh" <<'EOF'
#!/bin/sh
printf 'custom-ready\n'
IFS= read line
printf 'custom-line:%s\n' "$line"
sleep 30
EOF
  chmod +x "$TEST_ROOT/docker-tty-custom-child.sh"
  set +e
  python3 - <<'PY' | pty_run "$HOLD_BIN -it --detach-keys ctrl-a $TEST_ROOT/docker-tty-custom-child.sh" >"$TEST_ROOT/docker-tty-custom.out" 2>"$TEST_ROOT/docker-tty-custom.err"
import sys, time
out = sys.stdout.buffer
time.sleep(0.5)
out.write(b"\x01")
out.flush()
time.sleep(0.1)
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "run -it custom detach rc=$rc" >&2; cat "$TEST_ROOT/docker-tty-custom.out" "$TEST_ROOT/docker-tty-custom.err" >&2; return 1; }
  grep -q 'custom-ready' "$TEST_ROOT/docker-tty-custom.out" || { cat "$TEST_ROOT/docker-tty-custom.out" >&2; return 1; }
  ! grep -q 'custom-line:' "$TEST_ROOT/docker-tty-custom.out" || { cat "$TEST_ROOT/docker-tty-custom.out" >&2; return 1; }
  # Docker parity: foreground -it prints no id; the detached run is found via listing.
  # The lone running call is the detached -it child; take the first table row's
  # CALL ID (the Docker-shaped COMMAND column ellipsizes the long temp path).
  id=$("$HOLD_BIN" list | awk 'NR==2 {print $1; exit}')
  [ -n "$id" ] || { "$HOLD_BIN" list >&2; return 1; }
  "$HOLD_BIN" ps >"$TEST_ROOT/docker-tty-custom-ps.out" || return 1
  grep -Eq "^$id[[:space:]]+.*Up " "$TEST_ROOT/docker-tty-custom-ps.out" || { cat "$TEST_ROOT/docker-tty-custom-ps.out" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}


test_build_artifact_coexistence() {
  make clean >/dev/null || return 1
  make hold hold STATIC_LDFLAGS= EXTRA_CPPFLAGS=-DHOLD_TESTING >/dev/null || return 1
  [ -x ./hold ] && [ -x ./hold ] || return 1
  make hold-dynamic EXTRA_CPPFLAGS=-DHOLD_TESTING >/dev/null || return 1
  [ -x ./hold ] && [ -x ./hold ] && [ -x ./hold-dynamic ] || return 1
  [ -e ./hold ] && [ -e ./hold ] && [ -e ./hold-dynamic ]
}

test_concurrent_unique_ids() {
  local i id ids uniq
  ids=""
  for i in $(seq 1 20); do
    "$HOLD_BIN" -d sleep 60 >"$TEST_ROOT/start.$i.out" 2>"$TEST_ROOT/start.$i.err" &
  done
  wait
  for i in $(seq 1 20); do
    id=$(extract_id <"$TEST_ROOT/start.$i.out")
    [ -n "$id" ] || return 1
    ids="$ids\n$id"
  done
  uniq=$(printf '%b\n' "$ids" | sed '/^$/d' | sort -u | wc -l)
  [ "$uniq" -eq 20 ] || return 1
  for id in $(printf '%b\n' "$ids" | sed '/^$/d'); do
    "$HOLD_BIN" kill "$id" >/dev/null 2>&1 || true
  done
  return 0
}

set -e
# ---------------------------------------------------------------------------
# Security-invariant enforcement tests (audit hardening). Each fails loudly if
# the corresponding privilege/privacy invariant regresses, rather than only
# exercising the happy path.
# ---------------------------------------------------------------------------

# Run the binary as the unprivileged user with spoofed sudo provenance injected
# AFTER any privilege drop, so even the root-runner can test the euid gate.
as_user_spoof_sudo() {
  local args
  args=(
    "HOME=$ACTOR_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "PATH=$PATH"
    "SUDO_UID=0" "SUDO_GID=0" "SUDO_USER=root"
  )
  [ "${HOLD_BOOT_ID_PATH+x}" = x ] && args+=("HOLD_BOOT_ID_PATH=$HOLD_BOOT_ID_PATH")
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    "$SUDO_BIN" -n -u "$TEST_USER" env "${args[@]}" "$@"
  else
    env "${args[@]}" "$@"
  fi
}

root_safe_hold_copy() {
  local name="$1" dir path
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 1
  ROOT_SAFE_HOLD_DIR="/usr/local/lib/hold-test-$$"
  dir="$ROOT_SAFE_HOLD_DIR/$(basename "$TEST_ROOT")"
  path="$dir/$name"
  as_root mkdir -p "$dir" || return 1
  as_root chown 0:0 "$ROOT_SAFE_HOLD_DIR" "$dir" || return 1
  as_root chmod 755 "$ROOT_SAFE_HOLD_DIR" "$dir" || return 1
  as_root cp "$HOLD_REAL_BIN" "$path" || return 1
  as_root chown 0:0 "$path" || return 1
  as_root chmod 755 "$path" || return 1
  printf '%s\n' "$path"
}

test_system_store_directory_modes() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id d mode
  id=$(as_root "$HOLD_REAL_BIN" -d true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  for d in runs logs console; do
    mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR/$d") || return 1
    [ "$mode" = 700 ] || { echo "$d/ mode=$mode (want 700 -- private state would be world-visible)" >&2; return 1; }
  done
  mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR") || return 1
  [ "$mode" = 755 ] || { echo "base mode=$mode (want 755)" >&2; return 1; }
  mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR/public") || return 1
  [ "$mode" = 755 ]
}

test_system_store_tightens_preexisting_loose_dir() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id mode
  as_root mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  as_root chmod 0777 "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  id=$(as_root "$HOLD_REAL_BIN" -d true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  [ "$mode" = 700 ] || { echo "pre-existing loose runs/ not tightened: mode=$mode" >&2; return 1; }
}

test_system_store_refuses_symlinked_critical_dirs() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local old_store name root state target rc mode
  old_store="$HOLD_TEST_SYSTEM_STATE_DIR"
  for name in base runs logs console public; do
    root="$TEST_ROOT/syslink-$name"
    state="$root/state"
    target="$root/target"
    as_root mkdir -p "$root" "$target" || return 1
    as_root chmod 0777 "$target" || return 1
    if [ "$name" = base ]; then
      as_root rm -rf "$state" || return 1
      as_root ln -s "$target" "$state" || return 1
    else
      as_root mkdir -p "$state" || return 1
      case "$name" in
        public) as_root mkdir -p "$state/runs" "$state/logs" "$state/console" || return 1 ;;
        console) as_root mkdir -p "$state/runs" "$state/logs" || return 1 ;;
        logs) as_root mkdir -p "$state/runs" || return 1 ;;
      esac
      as_root ln -s "$target" "$state/$name" || return 1
    fi
    HOLD_TEST_SYSTEM_STATE_DIR="$state"
    export HOLD_TEST_SYSTEM_STATE_DIR
    set +e
    as_root "$HOLD_REAL_BIN" -d true >/dev/null 2>"$TEST_ROOT/syslink-$name.err"
    rc=$?
    set -e
    HOLD_TEST_SYSTEM_STATE_DIR="$old_store"
    export HOLD_TEST_SYSTEM_STATE_DIR
    [ "$rc" -ne 0 ] || { echo "system store accepted symlinked $name directory" >&2; return 1; }
    mode=$(root_file_mode "$target") || return 1
    [ "$mode" = 777 ] || { echo "symlink target for $name was chmod-followed: mode=$mode" >&2; return 1; }
  done
}

test_system_store_artifacts_owned_by_root() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id owner record log
  id=$(as_root "$HOLD_REAL_BIN" -d true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  record=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  log=$(root_log_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs") || return 1
  owner=$(root_file_owner "$record") || return 1
  [ "$owner" = "0:0" ] || { echo "record owner=$owner (want 0:0)" >&2; return 1; }
  owner=$(root_file_owner "$log") || return 1
  [ "$owner" = "0:0" ] || { echo "log owner=$owner (want 0:0)" >&2; return 1; }
}

test_nonroot_ignores_spoofed_sudo_provenance() {
  local out id json
  out=$(as_user_spoof_sudo "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(record_path "$id" "$ACTOR_HOME/.local/state/hold") || true
  [ -e "$json" ] || { echo "spoofed-SUDO run did not land in the user store" >&2; return 1; }
  ! root_record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || { echo "spoofed SUDO_* escalated into the system store" >&2; return 1; }
  if grep -q '"invoked_via_sudo": true' "$json" 2>/dev/null; then
    echo "spoofed SUDO_* was trusted by a non-root process" >&2
    return 1
  fi
}


test_signal_refuses_tampered_pgid() {
  local out id rec rc
  out=$("$HOLD_BIN" -d /bin/sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  rec=$(record_path "$id") || return 1
  # A record whose pgid is tampered to <=1 must never be signaled -- kill(-pgid)
  # with pgid<=1 would hit pid 1 / the whole session. valid_record rejects it, so
  # stop refuses with exit 5 and the real process is left untouched.
  sed -i.bak 's/"pgid":[[:space:]]*[0-9][0-9]*/"pgid":1/' "$rec" || return 1
  set +e; "$HOLD_BIN" stop "$id" >/dev/null 2>&1; rc=$?; set -e
  mv "$rec.bak" "$rec" 2>/dev/null || true
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  [ "$rc" -eq 5 ] || { echo "stop on tampered pgid<=1: rc=$rc (want 5 refused)" >&2; return 1; }
}

test_public_index_write_rollback() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local rc pids leftover
  set +e
  HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE=1 as_root "$HOLD_REAL_BIN" -d bash -c 'exec -a hold_pubidx_test sleep 60' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "public-index rollback start: rc=$rc (want 1)" >&2; return 1; }
  # The rollback SIGKILLs the group; poll for its disappearance instead of
  # sampling one instant. A genuine leak survives the whole window.
  local rb_tries=0
  while :; do
    pids=$(ps -eo pid=,args= | awk '/hold_pubidx_test/ && !/awk/ {print $1}')
    [ -z "$pids" ] && break
    rb_tries=$((rb_tries + 1))
    [ "$rb_tries" -lt 20 ] || { echo "leaked process after public-index rollback" >&2; return 1; }
    sleep 0.1
  done
  leftover=$(as_root sh -c 'ls "$1"/runs/*.json "$1"/public/*.json 2>/dev/null' sh "$HOLD_TEST_SYSTEM_STATE_DIR" 2>/dev/null || true)
  [ -z "$leftover" ] || { echo "leftover state after rollback: $leftover" >&2; return 1; }
}

test_quiet_suppresses_banner_keeps_id() {
  local out id
  out=$("$HOLD_BIN" --quiet -d /bin/sleep 300 2>"$TEST_ROOT/q.err") || return 1
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  [ -n "$id" ] || { echo "no id from --quiet start" >&2; return 1; }
  [ "$out" = "$id" ] || { echo "--quiet stdout is not the bare 64-hex id: [$out]" >&2; return 1; }
  [ ! -s "$TEST_ROOT/q.err" ] || { echo "--quiet start wrote to stderr:" >&2; cat "$TEST_ROOT/q.err" >&2; return 1; }
  "$HOLD_BIN" --quiet stop "$id" >/dev/null 2>"$TEST_ROOT/q2.err" || return 1
  [ ! -s "$TEST_ROOT/q2.err" ] || { echo "--quiet stop wrote to stderr" >&2; return 1; }
}

test_run_id_prefix_resolution() {
  local out id pgid pfx got
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  pfx=$(printf '%s' "$id" | cut -c1-4)
  got=$("$HOLD_BIN" stop --print "$pfx") || { echo "id-prefix did not resolve" >&2; return 1; }
  [ "$got" = "kill -TERM -- -$pgid" ] || { echo "prefix resolved to wrong run: [$got]" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
}


test_misc_action_guards() {
  local rc
  set +e; "$HOLD_BIN" --console end deadbeef >/dev/null 2>"$TEST_ROOT/c.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--console end: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'only when launching a call' "$TEST_ROOT/c.err" || { cat "$TEST_ROOT/c.err" >&2; return 1; }
  set +e; "$HOLD_BIN" help bogustopic >/dev/null 2>"$TEST_ROOT/h.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "help bogustopic: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'unknown help topic' "$TEST_ROOT/h.err" || { cat "$TEST_ROOT/h.err" >&2; return 1; }
  # -l is now the --live filter, not the legacy --iso surface: list renders only
  # the Docker-shaped table, never the old STARTED_AT ISO header.
  "$HOLD_BIN" list -l >"$TEST_ROOT/l.out" 2>&1 || true
  ! grep -q 'STARTED_AT' "$TEST_ROOT/l.out" || { echo "list still emits the legacy ISO header" >&2; cat "$TEST_ROOT/l.out" >&2; return 1; }
}


test_owned_command_exact_arity() {
  local rc
  set +e; "$HOLD_BIN" tail deadbeef extra >/dev/null 2>"$TEST_ROOT/tail-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "tail extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold tail <target>' "$TEST_ROOT/tail-extra.err" || { cat "$TEST_ROOT/tail-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" console deadbeef extra >/dev/null 2>"$TEST_ROOT/console-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "console extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold console <target>' "$TEST_ROOT/console-extra.err" || { cat "$TEST_ROOT/console-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" purge deadbeef extra >/dev/null 2>"$TEST_ROOT/prune-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "purge extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold purge' "$TEST_ROOT/prune-extra.err" || { cat "$TEST_ROOT/prune-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" tail --all deadbeef >/dev/null 2>"$TEST_ROOT/tail-all.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "tail --all: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold tail <target>' "$TEST_ROOT/tail-all.err" || { cat "$TEST_ROOT/tail-all.err" >&2; return 1; }

  "$HOLD_BIN" prune >/dev/null || { echo "prune with zero targets should remain valid" >&2; return 1; }
}


test_print_over_all_and_multiple() {
  local id1 id2 pgid1 pgid2 out
  id1=$("$HOLD_BIN" -d sleep 60 2>&1 | extract_id) || return 1
  id2=$("$HOLD_BIN" -d sleep 60 2>&1 | extract_id) || return 1
  pgid1=$(record_pgid "$id1"); pgid2=$(record_pgid "$id2")
  [ -n "$pgid1" ] && [ -n "$pgid2" ] || return 1
  out=$("$HOLD_BIN" stop --print "$id1" "$id2") || return 1
  printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid1" && printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid2" || { echo "explicit multi --print missing a pgid" >&2; return 1; }
  "$HOLD_BIN" stop "$id1" "$id2" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# P1 contract-gap pins (reconstruction audit 2026-07-20). Each test pins a
# documented identity/SPEC behavior that previously had zero coverage.
# ---------------------------------------------------------------------------

# G1: --env-file feeds the recipe env (blank/comment lines skipped) without
# touching hold's own environment.
test_env_file_populates_recipe_env() {
  local out id store fake envfile record log
  store="$HOME/.local/state/hold"
  fake="$TEST_ROOT/envfile-home"
  mkdir -p "$fake" || return 1
  envfile="$TEST_ROOT/recipe.env"
  cat >"$envfile" <<EOF

# comment lines and blank lines are skipped
HOLD_EF_ONE=alpha
HOME=$fake
HOLD_EF_TWO=beta
EOF
  out=$("$HOLD_BIN" -d --env-file "$envfile" -- /bin/sh -c 'echo "ef:$HOLD_EF_ONE:$HOLD_EF_TWO home:$HOME"' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id" "$store") || { echo "--env-file HOME relocated the record store" >&2; return 1; }
  [ ! -d "$fake/.local/state/hold" ] || { echo "--env-file leaked into hold's own environment" >&2; return 1; }
  record_ended_soon "$id" || return 1
  log=$(log_path "$id" "$store") || return 1
  grep -q "ef:alpha:beta home:$fake" "$log" || { cat "$log" >&2; return 1; }
  # The file's assignments are recipe data in the record; comments are not.
  grep -q '"HOLD_EF_ONE=alpha"' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"HOLD_EF_TWO=beta"' "$record" || { cat "$record" >&2; return 1; }
  ! grep -q 'comment lines' "$record" || { cat "$record" >&2; return 1; }
}

# G9: SPEC keeps one legacy shim — a record carrying only the old `Saved`
# spelling still loads as saved, so sweeps must skip it.
test_legacy_saved_key_still_protects() {
  local store legacy_id victim_id
  store="$HOME/.local/state/hold"
  victim_id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$victim_id" ] || return 1
  record_ended_soon "$victim_id" || return 1
  ensure_user_fixture_store || return 1
  legacy_id=ab34cd56ef780000000000000000000000000000000000000000000000000000
  cat >"$store/$legacy_id.json" <<JSON
{
  "Saved": true,
  "version": 1,
  "id": "$legacy_id",
  "run_id": "$legacy_id",
  "pid": 999999,
  "pgid": 999999,
  "sid": 999999,
  "start_unix_ns": 0,
  "argv": ["/bin/sh", "-c", "legacy-saved"],
  "name": "legacy_saved",
  "state": "exited",
  "exit_code": 0,
  "started_at": "2026-06-28T17:30:47Z",
  "created_at": "2026-06-28T17:30:47Z",
  "ended_at": "2026-06-28T17:30:48Z",
  "uid": $TEST_UID,
  "gid": $TEST_GID,
  "proc_starttime_ticks": 0,
  "exe_dev": 0,
  "exe_ino": 0
}
JSON
  chmod 600 "$store/$legacy_id.json" || return 1
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    chown "$TEST_UID:$TEST_GID" "$store/$legacy_id.json" || return 1
  fi
  "$HOLD_BIN" purge >/dev/null 2>&1 || return 1
  [ -f "$store/$legacy_id.json" ] || { echo "sweep removed a record saved under the legacy Saved spelling" >&2; return 1; }
  ! record_exists "$victim_id" || { echo "sweep kept an unsaved ended call (sweep did not run)" >&2; return 1; }
  # -a (include stale) must also honor the shim.
  "$HOLD_BIN" purge -a >/dev/null 2>&1 || return 1
  [ -f "$store/$legacy_id.json" ] || { echo "purge -a removed a legacy-Saved record" >&2; return 1; }
}

# G10: identity 'end the call politely: TERM, then KILL' — pinned for a plain
# (non-console) call; before this only the console path pinned escalation.
test_end_plain_call_escalates_to_kill() {
  local out id record pgid ok _
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'trap "" TERM HUP; while :; do sleep 30; done' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id") || return 1
  pgid=$(record_pgid "$id") || return 1
  [ -n "$pgid" ] || return 1
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  pgid_terminated "$pgid" || { echo "plain group $pgid survived end escalation" >&2; return 1; }
  record_ended_soon "$id" || { cat "$record" >&2; return 1; }
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 137' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 137:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 9' "$record" || { cat "$record" >&2; return 1; }
}

# G11: SPEC safety invariant — when the leader is gone, signals reach the group
# via the recorded pgid+sid within the same boot, never fail on the dead leader.
test_signal_delivers_to_group_after_leader_exit() {
  local out id pid pgid waiter children p _
  out=$("$HOLD_BIN" -d bash -c 'sleep 300 & sleep 301 & exit 0' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  pgid=$(record_pgid "$id")
  pid=$(sed -n 's/.*"pid": \([0-9]*\).*/\1/p' "$(record_path "$id")" | head -n1)
  [ -n "$pgid" ] && [ -n "$pid" ] || return 1
  sleep 0.3
  children=$(ps -eo pid=,pgid=,args= | awk -v g="$pgid" '$2==g && $1!=g && $3 ~ /^sleep$/ {print $1}')
  [ -n "$children" ] || { echo "no children found in group $pgid" >&2; return 1; }
  # Wait until the leader pid is fully gone (exited and reaped), so end must
  # match the group by its recorded pgid+sid alone. If the waiter still holds
  # the leader as a zombie, remove the waiter so init collects it.
  for _ in $(seq 1 50); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    waiter=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ' || true)
    [ -n "$waiter" ] && [ "$waiter" -gt 1 ] && kill -9 "$waiter" 2>/dev/null || true
    for _ in $(seq 1 50); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
  fi
  ! kill -0 "$pid" 2>/dev/null || { echo "leader $pid never fully reaped" >&2; return 1; }
  "$HOLD_BIN" end "$id" >/dev/null || { echo "end failed against a leader-gone group" >&2; return 1; }
  for p in $children; do
    pid_dead_enough "$p" || { echo "child $p survived end after leader exit" >&2; return 1; }
  done
}

# G14: identity — `hold attach <target>` is the primary spelling (console the
# alias); it round-trips bytes and tees to the log exactly like console.
test_attach_verb_matches_console() {
  local out id store logf out2 id2
  "$HOLD_BIN" help attach >"$TEST_ROOT/attach-help.out" || return 1
  grep -q 'usage: hold attach <target>' "$TEST_ROOT/attach-help.out" || { cat "$TEST_ROOT/attach-help.out" >&2; return 1; }
  out=$("$HOLD_BIN" --console /bin/sh -c 'read line; echo "attach:$line"' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  printf 'ping\n' | "$HOLD_BIN" attach "$id" >"$TEST_ROOT/attach.out" 2>"$TEST_ROOT/attach.err" || {
    cat "$TEST_ROOT/attach.out" "$TEST_ROOT/attach.err" >&2
    return 1
  }
  grep -q 'attach:ping' "$TEST_ROOT/attach.out" || { cat "$TEST_ROOT/attach.out" >&2; return 1; }
  sleep 0.2
  logf=$(log_path "$id" "$store") || return 1
  grep -q 'attach:ping' "$logf" || { cat "$logf" >&2; return 1; }
  # attach on a plain call reports 'has no console', like console.
  out2=$("$HOLD_BIN" -d sleep 30 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id2" ] || return 1
  "$HOLD_BIN" attach "$id2" >/dev/null 2>"$TEST_ROOT/attach-none.err" || return 1
  grep -q 'has no console' "$TEST_ROOT/attach-none.err" || { cat "$TEST_ROOT/attach-none.err" >&2; return 1; }
  "$HOLD_BIN" stop "$id2" >/dev/null || return 1
}

# G19: identity — 'plain output automatically when not a TTY': bare hold logs
# piped away dumps the raw log bytes, no viewer chrome, rc 0.
test_logs_non_tty_auto_plain() {
  local out id
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'printf "auto-plain-1\nauto-plain-2\n"' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record_ended_soon "$id" || return 1
  "$HOLD_BIN" logs "$id" </dev/null >"$TEST_ROOT/auto-plain.out" 2>"$TEST_ROOT/auto-plain.err" || {
    cat "$TEST_ROOT/auto-plain.err" >&2
    return 1
  }
  printf 'auto-plain-1\nauto-plain-2\n' >"$TEST_ROOT/auto-plain.want"
  diff -u "$TEST_ROOT/auto-plain.want" "$TEST_ROOT/auto-plain.out" || { echo "non-TTY logs output is not the raw log bytes" >&2; return 1; }
}

# G24: identity — `hold list -u/--user`: your personal calls only, even under
# sudo (the invoking user's store, never root's), no global/redacted rows.
test_list_user_scope_only_personal() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  local uout uid_call gout gid_call
  uout=$("$HOLD_BIN" -d --name personal_scope sleep 300 2>&1) || return 1
  uid_call=$(printf '%s\n' "$uout" | extract_id)
  [ -n "$uid_call" ] || { printf '%s\n' "$uout" >&2; return 1; }
  gout=$(as_root "$HOLD_REAL_BIN" -d --name global_scope sleep 300 2>&1) || return 1
  gid_call=$(printf '%s\n' "$gout" | extract_id)
  [ -n "$gid_call" ] || { printf '%s\n' "$gout" >&2; return 1; }
  # Under sudo, -u lists the invoking user's ledger only.
  as_sudo_from_user "$HOLD_REAL_BIN" list -u >"$TEST_ROOT/sudo-list-u.out" 2>&1 || return 1
  grep -Eq "^$uid_call[[:space:]]+$TEST_USER[[:space:]].*personal_scope$" "$TEST_ROOT/sudo-list-u.out" || { cat "$TEST_ROOT/sudo-list-u.out" >&2; return 1; }
  ! grep -q "$gid_call" "$TEST_ROOT/sudo-list-u.out" || { echo "sudo list -u leaked a global call" >&2; cat "$TEST_ROOT/sudo-list-u.out" >&2; return 1; }
  ! grep -q 'hidden' "$TEST_ROOT/sudo-list-u.out" || { echo "sudo list -u carried redacted rows" >&2; cat "$TEST_ROOT/sudo-list-u.out" >&2; return 1; }
  # Long spelling matches.
  as_sudo_from_user "$HOLD_REAL_BIN" list --user >"$TEST_ROOT/sudo-list-user.out" 2>&1 || return 1
  grep -Eq "^$uid_call[[:space:]]+$TEST_USER[[:space:]].*personal_scope$" "$TEST_ROOT/sudo-list-user.out" || { cat "$TEST_ROOT/sudo-list-user.out" >&2; return 1; }
  # As a plain user, -u excludes the global projection that -a includes.
  "$HOLD_BIN" list -a >"$TEST_ROOT/user-list-a.out" 2>&1 || return 1
  grep -q "$gid_call" "$TEST_ROOT/user-list-a.out" || { echo "list -a fixture missing the global call" >&2; cat "$TEST_ROOT/user-list-a.out" >&2; return 1; }
  "$HOLD_BIN" list -u >"$TEST_ROOT/user-list-u.out" 2>&1 || return 1
  grep -Eq "^$uid_call[[:space:]]" "$TEST_ROOT/user-list-u.out" || { cat "$TEST_ROOT/user-list-u.out" >&2; return 1; }
  ! grep -q "$gid_call" "$TEST_ROOT/user-list-u.out" || { echo "user list -u leaked a global call" >&2; cat "$TEST_ROOT/user-list-u.out" >&2; return 1; }
  as_root "$HOLD_REAL_BIN" stop "system:$gid_call" >/dev/null 2>&1 || true
}

# G25: identity — rm, prune, and drop are accepted purge aliases; drop removes
# record + log exactly like prune, targeted and sweeping.
test_drop_alias_matches_prune() {
  local out1 out2 id1 id2 store
  out1=$("$HOLD_BIN" -d true 2>&1) || return 1
  out2=$("$HOLD_BIN" -d true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/hold"
  record_ended_soon "$id1" || return 1
  record_ended_soon "$id2" || return 1
  "$HOLD_BIN" help drop >"$TEST_ROOT/drop-help.out" || return 1
  grep -q 'usage: hold purge' "$TEST_ROOT/drop-help.out" || { cat "$TEST_ROOT/drop-help.out" >&2; return 1; }
  "$HOLD_BIN" drop "$id1" >/dev/null || return 1
  ! record_exists "$id1" "$store" && ! log_exists "$id1" "$store" || { echo "drop left artifacts of $id1" >&2; return 1; }
  record_exists "$id2" "$store" && log_exists "$id2" "$store" || { echo "targeted drop touched another call" >&2; return 1; }
  # A bare drop sweeps ended calls, exactly like a bare purge/prune.
  "$HOLD_BIN" drop >/dev/null 2>&1 || return 1
  ! record_exists "$id2" "$store" || { echo "sweeping drop kept an ended call" >&2; return 1; }
}

# G35: SPEC — a public entry whose private record is gone is an orphan;
# a root purge of the global store sweeps it so no view can resurrect it.
test_orphan_public_entry_swept() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  local out id json public_json ok _
  out=$(as_root "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  public_json=$(record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public") || return 1
  ok=0
  for _ in $(seq 1 50); do
    as_root grep -q '"state": "exited"' "$json" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "global call never marked exited" >&2; return 1; }
  # Simulate the orphan: the private record vanishes, the projection stays.
  as_root rm -f "$json" || return 1
  [ -f "$public_json" ] || { echo "public entry disappeared before the sweep" >&2; return 1; }
  as_root "$HOLD_REAL_BIN" purge -s >/dev/null 2>&1 || return 1
  [ ! -e "$public_json" ] || { echo "orphan public entry survived sudo hold purge" >&2; cat "$public_json" >&2; return 1; }
  # No view resurrects it.
  "$HOLD_BIN" list -a >"$TEST_ROOT/orphan-list.out" 2>&1 || return 1
  ! grep -q "^$id" "$TEST_ROOT/orphan-list.out" || { echo "swept orphan still listed" >&2; cat "$TEST_ROOT/orphan-list.out" >&2; return 1; }
}

# G37: parity — `--restart unless-stopped` restarts crashes, but `hold end`
# stops the loop for good and records the term status.
test_restart_unless_stopped() {
  local out id count record _
  out=$("$HOLD_BIN" -d --restart unless-stopped --restart-delay 0 -- /bin/sh -c 'mkdir -p "$HOME"; echo attempt >> "$HOME/us-count"; c=$(wc -l < "$HOME/us-count"); c=$((c)); [ "$c" -lt 3 ] && exit 7; sleep 60' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  for _ in $(seq 1 50); do
    count=$(wc -l <"$HOME/us-count" 2>/dev/null || echo 0)
    count=$((count))
    [ "$count" -ge 3 ] && break
    sleep 0.1
  done
  [ "$count" -eq 3 ] || { echo "unless-stopped attempts: $count (want 3)" >&2; "$HOLD_BIN" logs "$id" --plain -n 50 >&2 || true; return 1; }
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/unless-stopped.json" || return 1
  grep -q '"restart": "unless-stopped"' "$TEST_ROOT/unless-stopped.json" || { cat "$TEST_ROOT/unless-stopped.json" >&2; return 1; }
  # The third attempt is now sleeping; end must bring it down and stay down.
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  record_ended_soon "$id" || { echo "ended unless-stopped call never marked exited" >&2; return 1; }
  record=$(record_path "$id") || return 1
  grep -q '"term_signal": ' "$record" || { cat "$record" >&2; return 1; }
  sleep 1
  count=$(wc -l <"$HOME/us-count" 2>/dev/null || echo 0)
  count=$((count))
  [ "$count" -eq 3 ] || { echo "unless-stopped restarted after hold end: attempts=$count" >&2; return 1; }
  "$HOLD_BIN" list -a | grep -Eq "^$id[[:space:]].*Exited" || { "$HOLD_BIN" list -a >&2; return 1; }
}

# hold msg: a call messages another; the receiver verifies the sender's call,
# labels impostors, and flattens newlines when there is no bracketed paste.
test_msg_between_calls() {
  "$HOLD_BIN" -dt --name lead -- /bin/sh -c 'stty -echo; echo lead-ready; while IFS= read -r l; do printf "LEAD GOT: %s\n" "$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait lead --match lead-ready --timeout 5000 || return 1
  # The sender runs in a new session, as agent harnesses run tool commands:
  # verification must follow ancestry, not just the session.
  if command -v setsid >/dev/null 2>&1; then
    "$HOLD_BIN" -dt --name worker -- /bin/sh -c "setsid \"$HOLD_REAL_BIN\" msg lead 'task done'; sleep 30" >/dev/null || return 1
  else
    "$HOLD_BIN" -dt --name worker -- /bin/sh -c "\"$HOLD_REAL_BIN\" msg lead 'task done'; sleep 30" >/dev/null || return 1
  fi
  "$HOLD_BIN" wait lead --match 'LEAD GOT' --timeout 10000 || { "$HOLD_BIN" peek lead >&2; return 1; }
  "$HOLD_BIN" peek lead | grep -qF 'LEAD GOT: [msg from worker; reply: hold msg worker "..."] task done' || { "$HOLD_BIN" peek lead >&2; return 1; }
  HOLD_CALL_ID= HOLD_CALL_NAME=worker "$HOLD_BIN" msg lead --now 'fake' || return 1
  "$HOLD_BIN" wait lead --match 'fake' --timeout 5000 || return 1
  "$HOLD_BIN" peek lead | grep -qF 'LEAD GOT: [msg from worker (unverified)] fake' || return 1
  env -u HOLD_CALL_ID -u HOLD_CALL_NAME "$HOLD_BIN" msg lead --now "$(printf 'two\nlines')" || return 1
  "$HOLD_BIN" wait lead --match 'two lines' --timeout 5000 || { "$HOLD_BIN" peek lead >&2; return 1; }
  "$HOLD_BIN" peek lead | grep -qF 'LEAD GOT: [msg via hold] two lines' || return 1
  "$HOLD_BIN" end lead worker >/dev/null 2>&1 || true
}

# A TTY call gets a usable TERM when the launcher had none (cron, CI), but an
# explicit -e TERM=... wins and plain calls are left alone.
test_tty_call_defaults_term() {
  # /usr/bin/env directly: some /bin/sh (macOS) set TERM=dumb themselves.
  env -u TERM "$HOLD_BIN" -dt --name t1 -- /usr/bin/env >/dev/null || return 1
  TERM=dumb "$HOLD_BIN" -dt --name t2 -- /usr/bin/env >/dev/null || return 1
  TERM=unknown "$HOLD_BIN" -dt --name t5 -- /usr/bin/env >/dev/null || return 1
  env -u TERM "$HOLD_BIN" -dt -e TERM=vt100 --name t3 -- /usr/bin/env >/dev/null || return 1
  env -u TERM "$HOLD_BIN" -d --name t4 -- /usr/bin/env >/dev/null || return 1
  for c in t1 t2 t3 t4 t5; do "$HOLD_BIN" wait "$c" --timeout 5000 >/dev/null || return 1; done
  "$HOLD_BIN" logs t5 | tr -d '\r' | grep -qx 'TERM=xterm-256color' || return 1
  # logs, not peek: the environment can scroll past the 24-row screen
  "$HOLD_BIN" logs t1 | tr -d '\r' | grep -qx 'TERM=xterm-256color' || { "$HOLD_BIN" logs t1 >&2; return 1; }
  "$HOLD_BIN" logs t2 | tr -d '\r' | grep -qx 'TERM=xterm-256color' || return 1
  "$HOLD_BIN" logs t3 | tr -d '\r' | grep -qx 'TERM=vt100' || return 1
  # A plain (non -t) call is left alone: hold injects nothing.
  ! "$HOLD_BIN" logs t4 | tr -d '\r' | grep -qx 'TERM=xterm-256color' || return 1
}

# Arrow keys and mouse events move a real program's cursor (vim, mouse=a).
test_cursor_control_keys_and_mouse() {
  command -v vim >/dev/null 2>&1 || skip_tool "vim not installed"
  W=$(actor_dir) || return 1
  seq -f 'line %g of a file long enough to click anywhere on it' 1 60 >"$W/f.txt"
  "$HOLD_BIN" -dt --name v -- vim -u NONE -N -i NONE -c 'set mouse=a' -c 'set ttymouse=sgr' "$W/f.txt" >/dev/null || return 1
  "$HOLD_BIN" wait v --idle 400 --timeout 8000 || return 1
  "$HOLD_BIN" peek v --json | grep -q '"mouse": "drag"' || { "$HOLD_BIN" peek v --json >&2; return 1; }
  "$HOLD_BIN" send v -k Down -k Down -k Right -k Right -k Right --wait 200 || return 1
  "$HOLD_BIN" peek v --json | grep -q '"cursor": {"row": 2, "col": 3,' || { "$HOLD_BIN" peek v --json | grep cursor >&2; return 1; }
  "$HOLD_BIN" send v --mouse click,20,10 --wait 200 || return 1
  "$HOLD_BIN" peek v --json | grep -q '"cursor": {"row": 10, "col": 20,' || return 1
  "$HOLD_BIN" send v --mouse press,5,3 --mouse drag,30,5 --mouse release,30,5 --wait 300 || return 1
  "$HOLD_BIN" peek v | tail -1 | grep -q -- '-- VISUAL --' || { "$HOLD_BIN" peek v | tail -2 >&2; return 1; }
  "$HOLD_BIN" send v -k Escape --mouse scroll-down,10,10 --wait 300 || return 1
  "$HOLD_BIN" peek v | head -1 | grep -qv '^line 1 ' || return 1
  "$HOLD_BIN" send v ':q!' --enter || return 1
  "$HOLD_BIN" wait v --timeout 5000 >/dev/null || return 1
}

# Keys arrive as exact xterm bytes, one write per key, in both cursor modes.
test_keys_exact_bytes_one_write_each() {
  cat >"$TEST_ROOT/keybytes.py" <<'PY'
import os, sys, tty, termios
fd = sys.stdin.fileno()
if sys.argv[1] == "app":
    sys.stdout.write("\033[?1h\033=")
sys.stdout.write("keys ready\r\n"); sys.stdout.flush()
tty.setraw(fd)
while True:
    chunk = os.read(fd, 64)
    sys.stdout.write("%s\r\n" % chunk.hex()); sys.stdout.flush()
    if chunk == b"Q": break
PY
  keys="Enter Backspace C-h Tab S-Tab Escape C-a C-c C-z M-x M-Enter Up Home C-Left F1 F12 C-Space"
  want_normal="0d 7f 08 09 1b5b5a 1b 01 03 1a 1b78 1b0d 1b5b41 1b5b48 1b5b313b3544 1b4f50 1b5b32347e 00"
  want_app="0d 7f 08 09 1b5b5a 1b 01 03 1a 1b78 1b0d 1b4f41 1b4f48 1b5b313b3544 1b4f50 1b5b32347e 00"
  for mode in normal app; do
    "$HOLD_BIN" -dt --name "kb$mode" -- python3 "$TEST_ROOT/keybytes.py" "$mode" >/dev/null || return 1
    "$HOLD_BIN" wait "kb$mode" --match 'keys ready' --timeout 5000 || return 1
    for k in $keys; do "$HOLD_BIN" send "kb$mode" -k "$k" || return 1; done
    "$HOLD_BIN" send "kb$mode" Q || return 1
    "$HOLD_BIN" wait "kb$mode" --timeout 5000 >/dev/null || return 1
    got=$("$HOLD_BIN" logs "kb$mode" | tr -d '\r' | sed -n '2,18p' | tr '\n' ' ' | sed 's/ $//')
    if [ "$mode" = normal ]; then want=$want_normal; else want=$want_app; fi
    [ "$got" = "$want" ] || { echo "$mode: got  $got" >&2; echo "$mode: want $want" >&2; return 1; }
  done
}

# A dialog-style form driven by keyboard only: Down, Space, Tab, Tab,
# Shift+Tab back to <Ok>, Enter -> the checklist answers "2".
test_whiptail_form_by_keyboard() {
  command -v whiptail >/dev/null 2>&1 || skip_tool "whiptail not installed"
  "$HOLD_BIN" -dt --name wt -- /bin/sh -c 'r=$(whiptail --checklist "Select options:" 15 40 3 1 "Option A" off 2 "Option B" off 3 "Option C" off 3>&1 1>&2 2>&3); echo "rc=$? RESULT:$r"; sleep 20' >/dev/null || return 1
  "$HOLD_BIN" wait wt --match 'Option C' --timeout 5000 || return 1
  "$HOLD_BIN" wait wt --idle 300 --timeout 3000 || return 1
  "$HOLD_BIN" send wt -k Down -k Space --wait 200 || return 1
  "$HOLD_BIN" send wt -k Tab -k Tab -k S-Tab --wait 200 || return 1
  "$HOLD_BIN" send wt -k Enter || return 1
  "$HOLD_BIN" wait wt --match 'RESULT' --timeout 5000 || return 1
  "$HOLD_BIN" peek wt | grep -qF 'rc=0 RESULT:"2"' || { "$HOLD_BIN" peek wt >&2; return 1; }
  "$HOLD_BIN" end wt >/dev/null 2>&1 || true
}

# The legacy byte mouse encoding cannot reach past cell 222: send says so.
test_send_mouse_legacy_clamp_is_reported() {
  "$HOLD_BIN" -dt --name lm -- /bin/sh -c 'printf "\033[?1000hready\n"; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait lm --match ready --timeout 5000 || return 1
  "$HOLD_BIN" resize lm 300 30 || return 1
  "$HOLD_BIN" send lm --mouse click,100,2 2>"$TEST_ROOT/lm1.err" || return 1
  ! grep -q 'legacy mouse encoding' "$TEST_ROOT/lm1.err" || return 1
  "$HOLD_BIN" send lm --mouse click,250,2 2>"$TEST_ROOT/lm2.err" || return 1
  grep -q 'legacy mouse encoding' "$TEST_ROOT/lm2.err" || { cat "$TEST_ROOT/lm2.err" >&2; return 1; }
  "$HOLD_BIN" end lm >/dev/null 2>&1 || true
}

# tmux under Hold: prefix chord split, mouse pane focus, prefix detach.
test_tmux_prefix_mouse_and_detach() {
  command -v tmux >/dev/null 2>&1 || skip_tool "tmux not installed"
  local sock
  sock="$(actor_dir)/tmux.sock" || return 1
  "$HOLD_BIN" -dt --name tx -- tmux -S "$sock" -f /dev/null new-session \; set -g mouse on >/dev/null || return 1
  "$HOLD_BIN" wait tx --idle 500 --timeout 8000 || return 1
  "$HOLD_BIN" send tx -k C-b --text % --wait 500 || { tmux -S "$sock" kill-server; return 1; }
  [ "$(tmux -S "$sock" list-panes | wc -l | tr -d ' ')" -eq 2 ] || { tmux -S "$sock" kill-server; return 1; }
  [ "$(tmux -S "$sock" display -p '#{pane_index}')" = 1 ] || { tmux -S "$sock" kill-server; return 1; }
  "$HOLD_BIN" send tx --mouse click,5,3 --wait 400 || { tmux -S "$sock" kill-server; return 1; }
  [ "$(tmux -S "$sock" display -p '#{pane_index}')" = 0 ] || { tmux -S "$sock" kill-server; echo "mouse did not focus pane 0" >&2; return 1; }
  "$HOLD_BIN" send tx -k C-b --text d || { tmux -S "$sock" kill-server; return 1; }
  [ "$("$HOLD_BIN" wait tx --timeout 8000)" = 0 ] || { tmux -S "$sock" kill-server; return 1; }
  tmux -S "$sock" kill-server
}

# An ncurses program's own mouse handling: a click toggles a checkbox.
# (Multi-click flows are covered by the vim, tmux, and micro tests; on
# GitHub's runners this python-curses toy registers only its first click.)
test_curses_mouse_checklist() {
  python3 -c 'import curses' 2>/dev/null || skip_tool "python3 curses unavailable"
  cat >"$TEST_ROOT/checklist.py" <<'PY'
import curses, sys, time
items = ["Option A", "Option B", "Option C"]
log = open(sys.argv[1], "w")
def main(scr):
    curses.mousemask(curses.ALL_MOUSE_EVENTS | curses.REPORT_MOUSE_POSITION)
    curses.mouseinterval(0)
    checked = [False] * 3
    pressed = False
    while True:
        scr.erase()
        for i, it in enumerate(items):
            scr.addstr(3 + i, 4, "[%s] %s" % ("X" if checked[i] else " ", it))
        scr.addstr(7, 4, "<  OK  >")
        scr.refresh()
        if scr.getch() != curses.KEY_MOUSE: continue
        try:
            _, x, y, _, b = curses.getmouse()
        except curses.error:
            continue
        log.write("x=%d y=%d bstate=%#x\n" % (x, y, b)); log.flush()
        # ncurses versions differ in which of press/click/release they report
        # (and may merge quick clicks into double/triple clicks); act once per
        # click on whichever arrives first.
        if b & (curses.BUTTON1_PRESSED | curses.BUTTON1_CLICKED | curses.BUTTON1_DOUBLE_CLICKED | curses.BUTTON1_TRIPLE_CLICKED):
            pressed = True
        elif b & curses.BUTTON1_RELEASED:
            if pressed:
                pressed = False
                continue
        else:
            continue
        if 3 <= y <= 5 and 4 <= x <= 16: checked[y - 3] = not checked[y - 3]
        elif y == 7 and 4 <= x <= 11: return " ".join(str(i + 1) for i, c in enumerate(checked) if c)
print("RESULT:" + curses.wrapper(main)); sys.stdout.flush(); time.sleep(20)
PY
  W=$(actor_dir) || return 1
  "$HOLD_BIN" -dt --name cl -- python3 "$TEST_ROOT/checklist.py" "$W/mouse-events.log" >/dev/null || return 1
  "$HOLD_BIN" wait cl --match 'OK' --timeout 15000 || { "$HOLD_BIN" peek cl --json >&2; "$HOLD_BIN" logs cl >&2; return 1; }
  "$HOLD_BIN" wait cl --idle 300 --timeout 5000 || { "$HOLD_BIN" peek cl >&2; return 1; }
  "$HOLD_BIN" send cl --mouse click,8,4 --wait 300 || return 1
  "$HOLD_BIN" wait cl --match '[X] Option B' --timeout 5000 || {
    "$HOLD_BIN" peek cl >&2; echo "mouse events:" >&2; cat "$W/mouse-events.log" >&2; return 1; }
  grep -q 'bstate=' "$W/mouse-events.log" || return 1
  "$HOLD_BIN" end cl >/dev/null 2>&1 || true
}

# micro: click places the cursor, double-click selects a word, drag selects
# across lines, the wheel scrolls.
test_micro_mouse_editing() {
  command -v micro >/dev/null 2>&1 || skip_tool "micro not installed"
  W=$(actor_dir) || return 1
  mkdir -p "$W/mcfg" && { [ "${USER_ACTOR_NEEDS_SUDO:-0}" -ne 1 ] || chown "$TEST_UID:$TEST_GID" "$W/mcfg"; }
  seq -f 'alpha bravo charlie delta %g' 1 80 >"$W/m.txt"
  "$HOLD_BIN" -dt --name mi -- micro -config-dir "$W/mcfg" "$W/m.txt" >/dev/null || return 1
  "$HOLD_BIN" wait mi --match 'delta 5' --timeout 8000 || return 1
  "$HOLD_BIN" wait mi --idle 500 --timeout 5000 || return 1
  "$HOLD_BIN" send mi --mouse click,12,2 --wait 300 || return 1
  "$HOLD_BIN" peek mi --json | grep -q '"cursor": {"row": 2, "col": 12,' || { "$HOLD_BIN" peek mi --json | grep cursor >&2; return 1; }
  "$HOLD_BIN" send mi --mouse click,11,1 --mouse click,11,1 --wait 300 || return 1
  "$HOLD_BIN" send mi -k C-c -k End -k Enter -k C-v --wait 300 || return 1
  "$HOLD_BIN" peek mi | sed -n 3p | grep -qE '^ *3 bravo$' || { "$HOLD_BIN" peek mi | head -4 >&2; return 1; }
  "$HOLD_BIN" send mi --mouse press,9,4 --mouse drag,10,5 --mouse release,10,5 --wait 300 || return 1
  "$HOLD_BIN" send mi -k Delete --wait 300 || return 1
  "$HOLD_BIN" peek mi | grep -qE '^ *5 alpha ravo charlie delta 5$' || { "$HOLD_BIN" peek mi | head -7 >&2; return 1; }
  "$HOLD_BIN" send mi --mouse scroll-down,10,10 --wait 300 || return 1
  "$HOLD_BIN" peek mi | head -1 | grep -qv '^ *1 ' || return 1
  "$HOLD_BIN" end mi >/dev/null 2>&1 || true
}

# wait --idle never passes before the program's first output; and a program
# launched through a symlink (a venv's python) runs under the name it was given.
test_wait_idle_needs_first_output_and_symlink_launch() {
  "$HOLD_BIN" -dt --name slow -- /bin/sh -c 'sleep 1.5; echo drawn-late; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait slow --idle 400 --timeout 8000 || return 1
  "$HOLD_BIN" peek slow | grep -qx 'drawn-late' || { echo "idle passed before first output" >&2; return 1; }
  "$HOLD_BIN" end slow >/dev/null 2>&1 || true
  mkdir -p "$TEST_ROOT/bin"
  printf '#!/bin/sh\necho "argv0=$0"\n' >"$TEST_ROOT/bin/real-tool"
  chmod +x "$TEST_ROOT/bin/real-tool"
  ln -s real-tool "$TEST_ROOT/bin/alias-tool"
  ( cd "$TEST_ROOT" && "$HOLD_BIN" -d --name via-link -- bin/alias-tool ) >/dev/null || return 1
  "$HOLD_BIN" wait via-link --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" logs via-link | grep -q 'argv0=.*/bin/alias-tool$' || { "$HOLD_BIN" logs via-link >&2; return 1; }
  "$HOLD_BIN" inspect via-link | grep -q '"exe": ".*/bin/real-tool"' || return 1
}

# --since makes --match wait for a changed screen: text already showing is
# not a fresh answer.
test_wait_match_since_ignores_stale_screen() {
  "$HOLD_BIN" -dt --name stale -- /bin/sh -c 'stty -echo; echo "BLUE BACKGROUND"; while IFS= read -r l; do echo "again: $l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait stale --match 'BLUE BACKGROUND' --timeout 5000 || return 1
  seq=$("$HOLD_BIN" peek stale --json | sed -n 's/.*"output_seq": \([0-9]*\).*/\1/p')
  set +e
  "$HOLD_BIN" wait stale --match 'BLUE BACKGROUND' --since "$seq" --timeout 600 2>/dev/null; rc=$?
  set -e
  [ "$rc" -eq 7 ] || { echo "stale match passed (rc $rc)" >&2; return 1; }
  seq=$("$HOLD_BIN" send stale --seq x --enter) || return 1
  "$HOLD_BIN" wait stale --match 'BLUE BACKGROUND' --since "$seq" --timeout 5000 || return 1
  "$HOLD_BIN" end stale >/dev/null 2>&1 || true
}

# Screenshot fidelity: CJK and emoji draw real glyphs, not the missing box.
test_peek_image_draws_cjk_and_emoji() {
  "$HOLD_BIN" -dt --name glyphs -- /bin/sh -c 'printf "\347\225\214\n\360\237\232\200\n\356\200\200\n\342\240\277\n"; sleep 20' >/dev/null || return 1
  "$HOLD_BIN" wait glyphs --idle 300 --timeout 5000 || return 1
  W=$(actor_dir) || return 1
  "$HOLD_BIN" peek glyphs --image "$W/g.png" --scale 1 --no-ruler --no-footer || return 1
  python3 - "$W/g.png" <<'PY' || return 1
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", d[16:24]); pos, idat = 8, b""
while pos < len(d):
    n, = struct.unpack(">I", d[pos:pos + 4])
    if d[pos + 4:pos + 8] == b"IDAT": idat += d[pos + 8:pos + 8 + n]
    pos += 12 + n
raw = zlib.decompress(idat); st = w * 3 + 1; px = []
for y in range(h):
    line = bytearray(raw[y * st + 1:(y + 1) * st])
    for i in range(3, len(line)): line[i] = (line[i] + line[i - 3]) & 255
    px.append(line)
def cell(row):  # the 16x16 block of a wide cell at column 0
    return b"".join(bytes(px[row * 16 + y][:16 * 3]) for y in range(16))
cjk, emoji, missing = cell(0), cell(1), cell(2)
assert missing != cjk and missing != emoji, "CJK or emoji drawn as the missing-glyph box"
# Braille U+283F (row 3): its two dot columns must not touch - x=3,4 is a gap.
bg = bytes(px[3 * 16 + 1][0:3])
for y in range(3 * 16, 4 * 16):
    for x in (3, 4):
        assert bytes(px[y][x * 3:x * 3 + 3]) == bg, "braille dot columns touch"
PY
  "$HOLD_BIN" end glyphs >/dev/null 2>&1 || true
}

# msg --settle (global --quiet owns that word) in both spellings; the value
# never leaks into the message.
test_msg_settle_flag() {
  "$HOLD_BIN" -dt --name rx -- /bin/sh -c 'stty -echo; while IFS= read -r l; do echo "GOT[$l]"; done' >/dev/null || return 1
  sleep 0.3
  env -u HOLD_CALL_ID -u HOLD_CALL_NAME "$HOLD_BIN" msg rx "one" --settle 150 || return 1
  env -u HOLD_CALL_ID -u HOLD_CALL_NAME "$HOLD_BIN" msg rx --settle=150 "two" || return 1
  "$HOLD_BIN" wait rx --match '[msg via hold] two]' --timeout 5000 || { "$HOLD_BIN" peek rx >&2; return 1; }
  "$HOLD_BIN" peek rx | grep -qx 'GOT\[\[msg via hold\] one\]' || { "$HOLD_BIN" peek rx >&2; return 1; }
  "$HOLD_BIN" end rx >/dev/null 2>&1 || true
}

# A full-screen program's last frame survives its switch back to the normal
# screen: peek --last-alt, live and after exit.
test_peek_last_alt_frame() {
  "$HOLD_BIN" -dt --name fs -- /bin/sh -c 'printf "\033[?1049hFULLSCREEN FRAME\n"; sleep 0.4; printf "\033[?1049l"; echo back to normal; sleep 1' >/dev/null || return 1
  "$HOLD_BIN" wait fs --match 'back to normal' --timeout 5000 || return 1
  "$HOLD_BIN" peek fs --last-alt | head -1 | grep -qx 'FULLSCREEN FRAME' || { "$HOLD_BIN" peek fs --last-alt >&2; return 1; }
  "$HOLD_BIN" wait fs --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" peek fs 2>"$TEST_ROOT/fs.err" | grep -qx 'back to normal' || return 1
  grep -q -- '--last-alt' "$TEST_ROOT/fs.err" || return 1
  "$HOLD_BIN" peek fs --last-alt | head -1 | grep -qx 'FULLSCREEN FRAME' || return 1
  "$HOLD_BIN" -dt --name plain -- /bin/sh -c 'echo no alt; sleep 0.2' >/dev/null || return 1
  "$HOLD_BIN" wait plain --timeout 5000 >/dev/null || return 1
  set +e; "$HOLD_BIN" peek plain --last-alt >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 5 ] || return 1
}

# ---- the screen verbs: peek / send / wait / resize -------------------------

# A TTY call's screen reads back as plain text, with a ruler, and as JSON.
test_peek_text_ruler_and_json() {
  "$HOLD_BIN" -dt --name scr -- /bin/sh -c 'printf "\033[1;32malpha\033[0m\nbeta\n"; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait scr --match beta --timeout 5000 || return 1
  out=$("$HOLD_BIN" peek scr) || return 1
  [ "$(printf '%s\n' "$out" | sed -n 1p)" = alpha ] || { printf '%s\n' "$out" >&2; return 1; }
  [ "$(printf '%s\n' "$out" | sed -n 2p)" = beta ] || return 1
  [ "$("$HOLD_BIN" peek scr | wc -l | tr -d ' ')" -eq 24 ] || { echo "expected 24 rows" >&2; return 1; }
  "$HOLD_BIN" peek scr --ruler | sed -n 3p | grep -qx ' 0|alpha' || return 1
  "$HOLD_BIN" peek scr --json >"$TEST_ROOT/peek.json" || return 1
  python3 - "$TEST_ROOT/peek.json" <<'PY' || { cat "$TEST_ROOT/peek.json" >&2; return 1; }
import json, sys
d = json.load(open(sys.argv[1]))
assert d["name"] == "scr" and d["running"] is True and d["exit_code"] is None
assert (d["rows"], d["cols"]) == (24, 80)
assert d["cursor"] == {"row": 2, "col": 0, "visible": True}, d["cursor"]
assert d["lines"][:2] == ["alpha", "beta"] and len(d["lines"]) == 24
assert d["output_seq"] > 0 and d["mouse"] == "none" and d["cursor_keys"] == "normal"
PY
  "$HOLD_BIN" end scr >/dev/null 2>&1 || true
}

# send types text and presses keys; --wait returns once the program answered.
test_send_text_keys_and_wait() {
  "$HOLD_BIN" -dt --name typer -- /bin/sh -c 'stty -echo; echo ready; while IFS= read -r l; do printf "got:%s\n" "$l"; done' >/dev/null || return 1
  "$HOLD_BIN" wait typer --match ready --timeout 5000 || return 1
  "$HOLD_BIN" send typer "hello world" --enter --wait 200 --timeout 5000 || return 1
  "$HOLD_BIN" peek typer | grep -qx 'got:hello world' || { "$HOLD_BIN" peek typer >&2; return 1; }
  "$HOLD_BIN" send typer --text --not-a-flag -k Enter --wait 200 || return 1
  "$HOLD_BIN" peek typer | grep -qx 'got:--not-a-flag' || return 1
  set +e
  "$HOLD_BIN" send typer -k NoSuchKey 2>"$TEST_ROOT/badkey.err"; rc=$?
  set -e
  [ "$rc" -eq 1 ] && grep -q "unknown key 'NoSuchKey'" "$TEST_ROOT/badkey.err" || return 1
  "$HOLD_BIN" send typer -k C-d || return 1
  "$HOLD_BIN" wait typer --timeout 5000 | grep -qx 0 || return 1
}

# Keys follow the program's modes: arrows switch to ESC O x under DECCKM.
test_send_keys_follow_cursor_key_mode() {
  "$HOLD_BIN" -dt --name keys -- /bin/sh -c 'printf "\033[?1h"; stty raw -echo; echo ready; dd bs=1 count=3 2>/dev/null | od -An -c; sleep 5' >/dev/null || return 1
  "$HOLD_BIN" wait keys --match ready --timeout 5000 || return 1
  "$HOLD_BIN" send keys -k Up || return 1
  "$HOLD_BIN" wait keys --regex '033 +O +A' --timeout 5000 || { "$HOLD_BIN" peek keys >&2; return 1; }
  "$HOLD_BIN" end keys >/dev/null 2>&1 || true
}

# Bracketed paste wraps the text only when the program enabled mode 2004.
test_send_paste_brackets_when_requested() {
  "$HOLD_BIN" -dt --name paste -- /bin/sh -c 'printf "\033[?2004h"; stty raw -echo; echo ready; dd bs=1 count=15 2>/dev/null | od -An -c; sleep 5' >/dev/null || return 1
  "$HOLD_BIN" wait paste --match ready --timeout 5000 || return 1
  "$HOLD_BIN" send paste --paste 'a b' || return 1
  "$HOLD_BIN" wait paste --regex '033 +\[ +2 +0 +0 +~ +a' --timeout 5000 || { "$HOLD_BIN" peek paste >&2; return 1; }
  "$HOLD_BIN" end paste >/dev/null 2>&1 || true
}

# wait: idle that never comes times out (7); a call that ends first is 8;
# no condition is docker wait (prints the exit code).
test_wait_timeout_ended_and_exit_code() {
  "$HOLD_BIN" -dt --name chatty -- /bin/sh -c 'while :; do echo tick; sleep 0.05; done' >/dev/null || return 1
  set +e
  "$HOLD_BIN" wait chatty --idle 400 --timeout 900 2>/dev/null; rc=$?
  set -e
  [ "$rc" -eq 7 ] || { echo "idle on a chatty call: rc $rc, want 7" >&2; return 1; }
  "$HOLD_BIN" end chatty >/dev/null 2>&1 || true
  "$HOLD_BIN" -dt --name brief -- /bin/sh -c 'echo hi; sleep 0.5; exit 3' >/dev/null || return 1
  set +e
  "$HOLD_BIN" wait brief --match never-appears --timeout 10000 2>"$TEST_ROOT/ended.err"; rc=$?
  set -e
  [ "$rc" -eq 8 ] || { cat "$TEST_ROOT/ended.err" >&2; echo "rc $rc, want 8" >&2; return 1; }
  [ "$("$HOLD_BIN" wait brief)" = 3 ] || return 1
  "$HOLD_BIN" -d --name plain -- /bin/sh -c 'exit 4' >/dev/null || return 1
  [ "$("$HOLD_BIN" wait plain --timeout 5000)" = 4 ] || return 1
}

# wait --idle counts silence from the start of the wait, and settles.
test_wait_idle_settles() {
  "$HOLD_BIN" -dt --name settle -- /bin/sh -c 'for i in 1 2 3 4 5; do echo "step $i"; sleep 0.1; done; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait settle --idle 400 --timeout 8000 || return 1
  "$HOLD_BIN" peek settle | grep -qx 'step 5' || { "$HOLD_BIN" peek settle >&2; return 1; }
  "$HOLD_BIN" end settle >/dev/null 2>&1 || true
}

# resize: the program gets SIGWINCH and the screen reports the new size.
test_resize_delivers_sigwinch() {
  "$HOLD_BIN" -dt --name sized -- /bin/sh -c 'trap "echo SIZE:\$(stty size)" WINCH; echo ready; while :; do sleep 0.1; done' >/dev/null || return 1
  "$HOLD_BIN" wait sized --match ready --timeout 5000 || return 1
  "$HOLD_BIN" resize sized 100 30 || return 1
  "$HOLD_BIN" wait sized --match 'SIZE:30 100' --timeout 5000 || { "$HOLD_BIN" peek sized >&2; return 1; }
  "$HOLD_BIN" peek sized --json | grep -q '"size_set_by": "resize"' || return 1
  "$HOLD_BIN" resize sized 90x20 || return 1
  "$HOLD_BIN" wait sized --match 'SIZE:20 90' --timeout 5000 || return 1
  "$HOLD_BIN" end sized >/dev/null 2>&1 || true
}

# peek --image writes a PNG; without ruler or footer at scale 1 it is exactly
# 8x16 pixels per cell.
test_peek_image_png() {
  "$HOLD_BIN" -dt --name pic -- /bin/sh -c 'printf "\033[44m box \033[0m \342\224\214\342\224\200\342\224\220\n"; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait pic --match box --timeout 5000 || return 1
  W=$(actor_dir) || return 1
  "$HOLD_BIN" peek pic --image "$W/full.png" --cursor-ring || return 1
  "$HOLD_BIN" peek pic --image "$W/bare.png" --scale 1 --no-ruler --no-footer || return 1
  python3 - "$W/full.png" "$W/bare.png" <<'PY' || return 1
import struct, sys, zlib
def ihdr(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    w, h = struct.unpack(">II", data[16:24])
    pos, idat = 8, b""
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4]); kind = data[pos + 4:pos + 8]
        if kind == b"IDAT": idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    assert len(raw) == h * (w * 3 + 1), (len(raw), w, h)
    return w, h
assert ihdr(sys.argv[2]) == (640, 384), ihdr(sys.argv[2])
w, h = ihdr(sys.argv[1])
assert w > 1280 and h > 768, (w, h)
PY
  "$HOLD_BIN" end pic >/dev/null 2>&1 || true
}

# After the call ends, peek shows its last screen; purge removes it.
test_peek_after_exit_and_purge() {
  "$HOLD_BIN" -dt --name gone -- /bin/sh -c 'echo last words; sleep 0.3' >/dev/null || return 1
  "$HOLD_BIN" wait gone --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" peek gone | grep -qx 'last words' || return 1
  "$HOLD_BIN" peek gone --json | grep -q '"running": false' || return 1
  ls "$HOME"/.local/state/hold/*.screen >/dev/null 2>&1 || return 1
  "$HOLD_BIN" purge gone >/dev/null 2>&1 || return 1
  ! ls "$HOME"/.local/state/hold/*.screen >/dev/null 2>&1 || return 1
}

# Calls without a TTY have no screen; the verbs say so.
test_screen_verbs_need_a_tty() {
  "$HOLD_BIN" -d --name notty -- /bin/sh -c 'sleep 30' >/dev/null || return 1
  set +e
  "$HOLD_BIN" peek notty 2>"$TEST_ROOT/notty.err"; rc=$?
  set -e
  [ "$rc" -eq 5 ] && grep -q 'no terminal screen' "$TEST_ROOT/notty.err" || { cat "$TEST_ROOT/notty.err" >&2; return 1; }
  "$HOLD_BIN" end notty >/dev/null 2>&1 || true
}

# A mouse event to a program with mouse reporting off is reported, not lost silently.
test_send_mouse_off_is_reported() {
  "$HOLD_BIN" -dt --name nomouse -- /bin/sh -c 'echo ready; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait nomouse --match ready --timeout 5000 || return 1
  set +e; "$HOLD_BIN" send nomouse --mouse click,3,1 2>"$TEST_ROOT/mouse.err"; rc=$?; set -e
  [ "$rc" -eq 9 ] || { echo "mouse-off send rc $rc, want 9" >&2; return 1; }
  grep -q 'mouse reporting off' "$TEST_ROOT/mouse.err" || return 1
  "$HOLD_BIN" peek nomouse --json | grep -q '"last_mouse": {"row": 1, "col": 3}' || return 1
  "$HOLD_BIN" end nomouse >/dev/null 2>&1 || true
}

# Every call knows its own address; a call launched from inside another
# learns its parent's.
test_call_identity_environment() {
  # Launched from outside any call (the suite itself may run inside one).
  out=$(env -u HOLD_CALL_NAME -u HOLD_CALL_ID -u HOLD_CALL_PARENT "$HOLD_BIN" -d --name selfaware -- /bin/sh -c 'echo "name=$HOLD_CALL_NAME id=$HOLD_CALL_ID parent=${HOLD_CALL_PARENT:-none}"') || return 1
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  "$HOLD_BIN" wait selfaware --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" logs selfaware | grep -qx "name=selfaware id=$id parent=none" || { "$HOLD_BIN" logs selfaware >&2; return 1; }
  HOLD_CALL_NAME=lead "$HOLD_BIN" -d --name kid -- /bin/sh -c 'echo "parent=$HOLD_CALL_PARENT name=$HOLD_CALL_NAME"' >/dev/null || return 1
  "$HOLD_BIN" wait kid --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" logs kid | grep -qx 'parent=lead name=kid' || { "$HOLD_BIN" logs kid >&2; return 1; }
}

# Attaching repaints the live screen rather than replaying bytes.
test_attach_repaints_live_screen() {
  "$HOLD_BIN" -dt --name painted -- /bin/sh -c 'printf "first\nsecond\033[1;1Hover"; sleep 30' >/dev/null || return 1
  "$HOLD_BIN" wait painted --match over --timeout 5000 || return 1
  ( sleep 1.5; "$HOLD_BIN" end painted >/dev/null 2>&1 ) &
  "$HOLD_BIN" attach painted </dev/null >"$TEST_ROOT/attach.out" 2>&1 || true
  wait
  grep -a -q 'overt' "$TEST_ROOT/attach.out" || { od -c "$TEST_ROOT/attach.out" | head -20 >&2; return 1; }
  "$HOLD_BIN" end painted >/dev/null 2>&1 || true
}

run_test "--print spans --all and multiple explicit targets" test_print_over_all_and_multiple
run_test "stop refuses a record with a tampered pgid<=1 (exit 5)" test_signal_refuses_tampered_pgid
run_test "public-index write failure rolls back the start" test_public_index_write_rollback
run_test "--quiet prints bare id and silences stderr" test_quiet_suppresses_banner_keeps_id
run_test "run id prefix resolves to the full run" test_run_id_prefix_resolution
run_test "action/help/list argument guards" test_misc_action_guards
run_test "owned commands reject extra targets and unsupported --all" test_owned_command_exact_arity
run_test "system store directory modes are private (0700/0755)" test_system_store_directory_modes
run_test "system store tightens a pre-existing loose dir" test_system_store_tightens_preexisting_loose_dir
run_test "system store refuses symlinked critical dirs without chmod-following" test_system_store_refuses_symlinked_critical_dirs
run_test "system store artifacts are owned by root:root" test_system_store_artifacts_owned_by_root
run_test "non-root process ignores spoofed SUDO_* provenance" test_nonroot_ignores_spoofed_sudo_provenance
run_test "start/stop lifecycle" test_lifecycle
run_test "kill subcommand kills process group" test_kill_subcommand
run_test "stop kills full process group (children)" test_group_kill_children
run_test "exec failure creates no record" test_exec_failure_no_record
run_test "fast exit command is recorded as exited" test_fast_exit_record_exited
run_test "exec replacement remains controllable" test_exec_replacement_remains_controllable
run_test "corrupt record warning and prune cleanup" test_corrupt_record_handling
run_test "deeply nested/corrupt JSON record is rejected, not crashed" test_deep_json_record_rejected_not_crashed
run_test "symlinked record is rejected" test_symlinked_record_rejected
run_test "symlinked log is rejected" test_symlinked_log_rejected
run_test "invalid pgid=0 record is not listed as running" test_invalid_pgid_record
run_test "unreferenced logs are removed by prune" test_orphan_log_cleanup
run_test "ID input sanitization rejects invalid ids" test_id_sanitization
run_test "stop/kill --print emits group signal command" test_print_signal_output
run_test "signal refuses tampered live process-group identity" test_signal_refuses_tampered_live_group_identity
run_test "stop supports multiple IDs in one command" test_stop_multiple_ids
run_test "argument edge cases" test_argument_edges
run_test "Docker-shaped run/logs/ps/rm surface" test_docker_shaped_cli_flags_and_rm
run_test "ps table humanizes CREATED, phrases STATUS, and marks saved" test_ps_table_humanized_status_and_saved
run_test "ps table content-sized columns do not shear" test_ps_table_columns_do_not_shear
run_test "ports lists a call's bound listeners" test_ports_lists_bound_listener
run_test "stats single-shot prints the documented columns" test_stats_single_shot_shape
run_test "inspect reports live stdio fd targets" test_inspect_reports_stdio_targets
run_test "bare foreground streams output; -d detaches with a 64-hex id" test_docker_bare_launch_foreground_follows_output_by_default
run_test "Docker run foreground follows output by default" test_docker_run_foreground_follows_output_by_default
run_test "unsupported Docker-shaped options fail loudly" test_docker_unsupported_options_fail_loudly
run_test "Docker restart policy restarts failed processes" test_docker_restart_policy_restarts_failures
run_test "purge never follows stored paths" test_purge_never_follows_stored_paths
run_test "usage errors go to stderr; help names both views" test_usage_errors_go_to_stderr_and_help_names_both_views
run_test "crash-leftover temp does not block record rewrites" test_crash_leftover_tmp_does_not_block_rewrites
run_test "purge sweep respects the creation reserve" test_purge_sweep_respects_creation_reserve
run_test "logs --plain dumps the whole log; -n/--tail is the last N" test_logs_plain_dumps_whole_log_and_tail_is_last_n
run_test "foreground launch propagates the exit code" test_docker_foreground_propagates_exit_code
run_test "restart call records its final exit status" test_restart_final_status_recorded
run_test "ending a restart call records the term status" test_end_restart_call_records_term_status
run_test "unwitnessed death renders Exited (?)" test_unwitnessed_death_renders_exited_unknown
run_test "Docker restart validates policies and tty gate" test_docker_restart_validation_and_tty_gate
run_test "start existing run appends retained log" test_start_existing_run_appends_retained_log
run_test "Docker publish and volume flags are rejected" test_docker_publish_and_volume_rejected
run_test "Docker -i keeps non-PTY stdin open" test_docker_interactive_stdin_pipes_to_child
run_test "Docker -it foreground attaches and detaches" test_docker_tty_foreground_attaches_and_detaches
run_test "Docker --detach-keys changes TTY detach sequence" test_docker_tty_custom_detach_keys
run_test "special characters are preserved in argv JSON" test_special_chars_args
run_test "logging captures stdout+stderr" test_log_capture
run_test "logs -n prints the newest lines; -f drains to the end" test_logs_tail_and_follow
run_test "-f starts and follows output" test_start_follow_short_form
run_test "tail <id> tails an existing run log" test_tail_verb_existing_id
run_test "persistent stale records remain visible and dumpable" test_persistent_stale_records
run_test "missing boot source does not force stale" test_boot_unavailable_does_not_force_stale
run_test "leader zombie with live group remains running" test_leader_zombie_group_still_running
run_test "tail <id> prints finished log output" test_tail_finished_log_prints_existing_output
run_test "--console works without an external attach tool" test_console_does_not_require_external_attach_tool
run_test "console reports a normal run has no console" test_console_reports_non_console_run
run_test "console attach round-trips and tees to the log" test_console_round_trip_and_log_tee
run_test "console exit code is recorded" test_console_exit_code_is_recorded
run_test "end on a console call signals the held group, not the broker" test_end_console_call_signals_target
run_test "end on a console call escalates TERM to KILL" test_end_console_call_escalates_to_kill
run_test "SIGTERM to the console broker forwards to the held group" test_console_broker_term_forwards_to_target
run_test "console rejects unrelated peer UID before replay" test_console_rejects_unrelated_peer_uid_before_replay
run_test "console can reattach after detach" test_console_can_reattach_after_detach
run_test "console socket lives in store dir, not /tmp, for long paths" test_console_socket_lives_in_store_dir
run_test "console target runs in caller cwd (relative-bind restores cwd)" test_console_target_runs_in_caller_cwd
run_test "prune <id> removes exactly one run record/output" test_prune_by_id
run_test "prune all removes prunable while preserving running" test_prune_all_keeps_running
run_test "save marks the record and is idempotent" test_save_marks_record_and_is_idempotent
run_test "sweeping purge skips saved calls" test_purge_sweep_skips_saved
run_test "targeted purge of a saved call refuses with exact wording" test_purge_targeted_refuses_saved_exact_wording
run_test "purge --force removes a saved ended call" test_purge_force_removes_saved_ended
run_test "purge --force removes a live saved call" test_purge_force_removes_live_saved
run_test "rename of a saved call keeps purge protection" test_rename_of_saved_call_keeps_protection
run_test "rename saves an unsaved call" test_rename_saves_an_unsaved_call
run_test "redial honors a recorded foreground recipe (and -d overrides)" test_redial_honors_recorded_foreground_mode
run_test "redial honors a recorded detached recipe" test_redial_honors_recorded_detached_mode
run_test "redial honors a recorded console recipe" test_redial_honors_recorded_console_mode
run_test "transactional launch rollback on record write failure" test_transactional_record_write_failure
run_test "raw start does not steal trailing --system" test_raw_start_does_not_steal_trailing_system
run_test "long command appears in list, truncated with ..." test_long_command_list_truncates_instead_of_skips
run_test "normal start writes user-local state" test_normal_start_writes_user_local_state
run_test "hand-authored native record is readable; legacy keys ignored" test_native_record_is_readable_when_hand_authored
run_test "root starts use system store and public state is projected" test_root_start_writes_system_store_and_public_unknown
run_test "sudo start writes system state with invoking-user metadata" test_sudo_start_writes_system_store_with_invoking_metadata
run_test "sudo --system home executable uses invoking-user store" test_sudo_system_start_of_home_executable_uses_user_store
run_test "sudo --system home argv paths use invoking-user store" test_sudo_system_start_of_home_argv_paths_uses_user_store
run_test "sudo context can stop unique invoking-user local run" test_sudo_context_can_stop_unique_user_local_run
run_test "public root index rows are redacted in normal list" test_public_root_index_list_is_redacted
run_test "public root index list reads projected State" test_public_root_index_list_reads_projected_state
run_test "list is the ledger with a USER column; -l/--live narrows to running" test_list_default_is_full_ledger_live_narrows
run_test "list <name> filters the ledger by the call's name" test_list_name_filter_matches_call_name
run_test "list -a renders the projected global ports" test_list_all_renders_global_ports_projection
run_test "list -s/--system shows the redacted global view only" test_system_list_is_redacted_global_only
run_test "ps is Docker's machine-wide running view" test_ps_is_docker_machine_wide_running_view
run_test "purge -s re-execs the global sweep through sudo" test_purge_system_reexecs_through_sudo
run_test "root list -a labels owners and walks user homes" test_root_list_all_labels_owner_and_walks_homes
run_test "normal run does not self-elevate on local/root ID conflict" test_user_local_wins_over_public_root_collision
run_test "explicit user:<id> targets user-local run" test_explicit_user_target
run_test "deleted capability flags are rejected honestly" test_cap_flags_are_rejected
run_test "Docker --rm removes run artifacts after exit" test_docker_rm_removes_run_artifacts_after_exit
run_test "env flags are recipe data, not hold's own environment" test_env_flag_is_recipe_data_only
run_test "tail Ctrl-C detaches from tail and does not stop run" test_tail_ctrl_c_detaches_from_tail_and_keeps_run
run_test "build artifacts for static and dynamic coexist" test_build_artifact_coexistence
run_test "concurrent starts produce unique ids" test_concurrent_unique_ids
run_test "--env-file populates the recipe env only" test_env_file_populates_recipe_env
run_test "legacy Saved spelling still protects a record from sweeps" test_legacy_saved_key_still_protects
run_test "end on a plain call escalates TERM to KILL" test_end_plain_call_escalates_to_kill
run_test "end signals the recorded group after the leader is gone" test_signal_delivers_to_group_after_leader_exit
run_test "attach is the primary console spelling" test_attach_verb_matches_console
# DIVERGENCE: identity doc promises `logs -p` -t/--time and --date timestamp
# flags, but the logs parser (src/runtime/signal.c hold_cmd_view_action) knows
# neither: `hold logs <id> -p -t` exits 5 with the usage line. Recorded in
# .agentic/reconstruction/contract-gaps.md; re-register once the flags exist.
run_test "logs to a non-TTY dumps plain raw bytes automatically" test_logs_non_tty_auto_plain
run_test "list -u shows only the invoking user's calls, even under sudo" test_list_user_scope_only_personal
run_test "drop removes a call exactly like prune" test_drop_alias_matches_prune
run_test "sudo purge sweeps an orphan public entry" test_orphan_public_entry_swept
run_test "unless-stopped restarts crashes but stays down after end" test_restart_unless_stopped
run_test "peek prints the screen as text, with a ruler, and as JSON" test_peek_text_ruler_and_json
run_test "send types text and keys; --wait settles after the answer" test_send_text_keys_and_wait
run_test "send encodes arrows for the program's cursor-key mode" test_send_keys_follow_cursor_key_mode
run_test "send --paste brackets only when the program asked" test_send_paste_brackets_when_requested
run_test "wait times out (7), sees early exit (8), and prints exit codes" test_wait_timeout_ended_and_exit_code
run_test "wait --idle settles after output stops" test_wait_idle_settles
run_test "resize delivers SIGWINCH and updates the screen" test_resize_delivers_sigwinch
run_test "peek --image writes a well-formed PNG" test_peek_image_png
run_test "peek shows the last screen after exit; purge removes it" test_peek_after_exit_and_purge
run_test "screen verbs refuse calls without a TTY" test_screen_verbs_need_a_tty
run_test "send --mouse to a program without mouse reporting exits 9" test_send_mouse_off_is_reported
run_test "calls see HOLD_CALL_ID, HOLD_CALL_NAME, HOLD_CALL_PARENT" test_call_identity_environment
run_test "attach repaints the live screen" test_attach_repaints_live_screen
run_test "msg delivers between calls and verifies the sender" test_msg_between_calls
run_test "TTY calls default TERM when the launcher has none" test_tty_call_defaults_term
run_test "arrow keys and mouse move a real program's cursor" test_cursor_control_keys_and_mouse
run_test "keys arrive as exact xterm bytes, one write each" test_keys_exact_bytes_one_write_each
run_test "a whiptail form is driven by keyboard alone" test_whiptail_form_by_keyboard
run_test "send reports a clamped legacy mouse event" test_send_mouse_legacy_clamp_is_reported
run_test "tmux: prefix split, mouse pane focus, detach" test_tmux_prefix_mouse_and_detach
run_test "an ncurses program takes mouse clicks" test_curses_mouse_checklist
run_test "micro: click, double-click, drag select, wheel" test_micro_mouse_editing
run_test "wait --idle needs first output; symlinked programs keep their name" test_wait_idle_needs_first_output_and_symlink_launch
run_test "wait --match --since ignores a stale screen" test_wait_match_since_ignores_stale_screen
run_test "peek --image draws CJK and emoji glyphs" test_peek_image_draws_cjk_and_emoji
run_test "msg --settle works in both spellings" test_msg_settle_flag
run_test "peek --last-alt keeps a full-screen program's last frame" test_peek_last_alt_frame

echo "----------------------------------------------------------------"
echo "summary: $PASSES passed, $FAILS failed, $SKIPS skipped"
if [ "$SKIPS" -ne 0 ]; then
  echo "  (some tests skipped; run as root and/or set HOLD_REQUIRE_ROOT_TESTS=1 for full coverage)"
fi
if [ "$FAILS" -ne 0 ]; then
  exit 1
fi
