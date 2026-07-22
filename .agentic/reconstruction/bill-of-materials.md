# Hold On reconstruction — Phase 1 bill of materials

Date: 2026-07-21. Inputs: verified layer inventories for core, platform, store, console
(mechanism-level, cross-checked against HEAD and against .agentic/audit-2026-07-20.md,
with the audit's stale entries corrected); measured line counts at HEAD.
Second pass, same date: verified mechanism-level inventories for runtime, viewer, and
cli replace the former ratio projections — this file now carries no projected layers
(access remains a trivial estimate). Blueprint budgets reconciled; see blueprint.md
"Verification reconciliation".

## Baseline

Measured at HEAD (`wc -l` over src/ + include/, excluding include/hold/names/ which is
1,579 lines of generated word data, carried verbatim):

| layer | lines now | inventory status |
|---|---|---|
| core (src/core + core.h) | 1,712 | verified, mechanism-level |
| platform (src/platform + platform.h) | 714 | verified, mechanism-level |
| store (src/store + store.h + types.h) | 1,094 | verified, mechanism-level |
| console (src/console + console.h + console_internal.h) | 1,391 | verified, mechanism-level |
| runtime (src/runtime + runtime.h + runtime_internal.h) | 5,770 | verified, mechanism-level |
| viewer (src/viewer + log_viewer.h) | 2,322 | verified, mechanism-level |
| cli (cli.c + cli_main.c + main.c + config.h) | 1,168 | verified, mechanism-level |
| access (invocation.c) | 62 | estimated (trivial) |
| shared headers not attributed above | ~100 | — |
| **total (excl. names data)** | **~14,337** | (the "13.5k" working figure undercounts by ~0.8k) |

(Runtime re-measured at 5,770 after 2f9011a split call operations out of start.c; the
earlier 5,741 figure predated that commit. The runtime inventory below tiles the layer
to the exact line.)

---

## Core — 1,712 now → ~930 intrinsic

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| SHA-256 primitive + hex + NUL-field hashing (run-id identity) | yes | 120 | 115 |
| Entropy source hold_rand_bytes (temp-name salt only) | no | 43 | 10 |
| Identity/name validation (valid_id/prefix/alias/record; one hex loop, name_looks_like_id) | yes | 85 | 40 |
| SUDO_UID/GID env parsing (checked narrowing) | yes | 31 | 15 |
| Safe-syscall substrate (die_errno, sig_note, checked_snprintf, write_all, has_suffix) | yes | 55 | 40 |
| Shell-quote argv formatter (COMMAND column) | yes | 79 | 45 |
| Exec handshake reader (errno over CLOEXEC pipe) | yes | 24 | 20 |
| Time formatting/parsing (RFC3339 + Docker HumanDuration, spec-pinned) | yes | 84 | 75 |
| JSON emit (escape + argv array writer) | yes | 35 | 30 |
| JSON scanner/tokenizer (ONE decode-next-codepoint core; skip/parse/match as modes) | yes | 214 | 115 |
| Typed JSON accessors + argv/env materialization (shared number tail, one array walk) | yes | 199 | 105 |
| No-symlink fs hygiene (open_dir_no_symlink + close_keep_errno; mkdir_p; hardened reader) | yes | 187 | 110 |
| Unique temp file creation (dot-prefix + .tmp contract) | yes | 26 | 20 |
| HLOGIDX sidecar writer (append-only entries, NO per-chunk header rewrite) | yes | 230 | 120 |
| HLOGIDX sidecar reader (map load / binsearch find / timestamp format) | yes | 120 | 90 |
| run_id_display (12-hex truncation) | yes | 12 | 8 |
| core.h | — | 112 | ~90 |
| **subtotal** | | **1,712** | **~930** (inventory range 900–950) |

Cut fuel: D8 triple escape-decoder, D1 open-dir ladder ×3, D6 close-keep-errno ×~10,
D7 write+index tail ×2, D5 hex loop ×3, i64/u64 clone, alias exports
(read_small_file, mkdir_p0700), write-only sidecar header fields, dead
META_CONTINUATION flag, unreachable STDIN/PTY stream constants, 5 zero-caller exports,
PROFILE_* vestiges. AUDIT-STALE: D2/D3 already resolved at HEAD (ed2e316) — not counted.

## Platform — 714 now → ~345 intrinsic

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| boot-id acquisition (one entry point, NULL-means-skip contract) | yes | 41 | 25 |
| process identity capture/verify (ONE parameterized /proc stat parser; exe dev/ino) | yes | 160 | 55 |
| group+session liveness tri-state scan | yes | 80 | 45 |
| session-escapee count (callback over shared process-table iterator) | yes | 58 | 15 |
| leader/group existence probes (EPERM-is-alive, ESRCH-is-gone) | yes | 34 | 22 |
| PATH-search binary resolution | yes | 47 | 32 |
| cwd-relative canonicalization + argv normalization (--flag=path aware) | yes | 72 | 45 |
| path containment check ('/'-boundary) | yes | 15 | 12 |
| self-executable resolution (sudo re-exec) | yes | 33 | 25 |
| passwd-by-uid + /etc/passwd fallback (fallback conditional on static-build decision) | yes | 80 | 45 |
| platform.h | — | 31 | ~25 |
| **subtotal** | | **714** | **~345** (~300 if static-build passwd fallback dropped) |

Also absorbs from runtime: observe.c's /proc-shaped primitives (hold_proc_read_ids /
read_cpu_rss / fd_target) plus its !__linux__ ENOSYS stubs — the confirmed layer leak.
Budgeted in the blueprint under platform, saving more than it costs.

VERIFIED ADDITION (runtime pass): observe.c's /proc/net/{tcp,tcp6,udp,udp6} listening-
socket scanner (~140 intrinsic) is Linux-specific and also relocates here — this is
what makes the blueprint's runtime/observe budget fit. Platform's effective intrinsic
becomes ~485 (~345 + 140); blueprint platform/process.c grows 230 → 370.

## Store — 1,094 now → ~670 intrinsic

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| store-layout resolution/creation (table-driven {path,mode,owner} loop) | yes | 148 | 80 |
| atomic private-record writer (shared hold_atomic_write_json helper) | yes | 184 | 95 |
| atomic public-index writer (same helper; sanitized projection) | yes | 105 | 50 |
| public-index reader (tolerant, per-field defaults) | yes | 72 | 50 |
| strict record reader (single-exit parse; no Saved shim, no run_id fallback) | yes | 178 | 130 |
| checked i64 narrowing (one unsigned helper) | yes | 49 | 15 |
| id-prefix resolution — THE one resolver, exported | yes | 54 | 45 |
| record teardown (free argv arrays) | yes | 12 | 10 |
| exit stamp, purged-is-final rc=1 vs retry rc=-1, owner restore, ports-cleared projection | yes | 72 | 60 |
| domain types (types.h minus dead fields: binary_path, run_id, public error/paused/restarting/dead) | yes | 136 | 100 |
| api surface (store.h + hold_for_each_record iterator + reserve create/clear) | yes | 35 | 35 |
| **subtotal** | | **1,094** | **~670** (inventory range 640–700) |

Cut fuel: atomic-commit tail ×2, uid/gid narrowing clones, layout triplet ×7,
resolver ×3 across layers (store keeps the only one), write-only "normalized" argv
block, phantom run_id, Saved shim (decision D-4 in blueprint), free/return pairs ×10.

## Console — 1,391 now → ~830 intrinsic (of which ~300 relocates into the shared term module)

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| PTY provisioning + child exec + errno handshake | yes | 190 | 100 |
| AF_UNIX bind/connect via chdir-relative sun_path (one with_console_dir seam) | yes | 166 | 90 |
| mutual peer-uid authentication (3-way platform ifdef) | yes | 70 | 50 |
| broker serve loop + target lifecycle (fd-context struct; drop_client; one reap path) | yes | 285 | 170 |
| 64 KiB replay ring | yes | 50 | 35 |
| wire protocol: magic sniff + 3-byte frames (raw-passthrough = decision D-5) | yes | 137 | 90 |
| attach client (raw termios, alt screen, SIGWINCH→resize, exit codes 5/3) | yes | 183 | 130 |
| detach-key FSM, 500 ms prefix timeout, configurable keys | yes | 86 | 60 |
| adopted-broker entry (hold shell/on) | yes | 34 | 25 |
| target-pid report fd ordered write (deadlock-free contract) | yes | 12 | 10 |
| SIGTERM forwarding to held group (async-signal-safe, no SA_RESTART) | yes | 25 | 20 |
| terminal-mode restoration (DEC resets → alt-screen exit → termios) | yes | 26 | 18 |
| hand-rolled raw termios (cfmakeraw exists) | no | 9 | 0 |
| 9-arg fail/cleanup plumbing (essential behavior, struct shape) | partly | 95 | 30 |
| headers | — | 113 | ~60 |
| **subtotal** | | **1,391** | **~830–890** |

Latent bugs the rebuild must NOT copy: C-R1 (O_NONBLOCK client fd + write_all retries
only EINTR → replay flush to a slow reader drops the client; pick blocking-fd or
explicit drop-on-backpressure) and C-R2 (errno==EIO checked without gating n<0).

## Runtime — 5,770 now → ~3,400 intrinsic (verified; replaces the ~3,050 projection)

Scope: src/runtime/* + include/hold/runtime.h + runtime_internal.h. The table tiles
the layer exactly (per-file "now" sums match `wc -l` at HEAD file by file).

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| Hashed run-id reservation (start.c: NUL-field hash, 5-path material check, O_EXCL reserve, 1024-attempt loop; reserve API moves into store per blueprint) | yes | 75 | 50 |
| No-symlink append log open (re-implements core/fs discipline; intrinsic is one core call) | yes | 36 | 10 |
| Pipe logger capture process (poll both pipes → hold_write_indexed_log_bytes_fd; term/pump consolidation target) | yes | 60 | 35 |
| Restart supervisor + policy engine (no/always/unless-stopped/on-failure[:max], stop-signal-aware wait; ~60-line spawn preamble collapses into one shared preamble) | yes | 180 | 120 |
| Child environment application (portable clearenv fallback + validated KEY=V; clean-base PATH for system-managed launches) | yes | 50 | 30 |
| Launch orchestration (720-line single fn: fork topology, errno handshake, record capture, transactional write; compressible: rollback ladder ×8 ≈120, pipe2 fallback ×3 ≈35, errno-write-then-_exit ×10, console-child extraction to term) | yes | 720 | 400 |
| Foreground exit-status poll (docker run exit parity; already minimal) | yes | 22 | 20 |
| Auto-remove watcher (--rm out-of-process observer, 200 ms poll) | yes | 26 | 20 |
| Sudo-home store routing (within-home target/argument scan + privilege matrix; deliberate safety feature, inherently fiddly) | yes | 90 | 65 |
| Spawn misc helpers (devnull stdio ×dup with shell.c, privileged cwd, kill-supervisor-if-distinct, unlink-if-nonempty) | yes | 57 | 30 |
| Signal-target identity validation (invariant 13 in executable form; every branch spec/test-pinned incl. zombie-leader best effort) | yes | 100 | 80 |
| Graceful TERM→KILL escalation + --print (EPERM=3 / ESRCH=done / other=4 map; near-minimal) | yes | 94 | 72 |
| Multi-target signal command (worst-rc aggregation, per-target notes; prologue shares the common helper) | yes | 69 | 55 |
| Raw log tail (O_NOFOLLOW, from-end seek, SIGINT copy loop; skeleton dedups with follow-stream) | yes | 99 | 75 |
| logs view engine glue (EOF-anchored --tail window ×4 widening, follow-until-exit, liveness callbacks; NOT budgeted in blueprint signal.c 550) | yes | 212 | 130 |
| logs command entry (dispatch + viewer wiring; the ~50-line flag loop is erased by the cli specs engine) | yes | 187 | 110 |
| inspect with live Stdio splice (/proc fd targets spliced into verbatim record JSON) | yes | 157 | 110 |
| List row model + spec-pinned formatting (Docker-parity quoting/ellipsis, stale-never-fabricates-exit, honest uid fallback) | yes | 210 | 160 |
| List collectors: private, public-redacted, cross-user (readdir skeleton ×3 collapses onto hold_for_each_record) | yes | 201 | 150 |
| Root public-ports refresh (only source of non-root PORTS; eventual-consistency contract) | yes | 40 | 30 |
| Content-sized table printer (never shears a value; with/without-USER printf pair collapses) | yes | 80 | 65 |
| list/ps scope dispatch (the scope-vs-privilege matrix — the product's core mental model) | yes | 77 | 60 |
| Prune one run + derived-path artifact unlink (never follows record-stored paths — security invariant) | yes | 110 | 80 |
| Purge sweep (600 s reserve age gate, orphan + tmp-litter sweeps, kept-counts accounting UX) | yes | 108 | 85 |
| Targeted purge command (saved-protection refusal echoing exact --force command; --force stops live calls first) | yes | 146 | 110 |
| hold on session (PTY open/spawn/raw-termios/pump/detach-FSM quintet re-implements console mechanics — audit C-B; residue is session wiring + HOLD_ON_PID + FSM config over term/pump) | yes | 322 | 140 |
| Foreground-group adoption (the killer feature; /proc snapshotting → platform, id hashing → store, broker/logger setup → console/term) | yes | 210 | 125 |
| Adoption id hashing (near-verbatim dup of start.c reservation; one store reserve API with a scope tag erases it) | no | 83 | 0 |
| /proc readers in shell.c (layer leak → platform; find_process_in_pgid dups platform's group scan) | no | 108 | 0 |
| hold off + session glue (comm-verify-before-SIGTERM guard; minimal) | yes | 31 | 25 |
| Target-resolution scope matrix (resolve.c — **blueprint idea 3 is WRONG that this deletes**: store's resolver covers only the 34-line dir scan; the privilege/scope/intent matrix is unbudgeted business logic that must land in runtime) | yes | 304 | 180 |
| Dir-level id/prefix scan (verbatim dup of the store resolver; deleted per idea 3) | no | 34 | 0 |
| /proc process primitives (observe.c leak; ~90 intrinsic counted under platform — 0 here to avoid double count) | yes | 170 | 0 |
| Group observation + port scan (only-listening/only-bound privacy filter; the ~140-line /proc/net scanner relocates to platform — see note under Platform) | yes | 303 | 200 |
| Redial (any-state resolve, recipe session-mode honoring, argv reload, id/log/name reuse) | yes | 165 | 120 |
| Save/rename protection (rename-implies-save, atomic rewrite; shrinks if store exposes rewrite-preserving-argv) | yes | 122 | 90 |
| Generated name assignment (deterministic adjective_noun from id-derived seed; minimal) | yes | 58 | 50 |
| Console attach command glue (commands.c) | yes | 63 | 50 |
| Usage text (hand-written; generated from cli specs — charged to cli) | no | 35 | 0 |
| State evaluation (liveness precedence truth table; blueprint 130 budget confirmed) | yes | 131 | 110 |
| Live stats view (docker-stats parity without cgroups; sampling/format intrinsic) | yes | 182 | 145 |
| ports command (script-safe clean-empty rc 0 semantics) | yes | 74 | 55 |
| API headers (runtime.h + runtime_internal.h; shrinks with a common single-target signature) | yes | 169 | 120 |
| **raw mechanism sum** | | **5,770** | **3,562** |
| less /proc/net scanner relocated to platform | | | −140 |
| **layer subtotal** | | | **~3,400** (range 3,250–3,550) |

Blueprint cross-check (code-verified verdicts): list.c 700 and state.c 130 CONFIRMED;
shell.c 350 has ~60 headroom (~290 needed incl. off); start.c 700 is ~150–200 TOO
TIGHT — launch orchestration + redial + name-gen measure ~900 intrinsic even after the
term/store consolidations; observe.c 350 covers stats+ports+observe (~400) ONLY IF the
/proc/net scanner moves to platform; resolve.c is NOT deletable (~180 lines of
scope/privilege matrix had no budget line — commands/glue 270 becomes ~350); signal.c's
550 label omitted ~360 intrinsic logs/inspect glue nobody else budgets. Net: the 3,050
projection was ~300 light, consistent with this file's original top-of-band warning.

Cut fuel (verified duplication families): spawn preamble ×3 in start.c + shell.c +
broker (term/spawn, ~100); hashed id reservation + material check duplicated whole
(start.c 75 vs shell.c 83 — store reserve API with scope tag erases the copy); log
pump ×2 + broker pump (term/pump); error-cleanup ladder hand-expanded ×8 (~120 → one
owned-resources struct); pipe2-fallback ×3 (~35); close_stdio_to_devnull ×2; /proc
parsing in runtime (~280 → ~150 in platform); record-walk skeleton ×6 →
hold_for_each_record (~120); single-target command prologue ×7 (~210 → ~110, a family
the blueprint does not name); resolve_run_id dup of store resolver; detach-key FSM ×2;
raw termios ×2; public-filename-to-id parse ×2; mark-finished retry loop ×2;
poll-recheck follower skeleton ×3; hash_field ×2; hand-rolled logs flag loop + usage
text (charged to cli); table printf pair ×2.

Runtime invariants (verified at HEAD; binding, extends the cross-layer list):
- Two-phase creation: .<id>.reserve precedes log/socket/record; unlinked only after record rename or on rollback; sweep spares reserves <600 s, reaps older.
- Run-id: SHA-256 over NUL-delimited tagged fields (hold-run-v1: exe, cwd, timestamp_ns, launcher pid, argc, argv[], counter); ≤1024 collision probes; candidate must not collide with ANY existing material (record/.log/.reserve/.sock/public); adoption uses scope tag hold-run-adopt-v1 so launched and adopted ids never collide by construction.
- Exec handshake: child writes errno to CLOEXEC pipe then _exit(127); parent EOF=success, child-errno=rc 1 (ENOENT → "command not found"), read error=fatal; every failure path rolls back (SIGKILL -group + waitpid, unlink reserve, unlink log only if owns_new_log, unlink sock, kill supervisor if distinct).
- Non-restart topology is a double fork: waiter writes target pid to the pid pipe BEFORE closing the handshake pipe (deadlock-free), ignores TERM/INT/HUP, waits, persists exit, exits WEXITSTATUS or 128+sig; record pid/pgid/sid are the TARGET's.
- Exit persistence retries mark_finished 50×100 ms; rc=1 (purged) is FINAL and never resurrects; only rc=-1 retries (covers fast-exit-before-record-write).
- Docker run parity: --docker detach prints bare 64-hex id alone, foreground prints nothing; native prints 12-hex + started/log/tail/stop note; foreground exit status = held process's; SIGINT during tail leaves the call held, exits 0.
- Restart policies no/always/unless-stopped/on-failure[:max]; failures counter resets on success; delay sleep interruptible by stop flag; supervisor stops on TERM/INT/HUP and persists last status; --restart with --tty rejected (rc 5).
- Never signal on pid alone: boot-id gate, starttime-ticks compare (exe dev/ino fallback), live pgid+sid re-verify, recorded identity token required; zombie/gone leader degrades to pgid+sid group-liveness best effort within same boot (test-pinned); pgid≤1 or sid≤0 hard refusal; kill errno map EPERM=3, ESRCH=already-done=0, other=4.
- Graceful stop: TERM to -pgid, wait STOP_TIMEOUT_MS, REVALIDATE identity, KILL, wait 1000 ms, report session escapees (warned, never killed).
- State precedence: recorded failed > unknown ids > boot-stale > zombie-leader (group live=running, empty/zombie-only=exited) > starttime/exe mismatch=stale > group liveness > exists probe; STALE never reports a fabricated exit code.
- Redial honors the recipe's recorded session mode unless explicit mode flags override; reuses id, log path, name, created timestamps; refuses a running call rc 6.
- Purge derives every unlink path from store layout + id, never follows record-stored paths (mismatch warns, leaves file); saved calls need targeted --force (sweep never mass-forces); --force stops live calls via the full graceful chain; sweep prints removed ids and accounts kept live/stale/saved.
- Redaction: public rows show literal 'hidden' for owner and COMMAND; public index never carries argv/env/cmdline; published ports = listening TCP (state 0A) + unconnected bound UDP only; public name matches resolve requires_root=true (rc 3).
- Resolution order: alias (per-command intent) beats id prefix; root plain tokens try system-private then sudo-user store; non-root try own store then public; user: under root requires sudo provenance; name ambiguity rc 6; not-found rc 5.
- list/ps: running-first then newest-start then id; content-sized columns, two-space gutter, headers as width floor, unpadded last column; COMMAND double-quoted, 30 chars + ASCII '...'; ps speaks only Docker; list default scope system for root, user otherwise.
- Sudo-home guard: would-be system launch whose binary or any existing-path argument resolves inside the invoking home is routed to that user's store; sudo-created user artifacts lchowned back (failure fatal with full rollback).
- System-managed launches: cleaned env (fixed safe PATH) + -e entries, cwd pinned to verified root-owned unwritable '/', recorded invocation provenance (invoked_by uid/gid/user, via_sudo).
- Console-mode winsize sampled in the parent before fork (broker runs on /dev/null stdio).
- hold on/off: session exports HOLD_ON_PID; off verifies /proc/<pid>/comm equals its own comm before SIGTERM; detach Ctrl-P Ctrl-Q with 500 ms pending-prefix flush; adoption NEVER kills or waits the adopted group; adopted runs get reserve + generated name + broker-with-log-only-fallback; all broker fds opened pre-fork.
- docker --tail semantics: newest N via EOF-anchored backward window widening ×4; interactive viewer auto-follows running calls from EOF; plain non-TTY keeps strict -f semantics.
- stats/ports resolve with inspect intent; not-running yields clean empty rc 0; EACCES surfaces rc 3 only when nothing at all observed.
- Corrupt records: list warns and skips; purge sweep unlinks; embedded id must equal filename id everywhere a record is trusted.

## Viewer — 2,322 now → ~1,720 intrinsic (verified; replaces the ~1,830 projection)

Scope: src/viewer/filter.c (501) + src/viewer/tty.c (1,728) + log_viewer.h (93).
Consumers: runtime/signal.c (non-interactive `hold logs` via hold_log_filter_fd /
backward_fd; interactive via hold_log_viewer_tty_fd) and tests/viewer_filter_test.c.
The 79% ratio projection was ~100 HIGH: verified intrinsic is ~1,720 self-contained
(74%). Blueprint sub-budgets: filter.c 450 is ~100 overweight (verified ~350 — the
forward/backward engine overlap WO-1 already maps); tty.c 1,300 is right AT CURRENT
features (~1,290) but excludes WO-3-owed spec features (horizontal scroll,
Ctrl-Home/End, per-stream Ctrl-1..4, center-out discovery: +~150–200 if "same
featureset" means spec-complete); log_viewer.h 80 correct.

BLUEPRINT INCONSISTENCY (flagged): the layer DAG says viewer "calls core logidx reader
+ term key input only", but term/'s 300 budget contains only spawn.c+pump.c — no room
for WO-10 term/keys (~300) or term/screen (~400). Either term/ grows +~700 and viewer
drops ~140 of absorbed input/substrate (to ~1,580), or viewer stays self-contained at
~1,720 and the DAG annotation is wrong. WO-10 absorption from tty.c: read_key
ESC/CSI/SS3/SGR decoder (90), poll plumbing (~20), raw/alt-screen escape setup (~45),
wheel coalescing (~20) → term/keys; the direct ANSI render emission (incl. the
status-recolor hack at tty.c:1070–1082) → term/screen's cell-grid diff renderer — also
the only route to WO-4's no-flicker bar.

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| filter: similarity match engine (lowercase FNV-1a token profiles, 64-term cap, Dice ≥0.45, ≤8 include/exclude examples; spec-pinned zap semantics, no allocation) | yes | 52 | 50 |
| filter: match predicate composition (include-then-exclude ordering is the zap contract) | yes | 13 | 13 |
| filter: source-mask visibility via sidecar (unknown record stays visible — never fake metadata) | yes | 13 | 13 |
| filter: options/result lifecycle + storage + rings (defaults block ×2; match-offset ring contents have ZERO consumers outside the unit test — ~35 lines dead unless WO-2's center-out discovery claims it) | yes | 108 | 65 |
| filter: forward incremental scanner (budget judged by bytes CONSUMED, mid-line record completion so next_offset is always a record boundary — the subtlety WO-2 continuation trusts) | yes | 136 | 110 |
| filter: backward window scanner (aligned past first newline — never a torn line; own inline match + third ring = WO-1's mapped overlap) | yes | 170 | 100 |
| tty: terminal substrate (raw enter/leave, alt screen, TIOCGWINSZ, poll; ~45 absorbed by WO-10 term) | yes | 78 | 60 |
| tty: key decoder (lone-Esc 50 ms lookahead, CSI/SS3, SGR mouse, printable-goes-to-filter; WO-10 absorbs entirely, viewer keeps ~25 keymap dispatch) | yes | 90 | 85 |
| tty: page cache + selection-as-record-identity (byte-offset selection resolution per spec:177–190, landed rsi-001 gen 4 fix) | yes | 100 | 80 |
| tty: filter-edit navigation reset + resize handling (preserves browsed page anchor + selection off the live edge — recorded rsi-001 tension resolution) | yes | 70 | 55 |
| tty: follow/live-edge machinery + newer-below detection (newer_floor/scan_offset pair; quit-time final check so exit message never lies) | yes | 101 | 85 |
| tty: zap example management (Space excludes-like-this, ≤8, re-resolves selection) | yes | 42 | 35 |
| tty: filter-opts plumbing + sidecar hot-reload (reload on raw-size growth; absence not an error) | yes | 29 | 25 |
| tty: refill + budget-continuation engine (backward↔forward mode flips; continuation ticks resume budget-limited scans — THE WO-2 landed mechanism, 8cdf8bc; owed center-out order adds ~50–70 on top) | yes | 165 | 130 |
| tty: row decoration + display-line building (sidecar-driven ordinal/timestamp/source, stderr tint; 3 sanitizer loops collapse to 1) | yes | 114 | 90 |
| tty: help overlay (macOS/Linux dismiss divergence — rebuild must make dismiss byte-exact) | yes | 50 | 45 |
| tty: polished chrome (header FOLLOWING/VIEWING/EXITED with byte-identical in-place recolor, wrap-aware body, footer; on term/screen becomes grid composition at similar mass) | yes | 140 | 105 |
| tty: legacy --debug-stats chrome (frozen test contract; WO-1 removes after WO-3/4 retest) | no | 67 | 8 |
| tty: navigation FSM (anchor history, EOF-is-not-a-page + no-snap-back rules — hard-won landed fixes; edge arrows scroll ONE line via backward probe; WO-3 still owes ~120–150 of features NOT counted here) | yes | 256 | 200 |
| tty: main event loop + lifecycle (ENOTTY guard, 250 ms/10 ms ticks, wheel coalescing, honest Ctrl-Z, quit-time newer check; keymap table shrinks dispatch) | yes | 265 | 200 |
| tty: file-head state struct + cache-semantics comment (the comment block IS the WO-2/3 design contract — keep) | yes | 135 | 90 |
| log_viewer.h public API (drop the 3 dead match-ring result fields) | yes | 93 | 80 |
| **subtotal** | | **2,322** | **~1,720** (self-contained, current features; ~1,580 if WO-10 term absorbs input/substrate; +150–200 if spec-complete) |

Cut fuel: forward/backward engine overlap (defaults ×2, storage boilerplate ×2, match
consumption ×2, THREE ring implementations — ~100–140); dead match-offset ring (~35);
sanitize loop ×3; strdup-into-cache loop ×3 each with own OOM unwind; restart branches
×2 in refill; probe-options setup ×3 + budget-floor computation ×4; legacy chrome (55);
cross-layer raw-termios/escape-parsing family shared with console/attach.c (WO-10);
run-label derivation ×2; id-display pattern ×2.

Viewer invariants (verified at HEAD; binding):
- Scan budget bounds per-tick latency, never total coverage: budget-limited pages keep scanning in 10 ms idle-tick slices until viewport filled or boundary exhausted; the engine never silently reports a match as nonexistent (WO-2, needle-past-budget test).
- result.next_offset is ALWAYS a record boundary; budget judged by bytes consumed — resumed scans can trust anchors.
- Selection is record identity (byte offset), never a row number: survives refill/resize/wrap/exclusion; resolves to record, else record underneath, else nearest previous; PageUp parks selector top, PageDown bottom.
- Arrow at screen edge scrolls exactly one line via re-anchor+refill; falls back to page ops only when the adjacent record is beyond probe budget.
- Source-mask filtering never fabricates metadata: no-sidecar lines stay visible; mask 0 = all visible, zero lookups.
- Presentation toggles (Ctrl-T/U/W/Y/L) never change stored records or filtering; the viewer NEVER parses timestamps from log text — sidecar only.
- EOF is not a page and not a line: never advance to an empty tail page; on a live call, paging past newest returns to tail and resumes follow — no snap-back loop; the pre-browse EOF history anchor is never replayed.
- Live edge: no cursor (first Up/Space summons it bottom-row and pins the page); newer-below uses newer_floor_offset = file size at browse-away, advances in bounded slices — sparse matches deferred, never skipped; quit does a final newer-check so the exit message never lies.
- Editing the filter while browsed away preserves page anchor + selection (local scan limit pins the old span); Backspace relaxes WITHOUT un-excluding; Ctrl-R restores; exclusions capped at 8.
- Quit keys are exactly the untypeable ones: lone Esc (50 ms lookahead), Ctrl-C, Ctrl-D; every printable byte belongs to the filter.
- Raw terminal contract: ISIG deliberately off; Ctrl-Z suspends honestly (restore, SIGTSTP group, re-enter raw + invalidate cache); restore order = mouse off, cursor show, alt-screen leave, termios TCSAFLUSH.
- Backward scan aligns past the first newline in its window — never a torn first line; reports scan_limited when the window clipped history.
- filter.c is layered below runtime (fd-based, no TTY assumptions) — serves both interactive TTY and `hold logs` plain dumps.
- render_legacy --debug-stats chrome is a frozen byte-stable test contract until WO-3/4 retests land.
- Dice-similarity semantics are spec-pinned (FNV-1a, ≥2-alnum tokens, 64-term cap, 0.45 default, include-then-exclude).
- Sidecar reload tracks raw-log growth by size; sidecar absence is never an error.
- Help-dismiss keystrokes must be explicit and byte-exact cross-platform (WO addendum); mind BSD VSTATUS/VDSUSP for pre-raw-mode reads.

## CLI — 1,168 now → ~760 intrinsic (verified; replaces the ~725 projection)

Scope (this BoM's cli row): cli.c 206 + cli_main.c 869 + main.c 7 + config.h 86.
(The task-slice figure cli_main.c + runtime/commands.c = 967 counts commands.c, which
this file books under runtime glue — commands.c 98 + runtime.h are inside the runtime
inventory above; the blueprint's glue budget holds.) The provided cross-check verdict:
blueprint cli/ ~725 (specs 280 + main 400 + access 45) is mildly optimistic —
cli/specs.c 280 holds AT budget only if it absorbs cli.c's ~145 lines of per-command
help prose, and cli/main.c's residue measures ~430 after the flag engine erases the
hand-rolled parsing. Verified against HEAD line by line:

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| command_specs table + accessors (arity min/max, ALLOW_ALL flag, usage lookup, parser-owned probe — the seed of the blueprint's flag engine) | yes | 86 | 70 |
| help topics + per-command help prose (targets/system/scripting + help_action + show_help; prose is content, not code — generation removes only duplicated usage lines) | yes | 118 | 110 |
| docker-run flag recognizer (-d/-i/-t/-it/-ti/--privileged combos) | yes | 29 | 20 |
| restart policy + delay grammar (on-failure[:max] validation — spec surface) | yes | 43 | 35 |
| detach-keys token parser (^X and ctrl-x forms) + console handoff | yes | 58 | 45 |
| env KEY=V validation + append | yes | 32 | 25 |
| --publish/--volume/--cap-add honest rejects (docker-reject list is a product decision) | yes | 21 | 18 |
| env-file reader (comments, CRLF strip, per-line error positions) | yes | 37 | 30 |
| parse_run_options dispatcher (--flag vs --flag= spelled out ×6 families — exactly what the flag tables erase) | yes | 143 | 40 |
| run-options struct + free + start-options bridge | yes | 47 | 30 |
| global pre-scan (--system/--quiet/--) + bare-usage + explicit-session-mode capture | yes | 50 | 35 |
| owned-command inline flag sweep (per-verb -a/-s/-u/-l/--force/--print/--no-stream + unknown-flag rejects + -- literal handling — erased by per-command flag tables) | yes | 84 | 30 |
| help/version routing + arity + cross-flag validation (-t only on launch; --restart-delay requires --restart) | yes | 44 | 35 |
| invocation + store init + docker output dialect + alias folding (stop≡end, console≡attach, prune/rm/drop≡purge, shell≡on) + system-store privilege gate | yes | 66 | 55 |
| bare-form launch path (start-store routing, redial-then-PATH order — "call id → call name → PATH command") | yes | 37 | 33 |
| owned dispatch (list/ps scope selection + name-filter validation, 12 verb calls, purge sudo re-exec — one auditable execvp, NOT the deleted elevation subsystem) | yes | 117 | 65 |
| includes/prototypes/misc (cli_main.c head) | — | 61 | 15 |
| main.c wrapper | yes | 7 | 7 |
| config.h (feature-test macros, limits, platform defines; PROFILE_* vestiges drop) | yes | 86 | 70 |
| **subtotal** | | **1,166** (+2 untiled) | **~760** (range 720–800) |

Cut fuel: the --flag/--flag= double-spelling ladder ×6; the per-verb inline flag sweep
vs the specs table it sits next to; usage strings duplicated between the specs table
and help prose; hold_usage (in runtime/commands.c) generated from specs.

CLI invariants (verified at HEAD; binding):
- Bare invocation prints help rc 0 (Docker parity); bare form speaks Docker's output dialect (foreground silent, -d prints bare 64-hex id); management verbs keep their own output.
- Alias folding is total before dispatch: shell→on, stop→end, console→attach, logs→__view, prune/rm/drop→purge; ps is deliberately NOT folded onto list.
- ps takes no scope flags and rejects non-Docker flags; list/ps reject any unconsumed flag rather than misreading it as a name filter; -a is accepted wherever --all is.
- Read views (list/ps) may look at the system store without root; acting on it requires root (rc 3), except purge which re-execs through sudo (one auditable execvp).
- Redial gate: only a lone token with no --name and no `--` attempts id-then-name resolution before PATH launch; --, --name, or multi-token argv force a fresh launch.
- -t/--console applies only when launching (rc 5 otherwise); --restart-delay requires an active --restart policy; -it implies console mode; bare foreground defaults tail on, -d forces it off.
- Rejected substrate flags fail honestly with guidance (publish/volume/cap-add), never accept-and-ignore.
- Exit code contract: 0 success, 1 usage/generic, 2 refused for safety, 3 permission/storage, 4 delivery failed, 5 not found/invalid, 6 must disambiguate; stdout is machine data, human text goes to stderr, --quiet suppresses normal status.

## Access — 62 now → ~50 intrinsic (estimated, trivial)

invocation.c: SUDO_*-gated invoking-user recovery. Blueprint's 45 rounds to 50 with
the quiet flag plumbing; not worth a mechanism table.

## Cross-layer invariants (binding on the rebuild — every one, grouped)

### Atomicity & store
1. Atomic visibility: unique temp in SAME dir (O_CREAT|O_EXCL|O_NOFOLLOW, caller mode) → write → fflush → fsync(fd) → close → rename → fsync(parent); dir-fsync warn-only, everything earlier fatal; ferror checked before commit; temp unlinked on failure.
2. Temp naming contract `.<prefix>.<pid>.<nonce>.tmp` — the purge sweep recognizes orphans by exactly this shape; changing it silently breaks two-phase-creation cleanup.
3. Public/private projection: public index never contains argv/env/cmdline/owning uid-gid — only id, name?, state_hint, timestamps, pids, observed_ports, exit_code, running. Absent observed_ports = "not observed yet", not "no ports".
4. Permission matrix: private records 0600; public files 0644 + fchown(0,0) when euid==0; user store dirs 0700; system base+public/ 0755 root; runs/logs/console 0700 root; sudo-created user stores chowned back through ~/.local → ~/.local/state → base.
5. Purged-is-final: mark_finished rc=1 (record vanished, terminal, never recreate) vs rc=-1 (retryable); the stat-to-rename window is accepted, must not widen.
6. Exit stamp: WIFEXITED→status, WIFSIGNALED→128+sig (+term_signal), else 255; root restores prior file uid/gid; public projection rewritten with ports cleared.
7. Strict private-record parse: required fields hard-fail (version, id, pid/pgid/sid, uid/gid, proc_starttime_ticks, exe_dev, exe_ino); every id-like int narrows with ERANGE; name only if valid_alias; console_sock only if absolute; post-load valid_record + embedded-id==filename-id.
8. Read fallback chains: created←started (ns and rfc3339); cmdline←argv-render←cmdline_display←"?"; public created←started←"-"; public running derives from state_hint=="running".
9. Reserve-file contract: .<id>.reserve lives from id reservation to record rename; writer unlinks it; sweep spares young reserves, reaps stale. Rebuild centralizes reserve/commit/abort in the store API.
10. Path layout is on-disk ABI: user store FLAT (+console/); system store runs/logs/public/console; HOLD_TESTING overrides compile out of release; HOME realpath'd first.
11. ID-prefix semantics: exact id wins if file exists; else prefix must match exactly one *.json or fail.
12. Optional fields: absent has_* flag ⇒ key omitted on write, absence not an error on read.

### Identity & signal safety
13. No signal on pid alone — identity is (pid, starttime token, exe dev+ino) captured at launch and re-verified, plus (pgid, sid) match, gated by boot-id. Every link stays.
14. Boot-id: NULL means "evaluate without boot check", never fabricate; macOS synthesized id stable within / distinct across boots.
15. Liveness is tri-state-plus-error: LIVE / ZOMBIE_ONLY / EMPTY / SCAN_ERROR all distinct; zombie never reads live; scan failure never reads empty.
16. Group scans filter pgid AND sid together; guard pgid<=1 || sid<=0 before any group op (kill(-1) is a massacre).
17. kill(x,0): EPERM = exists; only ESRCH = gone; other errno = error, not "no".
18. starttime tokens opaque and platform-local (Linux raw ticks, field 22 parsed after strrchr(')'); macOS usec); compare only within platform+boot.
19. exe identity is (st_dev, st_ino) of the resolved binary, never the path string.
20. Run-id derivation: SHA-256 over NUL-delimited fields (NUL appended even for NULL→"") — injective; 64 lower-hex, displayed as 12.

### Hardening
21. No-symlink discipline: every store-controlled open O_NOFOLLOW (O_DIRECTORY for dirs) + fstat re-verify on the fd; act on fd, never path; ownership check root-or-self-or-root-owned; MAX_RECORD_BYTES cap BEFORE malloc.
22. JSON parser hardening: depth cap 64; reject escaped NUL (backslash-u0000), surrogate escapes, raw control chars; bounds-checked outputs; strict number/bool terminators; get_i64 rejects '+', get_u64 rejects '+'/'-'.
23. checked_snprintf: truncation is ENAMETOOLONG, never a silently shortened path.
24. EINTR retry on every IO loop; write()==0 is EIO; close paths preserve errno; /proc reads O_RDONLY|O_CLOEXEC.
25. mkdir_p chmods only dirs it created; non-dir component → ENOTDIR. chown_if_root is a silent no-op when euid!=0.
26. die_errno captures errno before stdio; sig_note respects quiet.

### Paths & accounts
27. Everything compared/recorded is realpath'd; path_is_within_dir requires '/'-boundary prefix.
28. argv normalization rewrites only tokens resolving to EXISTING paths; --flag=path rewrites only after first '='; all else byte-identical.
29. PATH search: explicit slash bypasses PATH; empty PATH → /usr/local/bin:/usr/bin:/bin; access(X_OK) before realpath.
30. passwd order: platform account db first, file parse second; absent fields empty; oversize → ENAMETOOLONG.

### Sidecar / logs
31. HLOGIDX v1 on-disk format: magic "HLOGIDX\0", LE, versioned header with base_unix_us; entries pack 44-bit offset / 20-bit len-1 / 48-bit µs delta / 16-bit meta; existing sidecars must load or the version field gates a documented migration (sidecar-v2 WO owns the entry-layout decision).
32. Crash recovery: trust min(header count, physical count); corrupt header → truncate+re-init, never fatal; index-append failures ignored — indexing never fails or reorders the raw write; raw log is sole source of truth.
33. Record splitting at '\n'; indexed at pre-write EOF offset; that offset is the viewer's lookup key; single-writer assumption.
34. Oversize records saturate at 2^20 with META_TRUNCATED; len stored as len-1; len==0 skipped.

### Console / terminal
35. Exec handshake: child writes errno on ANY pre-exec failure then _exit(127); parent: EOF=success, errno=child failure, read-error=broker failure.
36. Ordering: target pid written to target_pid_fd BEFORE parent_pipe closes — parent's blocking pid read can never deadlock.
37. Broker failure pre-handshake: errno to parent_pipe, close all fds, unlink socket, kill+reap forked target, _exit(127) never exit().
38. Child: setsid + TIOCSCTTY + dup2×3; broker opens slave O_NOCTTY; PTY has nonzero winsize before exec (80x24 fallback).
39. Socket: umask(077)+0600 in 0700 console dir; chdir-relative sun_path; cwd saved as fd, fchdir restore BEFORE fork/exec, failure fatal; unlink/stat via absolute path.
40. Mutual authn: broker allows root|owner|allowed_peer_uid, writes "attach denied" before close; client verifies socket-file owner. Exactly one interactive client.
41. Replay ring flushed at accept before client goes live; ALL master output goes to the indexed log unconditionally.
42. Adopted mode: NEVER kill/waitpid the adopted group; exit via master EOF/EIO or liveness scan; SIGHUP hup_pid after; never set the SIGTERM-forward target.
43. Child mode: exit persisted via mark_finished, in-loop best-effort then post-loop retry (50×100 ms); rc==1 is final.
44. SIGTERM at broker forwards to -pgid via async-signal-safe handler, installed WITHOUT SA_RESTART, cleared at every reap site. SIGPIPE ignored, prior handler restored.
45. Frame protocol: magic "holdv1\0\0"; frames type+be16 len; 'D' data, 'W' resize (zero dims ignored), 'X' detach; detach default C-p C-q, ≤8 bytes configurable; held prefix flushes after 500 ms or mismatch.
46. Client stdin EOF ⇒ SHUT_WR, keep relaying output; exit codes 5 (no socket/name too long), 3 (other), 0 (clean).
47. Terminal restore order: DEC mode resets → leave alt screen → show cursor → termios TCSAFLUSH.
48. Serve loop 1000 ms poll doubles as post-exit drain: break only on quiet tick or master EOF — eager exit truncates final output.

### Spec-pinned formatting
49. format_duration_human is character-for-character go-units HumanDuration incl. int(hours+0.5) — do not "improve".
50. parse_rfc3339 rejects t<=0 — zero-value timestamp parses as absent, not 1970.

## Sum and honest cross-check (all layers verified 2026-07-21)

| | now | intrinsic |
|---|---|---|
| core, platform, store, console (verified first pass) | 4,911 | **~2,775** (2,745–2,865) |
| + /proc/net scanner relocating runtime → platform | — | +140 |
| runtime (verified; scanner excluded above) | 5,770 | **~3,400** (3,250–3,550) |
| viewer (verified; self-contained, current features) | 2,322 | **~1,720** |
| cli (verified) | 1,168 | **~760** (720–800) |
| access (trivial estimate) | 62 | ~50 |
| names word data (carried verbatim, excluded both sides) | 1,579 | 1,579 |
| **code total (excl. names)** | **14,233** | **~8,850** (range ~8,600–9,100) |

Every non-trivial layer is now backed by a mechanism-level inventory tiled against
HEAD (runtime tiles to the exact line). The verdict the projections could not give:
**the 8–9k target survives verification, but only at the TOP of the band.** The old
~8,430 projection was ~400 light — runtime's business logic compresses worse than its
duplication density suggested (+350 vs projection), viewer was ~110 lighter than the
79% guess, and cli lands ~35 heavier. ~8,850 is the honest center; sub-8.5k is off the
table. Two pressures could push past 9k and must be managed, not ignored: (1) if
"same featureset" is read as SPEC-complete, the viewer's WO-3/WO-2-owed features add
+150–200 (worst case ~9,300); (2) the WO-10 term/keys+screen question — if term/ grows
+~700 it must net out against the ~140 it absorbs from viewer plus the console/attach
terminal family, or the band breaks. Both are recorded as open reconciliation items in
blueprint.md.
