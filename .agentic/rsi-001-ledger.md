# rsi-001 — generation ledger (append-only; the experiment's evolution record)

Experiment: can generation N's loop instructions, amended by generation N-1,
steer a fresh agent to a better improvement than a static prompt would?
Target: the Hold On log viewer. Baseline: 146/0 suite, reconciled tree.

Row format — one per generation, appended by the generation's agent:

```
## gen <N> — <commit hash> — <ACCEPTED|REVERTED>
- item: <priority letter + one line>
- changed: <what, concretely>
- evidence: <suite summary line + any new test names>
- unverified: <honest list, or "nothing">
- lesson fed forward: <the one line added to the loop file>
- validator: <verdict + one line, filled by the validator agent>
```

## gen 1 — this commit ("rsi-001 gen 1", hash unknowable pre-commit) — ACCEPTED
- item: a — WO-2: filter must search the whole file (idle-tick continuation)
- changed: filter.c forward scan stops only on record boundaries (soft mid-line
  overshoot; budget judged by bytes consumed, not bytes read); tty.c reuses the
  persisted next_offset/prev_offset anchors to resume budget-limited scans on
  10ms idle poll ticks — forward slices append, backward slices prepend (cursor
  follows its record) — until the viewport fills or the page's file boundary is
  exhausted; pages pinned by local_scan_limit (typed-filter page preserve) are
  exempt by design; renders only on change.
- evidence: "summary: 147 passed, 0 failed, 0 skipped"; new test
  test_log_view_filter_scan_continues_across_ticks (needle 2.7 MB past the
  256 KiB first-page budget: frame shows "| partial", then the hit, then
  "| EOF"); manual pty smokes of both forward (exited run) and backward
  (live-edge follow) continuation found deep/ancient needles.
- unverified: center-out discovery order (spec:297-315) not implemented —
  discovery is anchor-forward/anchor-backward; typed-filter-while-browsed
  pages stay truncated to the pinned byte range (deliberate, needs Rich);
  partial trailing line at EOF of a still-growing log is consumed as a line
  (pre-existing engine behavior); >100 MB timing only reasoned, not measured.
- lesson fed forward: judge scan budgets by bytes consumed, not bytes read —
  a chunk-granular stop silently dropped in-budget lines; trust the pty suite.
- validator:
