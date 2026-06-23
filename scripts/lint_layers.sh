#!/usr/bin/env bash
# Enforce the layered architecture's dependency direction: no lower-layer source
# or header may include a higher-layer header. Single source of truth, called by
# the GitHub CI `layer-dependencies` job, by scripts/ci.sh, and by `make lint`.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

fail=0
viol() {
  if grep -rlE "#include \"sigmund/($2)\\.h\"" $1 2>/dev/null; then
    echo "DAG violation in: $1 (forbidden include of: $2)"
    fail=1
  fi
}
viol "src/core include/sigmund/core.h"                                          'platform|store|console|access|runtime|cli'
viol "src/platform include/sigmund/platform.h"                                  'store|console|access|runtime|cli'
viol "src/store include/sigmund/store.h"                                        'console|access|runtime|cli'
viol "src/console include/sigmund/console.h include/sigmund/console_internal.h" 'access|runtime|cli'
viol "src/access include/sigmund/access.h"                                      'runtime|cli'
viol "src/runtime include/sigmund/runtime.h include/sigmund/runtime_internal.h" 'cli'

if [ "$fail" -ne 0 ]; then
  echo "layer dependency direction: VIOLATIONS found" >&2
  exit 1
fi
echo "layer dependency direction: clean"
