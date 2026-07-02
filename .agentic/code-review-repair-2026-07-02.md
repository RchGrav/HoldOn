# Code review repair report — 2026-07-02

## Goal
Repair the security/robustness findings from the privileged process-manager code review backlog (CR-001 through CR-015), prioritizing sudo/root artifact and grant paths.

## Changes implemented
- CR-001: `.log.idx` sidecar now opens through parent-directory `openat(... O_NOFOLLOW ...)` and verifies regular file before truncation/header writes.
- CR-002: sudoers self-binary validation now reuses `validate_grant_path_chain` before pinning `abs_hold`.
- CR-003: log-capture fork failure is handled separately and rolls back the already-started target instead of leaving it on an undrained pipe.
- CR-004: `HOLD_BOOT_ID_PATH` env override is compiled only under `HOLD_TESTING`.
- CR-005: granted `run --cap` execution now requires a subject grant for the invoking user or one of their groups, with action checked by the grant loader. Internal root-only elevated alias/hash actions first accept a matching subject grant when present, then retain the existing alias/hash self-elevation validation path for public system aliases.
- CR-006: empty `PATH` components are skipped by trusted helper resolution instead of resolving to cwd.
- CR-007: raw JSON control bytes `< 0x20` are rejected in string parsing.
- CR-008: console broker accepts client sockets as nonblocking; stalled client writes are dropped rather than blocking broker loop.
- CR-009: console client verifies connected broker peer uid matches the pre-connect socket owner or root.
- CR-010: exact run-name matches are attempted before id-prefix resolution.
- CR-011: signal actions refuse live targets without a recorded start-time or exe identity token.
- CR-012: graceful TERM→KILL escalation revalidates the target before SIGKILL.
- CR-013: mark-finished ownership capture/chown uses `open(... O_NOFOLLOW)+fstat` and `fchown`.
- CR-014: record pid/pgid/sid/uid/gid loads and uid/gid env parsing reject narrowing overflows.
- CR-015: privileged/system-managed child starts clear inherited environment and then apply a safe base PATH plus pinned recipe env.

## Tests added/updated
- `tests/viewer_filter_test.c` now covers:
  - symlinked `.log.idx` rejection and sentinel preservation,
  - JSON raw control-byte rejection,
  - empty PATH component skipping without losing non-empty PATH resolution.
- `tests/test_hold.sh` root-lane self-binary test now includes unsafe parent-directory chain coverage, and grant/capability fixtures copy test binaries into root-owned safe paths for CR-002 parent-chain validation.
- `tests/test_version_makefile.sh` release-build fake harness constrains `PATH` so the native-only fake test does not accidentally discover the Linux CI image's real `zig` binary.
- Tag-based versioning now removes the stale `VERSION` file, derives release versions from Git `v*` tags, reports `dev` for source snapshots without Git metadata, and updates `scripts/bump_version.sh` to compute `patch`/`minor`/`major`/`custom` next tags without mutating files.
- `Makefile` links `src/platform/paths.c` into the targeted C harness for the PATH resolver regression.

## Validation run
- `make CC=cc CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'` — pass.
- `make viewer-filter-test CC=cc CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'` — pass.
- `make viewer-filter-test CC=cc CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'` — pass.
- `make CC=clang CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'` — pass.
- `make viewer-filter-test CC=clang CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'` — pass.
- `bash scripts/linux.sh` using the Apple `container` Linux CI path — pass after tag-based versioning changes (`ci.sh` summary: version-sync, static-build, dynamic-build, suite, asan-ubsan, cppcheck, layer-lint all pass; shell suite reports `174 passed, 0 failed, 0 skipped`).
- `bash scripts/linux.sh root` — pass after making the live pgid/sid tamper test deterministic with a second live `hold` run instead of shell pgid/sid introspection (`174 passed, 0 failed, 0 skipped`, plus viewer-filter and hash-vector pass).
- `git diff --check` — pass.

## Validation not completed
- No separate host-level `sudo`/privileged validation was run outside the Apple containerized Linux CI lane.

## Remaining risk
- CR-005 preserves the existing root-only alias/hash self-elevation fallback for public system aliases after attempting subject-grant validation; this is necessary for current self-elevation behavior but should be reviewed against the intended stale-sudoers threat model.
- CR-015 environment cleaning changes privileged/system-managed child semantics; Linux CI coverage passes, but release review should confirm no required deployment environment variables were intentionally inherited before this change.
