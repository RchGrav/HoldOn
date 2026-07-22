#!/usr/bin/env bash
# Enforce the layered architecture's dependency direction: no lower-layer source
# or header may include a higher-layer header. Single source of truth, called by
# the GitHub CI `layer-dependencies` job, by scripts/ci.sh, and by `make lint`.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

fail=0
viol() {
  if grep -rlE "#include \"hold/($2)\\.h\"" $1 2>/dev/null; then
    echo "DAG violation in: $1 (forbidden include of: $2)"
    fail=1
  fi
}
viol "src/core include/hold/core.h"                                          'platform|store|term|console|access|runtime|cli'
viol "src/platform include/hold/platform.h"                                  'store|term|console|access|runtime|cli'
viol "src/store include/hold/store.h"                                        'term|console|access|runtime|cli'
viol "src/term include/hold/term.h"                                          'console|access|runtime|cli'
viol "src/console include/hold/console.h include/hold/console_internal.h" 'access|runtime|cli'
viol "src/access include/hold/access.h"                                      'runtime|cli'
viol "src/runtime include/hold/runtime.h include/hold/runtime_internal.h" 'cli'

if [ "$fail" -ne 0 ]; then
  echo "layer dependency direction: VIOLATIONS found" >&2
  exit 1
fi
echo "layer dependency direction: clean"
