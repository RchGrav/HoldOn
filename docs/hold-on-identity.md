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

hold list                   # Docker-look table (ps is an alias)
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
hold purge [<target>] [-a] [--force]
                            # the one removal verb: no target sweeps ended
                            #  calls (-a includes stale); a target removes one
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

Docker parity applies at the **flag level** (`-d`, `-i`, `-t`, `-e`,
`--env-file`, `--name`, `--rm`, `--restart`, `--detach-keys`) and at the
**output level**: `list`/`ps` renders Docker's table look — content-sized
columns, `2 days ago`, `Exited (0) 2 days ago`, names always present —
with the columns

```text
CALL ID   COMMAND   CREATED   STATUS   PORTS   NAMES
```

There is no PROFILE column (profiles do not exist, and a column of `-`
is noise). Verbs are Hold-native. `-p`/`-P`/`-v` remain honestly
rejected.

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
| grants, sudoers pinning, elevation, `--cap` paths (`src/access/`) | delete — cruft; do one thing well |
| captive CLI (`src/runtime/captive.c`) and the `hcli` idea | delete — with profiles gone there is nothing to edit |
| viewer (`src/viewer/`) | stays — rebuilt in core to the before-0.5 design as `hold logs` |
| `hold shell` | becomes `hold on` / `hold off` |

## What stays (the hard-won plumbing)

Hash-derived 64-hex call IDs with generated `adjective_noun` names; the
raw-log + sidecar capture path; the console broker with child and adopted
modes; the call record store with restart-append; stop/kill escalation
with target revalidation; the test-suite discipline (every surface shape
asserted).
