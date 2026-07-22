# Hold On reconstruction — Phase 1 blueprint

Target: same featureset, ~9.0k lines (top of the 8–9k band) vs 14.3k today (excl.
names data). (Was ~8.5k; raised 2026-07-21 after mechanism-level verification of
runtime/viewer/cli — see "Verification reconciliation" below.)
Organizing ideas, each of which erases a verified duplication family:

1. **One spawn engine with modes** — today the fork/setsid/TIOCSCTTY/dup2/exec + errno
   handshake exists three times (start.c console child, shell.c, console/broker.c) and
   the PTY-master pump twice more. One `term/` module owns it; every launcher calls it.
2. **One record schema, one field table** — writer and reader generated from the same
   table; no phantom fields (run_id, normalized.argv, binary_path, dead public quartet).
3. **One resolver** — store exports the only id/prefix/name resolution; runtime's
   dir-level prefix scan (resolve.c:resolve_run_id) and call.c copies are deleted, as
   are the 4–6 hand-rolled record walks (replaced by `hold_for_each_record`).
   AMENDED 2026-07-21: resolve.c is NOT deleted wholesale — store's resolver covers
   only the 34-line dir scan; the ~300-line privilege/scope/intent matrix (alias
   intent predicates, root-vs-nonroot × plain/user:/system: routing, ambiguity rc 6)
   is business logic that survives in runtime (~180 intrinsic, budgeted under glue).
4. **One logger pump** — master/pipe → `hold_write_indexed_log_bytes_fd` in one place,
   shared by broker, shell adoption, and foreground follow.
5. **One flag engine off `command_specs`** — per-command flag tables drive parsing,
   arity guards, usage text, and help; no hand-rolled per-verb parsing.

## Layer DAG (arrows = may call; no back edges, no skips except where noted)

```
names(data)   core          platform
     \          \      ____/    |
      \          v    v         |
       \        store           |   (store also calls core+platform fs/paths)
        \         |             |
         \        v             v
          \      term  <—— (core logidx pump, platform pty facts)
           \      |
            v     v
            console            viewer (calls core logidx reader + term key input only)
                \             /
                 v           v
                  runtime  (business logic: launch, signal, list, purge, observe)
                      |
                      v
                     cli   (command_specs, dispatch, usage/help)
```

Rules: platform is the ONLY home for OS #ifdefs about processes/boot/paths/accounts
(observe.c's leak must not recur). core knows no domain types. term knows PTYs and
pumps, not records. console knows sockets/frames, not CLI. viewer never parses
timestamps from text — sidecar only. runtime never opens /proc directly.

## Module list with line budgets

| module | responsibility | budget |
|---|---|---|
| **core/** | | **~930** |
| core/util.c | die/note, checked_snprintf, write_all, exec-handshake read, shell-quote, RFC3339 + HumanDuration, run_id_display | 250 |
| core/sha256.c | SHA-256 + hex + NUL-field update | 115 |
| core/validate.c | hex-charset helper; valid_id/prefix/alias; name_looks_like_id; SUDO_UID/GID parse | 95 |
| core/json.c | one codepoint tokenizer (skip/parse/match modes); typed accessors; one array walk; escape/emit | 250 |
| core/fs.c | open_dir_no_symlink + close_keep_errno; chmod/chown-no-symlink; mkdir_p_mode; ONE hardened reader; unique temp; path_exists | 130 |
| core/logidx.c | HLOGIDX writer (append-only entries, header = magic+version+base_unix_us, count from st_size) + reader (map/find/format) | 210 |
| core.h | | (90) |
| **platform/** | | **~540** |
| platform/boot.c | one boot-id entry point (NULL-or-buffer) | 25 |
| platform/process.c | one /proc-stat field extractor; one process-table iterator + liveness/escapee callbacks; leader/group probes; absorbed observe primitives (proc ids/cpu-rss/fd-target) + non-Linux stubs; absorbed /proc/net/{tcp,tcp6,udp,udp6} listening-socket scanner (~140, was runtime/observe — verified 2026-07-21) | 370 |
| platform/paths.c | PATH resolve, cwd canonicalize + argv normalize, within-dir, self-exe, passwd lookup (+fallback iff static builds stay: decision D-1) | 145 |
| **store/** | | **~700** |
| store/layout.c | table-driven store creation, sudo chown chain | 90 |
| store/atomic.c | hold_atomic_write_json(dir,name,mode,emit_cb,ctx) — the ONLY commit tail; umask defense inside | 60 |
| store/record.c | field-table writer (private 0600) | 100 |
| store/public_index.c | projection writer (0644) + tolerant reader | 120 |
| store/record_read.c | strict single-exit parse; checked narrowing | 150 |
| store/lifecycle.c | free; mark_finished (purged-is-final); reserve create/commit/abort API | 100 |
| store/resolve.c | THE resolver (exact→unique-prefix→name) + hold_for_each_record | 80 |
| types.h + store.h | schema table lives here | (135) |
| **term/** (new) | | **~300** |
| term/spawn.c | pty_spawn(argv,cwd,winsize,mode) → {master,pid}; errno handshake; cfmakeraw wrapper; winsize preset | 150 |
| term/pump.c | master→indexed-log(+sink) pump; detach-key FSM (configurable, 500 ms flush) | 150 |
| **console/** | | **~550** |
| console/broker.c | serve loop (mode struct: child vs adopted); fd-context cleanup; drop_client; single reap+persist path; SIGTERM forward; replay ring | 300 |
| console/socket.c | with_console_dir seam; bind/connect; peer-uid authn both ways | 120 |
| console/attach.c | client: raw mode, alt screen, resize frames, restore ladder, exit codes | 130 |
| frame protocol | lives in pump/attach headers; 3-byte frames + magic (raw-passthrough: decision D-5) | (60) |
| **runtime/** | | **~3,430** |
| runtime/start.c | launch orchestration: id reserve, name gen, recipe capture, store routing (sudo/home), record+index transactional write, redial, restart supervision (was 700 — verified ~150–200 too tight: orchestration+redial+name-gen measure ~900 intrinsic even after term/store consolidations) | 900 |
| runtime/shell.c | hold on/off: real shell via term/spawn, dwell/adoption, normalize adopted argv (was 350 — verified ~60 headroom) | 300 |
| runtime/signal.c | end/stop/kill: full safety chain, TERM→KILL escalation, --print, multi-target; PLUS tail/logs/inspect glue (~360 intrinsic the old 550 label omitted: view engine bridging, follow-until-exit, logs entry, Stdio-splice inspect) | 700 |
| runtime/list.c | ledger/ps tables, USER column, projections, humanized columns, purge/prune sweep incl. reserve+temp+orphan-log reaping (verified CONFIRMED) | 700 |
| runtime/state.c | state evaluation (live/zombie/escapees/stale/unwitnessed) (verified CONFIRMED) | 130 |
| runtime/observe.c | ports/stats/inspect-observation over platform primitives (holds ONLY with the /proc/net scanner moved to platform — see platform row) | 350 |
| runtime/commands.c + runtime.h | glue + the resolve.c scope/privilege/intent matrix (~180 intrinsic, NOT deletable — see amended idea 3; was 270) | 350 |
| **viewer/** | | **~1,730** |
| viewer/filter.c | literal/similar/exclude match engine, lazy scan, backward window, source mask (was 450 — verified ~100 overweight; forward/backward engine consolidation per WO-1) | 350 |
| viewer/tty.c | dynamic viewer: chrome, paging FSM, follow, overlays, timestamps/source columns. Verified right AT budget for CURRENT features; WO-3-owed spec features (horizontal scroll, Ctrl-Home/End, per-stream toggles, center-out discovery) add +150–200 if "same featureset" means spec-complete | 1,300 |
| log_viewer.h | (drop 3 dead match-ring result fields) | (80) |
| **cli/** | | **~760** |
| cli/specs.c | command_specs: verbs, aliases (rm/prune/drop, stop≡end, attach/console, on/off), flag tables, arity, docker-reject list. Holds AT budget only if it absorbs cli.c's ~145 lines of per-command help prose (prose is content — it does not compress, only its duplicated usage lines do) | 280 |
| cli/main.c | dispatch, usage/help generated from specs, version, sudo re-exec path (was 400 — verified residue after the flag engine measures ~430: value-grammar parsers, scope/privilege gating, redial-then-PATH order, purge sudo re-exec all survive) | 430 |
| access/invocation.c | invoking-user recovery (SUDO_* gated) (was 45) | 50 |
| **total** | | **~8,940** |

Names word data (+1,579, verbatim) sits outside the budget as generated data.

## Record schema field table (the one source of truth)

| field | type | write | read | note |
|---|---|---|---|---|
| version | int | always | required | |
| id | 64-hex | always | required, ==filename | SHA-256 over NUL-delimited fields |
| name | alias | if set | valid_alias only | |
| state | string | always | required | running/exited/stale hints |
| created_unix_ns / created_at | i64/rfc3339 | always | fallback←started | t<=0 = absent |
| started_unix_ns / started_at | i64/rfc3339 | always | required | |
| ended_unix_ns / ended_at | i64/rfc3339 | on exit | optional | |
| pid, pgid, sid | pid_t | always | required, ERANGE-checked | |
| uid, gid | uid_t/gid_t | always | required, ERANGE-checked | |
| proc_starttime_ticks | u64 | always | required | opaque, platform-local |
| exe_dev, exe_ino | u64 | always | required | binary identity |
| boot_id | string | always | optional | NULL ⇒ skip boot check |
| recipe.argv | array | always | required (renders cmdline) | ONE copy — no normalized.argv |
| recipe.env | array | if -e/--env-file | optional | recipe data only |
| recipe.cwd | path | always | optional | realpath'd |
| recipe.tty | bool | always | optional | redial honors, never rewritten |
| recipe.mode | string | always | optional | foreground/detached/console |
| recipe.restart / restart_delay | string/i64 | if set | optional | no/on-failure:N/always/unless-stopped |
| console_sock | abs path | console mode | absolute only | |
| saved | bool | on save/rename | optional | capital-"Saved" shim: decision D-4 |
| exit_code, term_signal | int | on exit | optional | WIFSIGNALED→128+sig |
| observed.ports | array | root refresh | optional | absent ≠ none |
| observed.argv | array | adoption | optional | |
| **dropped** | | | | run_id, normalized.*, cmdline_display (writer side), recipe.binary_path, public error/paused/restarting/dead |

Public projection (public_index): id, name?, state_hint, created/started/ended, pid,
pgid, sid, observed_ports, exit_code, running — never argv/env/cmdline/owner.

## The five consolidations, mapped to deleted duplication

| consolidation | replaces (verified) | est. saving |
|---|---|---|
| term/spawn + term/pump | broker child block, shell.c pty/spawn/pump/detach ×5 mechanisms, start.c console child, frame.c raw termios | ~350 |
| store/resolve + for_each_record | record_read resolver + runtime/resolve.c + call.c find_restart_record + 4–6 record walks | ~250 |
| store/atomic.c | record.c/public_index.c commit tails ×2 + call-site fchmod | ~60 |
| core/json tokenizer | D8 decoder ×3, i64/u64 clone, argv_display re-walk | ~130 |
| cli/specs flag engine | per-verb hand parsing, hand-written usage strings | ~250 |

## Open decisions (must be closed before Phase 2 cuts code)

- **D-1 static Linux builds**: keeps the 46-line /etc/passwd parser and the
  build-artifact-coexistence test. Default: keep (test-pinned).
- **D-2 sidecar entry layout**: keep 16-byte packed v1 (external-format stability)
  vs 24-byte plain struct under a v2 bump. Owned by the sidecar-v2 work order;
  blueprint budgets assume v1 kept.
- **D-3 raw-passthrough console clients** (nc/socat attach without magic): keep
  (+~40 lines, current tests may rely on it) or drop (always-framed). Default: keep
  until the test harness is audited for raw connects.
- **D-4 legacy "Saved" read shim**: SPEC.md keeps it; a from-scratch build has no
  pre-0.7 records. Needs an explicit spec amendment either way (currently untested —
  see contract-gaps G10).
- **D-5 hold on/off vs shell, attach vs console spellings**: identity doc says
  on/off/attach; tests pin shell/console. Rebuild must implement BOTH or the contract
  must be re-pinned first (contract-gaps G18/G19 — highest-risk gap).
- **D-6 WO-10 term/keys + term/screen vs the layer DAG**: see "Verification
  reconciliation" below — decides whether viewer stays self-contained (~1,720) or
  term/ grows +~700 while viewer sheds ~140.

## Build order (each step verified by the pinned test groups it enables)

1. core → unit tests (json hardening, sidecar format, time formatting)
2. platform → identity/liveness unit tests
3. store → records/store group
4. term + console → console/PTY group
5. runtime signal/state → signals/safety group
6. runtime start/shell → launch semantics group
7. runtime list/purge + observe → CLI parity, purge/prune groups
8. viewer → logs/viewer group
9. cli specs → parity/surface + argument-edge tests

Precondition for starting Phase 2: the contract gaps in contract-gaps.md are pinned
(at minimum the P1 tier). DONE 2026-07-21: mechanism inventories now exist for
runtime/viewer/cli (see bill-of-materials.md) — no budget in this file is a
projection anymore.

## Verification reconciliation (2026-07-21)

Budgets reconciled against the mechanism-level inventories in bill-of-materials.md.
Every change:

| budget | was | now | why |
|---|---|---|---|
| header target | ~8.5k | ~9.0k | sum of verified budgets; sub-8.5k is off the table |
| platform/process.c | 230 | 370 | absorbs the /proc/net listening-socket scanner (~140) from runtime/observe — the move that makes observe fit |
| platform total | ~400 | ~540 | above |
| runtime/start.c | 700 | 900 | verified ~150–200 too tight (launch orchestration + redial + name-gen ≈900 intrinsic after consolidations) |
| runtime/shell.c | 350 | 300 | verified ~60 headroom (~290 needed incl. off) |
| runtime/signal.c | 550 | 700 | scope was mislabeled: today's file carries ~360 intrinsic tail/logs/inspect glue no other budget line covered |
| runtime glue | 270 | 350 | resolve.c's scope/privilege/intent matrix (~180) is NOT deletable; organizing idea 3 amended |
| runtime total | ~3,050 | ~3,430 | the projection was ~300–400 light; list.c 700 and state.c 130 CONFIRMED unchanged |
| viewer/filter.c | 450 | 350 | verified ~100 overweight (WO-1 forward/backward consolidation) |
| viewer total | ~1,830 | ~1,730 | above; tty.c 1,300 confirmed AT current features |
| cli/main.c | 400 | 430 | verified residue after the flag engine erases hand parsing |
| access/invocation.c | 45 | 50 | trivial rounding vs verified estimate |
| cli total | ~725 | ~760 | above; specs.c 280 holds only if it absorbs ~145 lines of help prose |
| grand total | ~8,485 | ~8,940 | top of the 8–9k band |

Open reconciliation items (decisions, not budget errors):
- **D-6 (new) — WO-10 term/keys + term/screen vs the DAG**: the layer DAG says viewer
  "calls core logidx reader + term key input only", but term/'s 300 budget contains
  only spawn.c+pump.c — no room for WO-10 term/keys (~300) or term/screen (~400).
  Either term/ grows +~700 (netting against ~140 absorbed from viewer/tty.c and the
  console/attach raw-termios family, and unlocking WO-4's no-flicker chrome), or the
  DAG annotation is wrong and viewer stays self-contained at its verified ~1,720.
  Unresolved; the 8,940 total assumes viewer self-contained and NO term/keys+screen.
- **Spec-complete featureset**: viewer/tty.c's budget excludes WO-3/WO-2-owed spec
  features (+150–200). If "same featureset" means spec-complete, worst case is
  ~9,100–9,300 — the band holds only if D-6's consolidation nets out.
