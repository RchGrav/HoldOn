# Hold On — identity and cut plan

Status: draft for redline. When approved, this document sits above
[docker-parity-contract.md](docker-parity-contract.md), which shrinks to
govern flag behavior and table output only.

## The name

`nohup` says *no hang-up*: abandon the call gracefully. Hold says **hold
on**: the line stays open. That is not a slogan, it is the mechanism — the
broker holds the PTY alive, and `attach` picks the call back up. Sigmund:
*sig* (signal) + *mund* (Old English, guardian). The anti-goal has a name
too: the tool must never become a C-Monster.

More than nohup, less than systemd.

The unit Hold manages is a **call**: one held process group. You put a
call on hold, list your calls, pick one back up, end it, save the ones
worth keeping. "Run" and "container" do not appear in the vocabulary.

## The shape: one binary

One static binary, small enough to code-review in an afternoon. Target:
under 10k lines — the lean core (6–8k) plus the built-in log viewer.

The viewer is part of the tool, not an add-on: `hold logs <call>` opens
the polished full-screen viewer from the before-0.5 design (quiet 80x24
chrome, center-out filtering, no counters, no flicker), and it is held to
that design's standard. The on-disk format — raw `.log` plus the
16-byte-entry `.log.idx` sidecar (v1) — stays a documented, stable format
so anything else can read Hold's logs without Hold's help.

## The surface

`hold` is already a verb. There is no `run` command.

```sh
hold on                     # guarded shell: "Hold is now active. Ctrl-P Ctrl-Q
                            #  puts the foreground program on hold; hold off or
                            #  exit ends the session."
hold off                    # end the guarded shell
hold -d <cmd> [args...]     # start on hold (detached), print the 64-hex run id
hold -it <cmd> [args...]    # start attached on a PTY
hold <cmd> [args...]        # start in the foreground, stream output
hold <id|name>              # redial: restart a retained call from its recipe
                            #  (-it recipe reattaches, -d recipe detaches,
                            #   otherwise foreground)

hold list                   # your ledger: your calls, live and past, with a USER column
hold list -l                 #  --live: only the running ones
hold list -a                 #  --all: your ledger plus the redacted global calls
hold list -s                 #  --system: the global calls only (redacted for a user)
hold list -u                 #  --user: your personal calls only, even under sudo
hold ps                     # Docker's machine-wide view: running calls (add -a for ended),
                            #  both your calls and the global ones; no USER column
hold attach <target>        # pick the call back up (Ctrl-P Ctrl-Q detaches)
hold end <target>           # end the call politely: TERM, then KILL
                            #  (stop is an alias)
hold kill <target>          # KILL now, when it won't listen
hold logs <call> [-f] [-n N]
                            # the polished full-screen viewer, built in
                            #  (plain output automatically when not a TTY)
hold logs <call> -p         # --print: plain dump, always script-safe
                            #  -t/--time prepends times, --date adds date+time
                            #  (timestamps read from the sidecar, never parsed
                            #   out of the log text)
hold inspect <call>         # uptime, session type (plain/console/fullscreen),
                            #  fd targets, everything visible at your access
                            #  level, as JSON
hold ports <call>           # ports in use by the call's process group
                            #  (own calls need no root: /proc socket inodes)
hold stats <call>           # live resource usage stream (CPU, memory, pids)
hold save <target>          # protect this call from purge; no unsave — see purge
hold rename <target> <name> # rename a call (docker rename); naming a call
                            #  declares you want to keep it, so rename saves it
hold purge [<target>] [-a] [-s|-u] [--force]
                            # the one removal verb: no target sweeps ended
                            #  calls (-a includes stale); -u sweeps your calls
                            #  (default), -s sweeps the global store (re-execs
                            #  via sudo for a normal user); a target removes one
                            #  call; --force means "remove regardless of state"
                            #  (saved or still live). rm, prune, and drop are
                            #  accepted aliases.
```

Purging a saved call without `--force` refuses safely (exit 2) and
repeats the command the user meant, ready to copy:

```text
$ hold purge happy_tiger
hold: 'happy_tiger' is saved — purging a saved call requires --force
  hold purge happy_tiger --force
```

## ps speaks Docker; list speaks Hold

There are two viewers, and they answer different questions.

**`ps` is the Docker mirror.** Docker has no user/system split — one daemon,
one namespace — so a faithful `ps` shows *everything running on the machine*
and nothing else. `hold ps` therefore shows running calls from both scopes at
once: your own calls in full, and the global (root-managed) calls redacted;
`hold ps -a` adds the ended ones, exactly as Docker's `-a` does. Scope-
awareness is not Docker vocabulary, so `ps` has no scope flags — it takes only
`-a` — and no USER column. It renders Docker's columns minus IMAGE (which we
have no analogue for and do not pretend to):

```text
CALL ID   COMMAND   CREATED   STATUS   PORTS   NAMES
```

**`list` is Hold's scoped ledger.** Hold is a runner, and the ledger of past
calls is the product, so `list` defaults to your whole personal ledger — live
*and* past — and grows a USER column in Docker's IMAGE slot (second, after
CALL ID):

```text
CALL ID   USER   COMMAND   CREATED   STATUS   PORTS   NAMES
```

The scope flags belong entirely to `list`:

- **`hold list`** (a normal user) — your calls, live and past. `-l`/`--live`
  narrows to running.
- **`hold list -a`/`--all`** — your ledger plus every global call, redacted.
- **`hold list -s`/`--system`** — the global calls only, none of yours.
- **`hold list -u`/`--user`** — your personal calls only, even under sudo (the
  invoking user's store, never root's).
- **`sudo hold list`** (root) — the global calls only, authoritative. `sudo
  hold list -a` also walks every user's store under `/home/*/.local/state/hold`
  and the USER column names each owner. `-l` composes with all of these.

A call a user is not entitled to read shows the literal `hidden` in its USER
and COMMAND cells — not `-`. `-` reads as *none*; `hidden` reads as
*exists, but not yours to see*, which is the truth. There is no PROFILE column
(profiles do not exist). `-p`/`-P`/`-v` remain honestly rejected.

The PORTS a user sees for a global call come from the public index, a
projection: root refreshes each live global call's observed ports (listening
TCP and bound UDP only, never outbound connections) into its public entry
whenever root runs `list`/`ps`. It is therefore eventually consistent — a port
that opened a moment ago appears the next time root lists, not instantly.

Purge follows the same scope split as list, but `-a` keeps its Docker-ish
*state* meaning (include stale) rather than becoming a scope flag: `hold purge`
and `hold purge -u` sweep your own ended calls; `hold purge -s` sweeps the
global store, re-execing once through `sudo` when you are not root so `sudo`
can prompt for the password (a single audited re-exec, not a return of the
old elevation machinery).

Docker parity otherwise applies at the **flag level** (`-d`, `-i`, `-t`,
`-e`, `--env-file`, `--name`, `--rm`, `--restart`, `--detach-keys`) and at the
**output level**: both tables render Docker's look — content-sized columns,
`2 days ago`, `Exited (0) 2 days ago`, names always present. Verbs are
Hold-native.

## Saved calls replace profiles

There is no profile system. A call record already is a launch recipe:
`save` flags it as protected, `rename` gives it a meaningful name, and
`hold <name>` redials it. One concept instead of three.

## Automatic console detection

No guessing and no flags required for adoption:

- `tty_nr` (`/proc/<pid>/stat` field 7) — does it have a controlling
  terminal at all;
- termios sampled over time on the broker's poll loop — line vs raw
  framing (dwell time, so readline flips don't flap it);
- foreground-pgid tracking (`TIOCGPGRP`) in the `hold on` proxy loop —
  attributes PTY bytes to the child, anchors classification at the spawn
  moment, and snapshots `/proc/<fg>/exe` facts when the child starts;
- positioning-escape scan of the broker replay buffer (ignoring SGR
  color) — confirmatory full-screen tag only, never load-bearing.

Redirected stdio is purposeful, not a failure: `inspect` records where
fds 1/2 point as neutral state; `log` prints one informative line only
when the log would otherwise be misleadingly empty.

## What leaves the core

| Today | Fate |
| --- | --- |
| `run` verb and its parser paths | delete — flags move to the bare/`-d`/`-it` forms |
| profiles, alias store, `profile`/`profiles`/`export`/`import`/`commit` | delete — replaced by save/rename on run records |
| grants, sudoers pinning, elevation, `--cap-add`/`--cap-drop`/`--privileged` launch flags | deleted (executed 2026-07-04) — Hold launches with the invoker's rights; `--system` selects scope; the flags are rejected honestly |
| captive CLI (`src/runtime/captive.c`) and the `hcli` idea | delete — with profiles gone there is nothing to edit |
| viewer (`src/viewer/`) | stays — rebuilt in core to the before-0.5 design as `hold logs` |
| `hold shell` | becomes `hold on` / `hold off` |

## What stays (the hard-won plumbing)

Hash-derived 64-hex call IDs with generated `adjective_noun` names; the
raw-log + sidecar capture path; the console broker with child and adopted
modes; the call record store with restart-append; stop/kill escalation
with target revalidation; the test-suite discipline (every surface shape
asserted).
