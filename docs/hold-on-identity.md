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

One static binary, small enough to code-review in an afternoon: the lean
core plus the screen layer (vendored libvterm and an embedded font subset
live in `third_party/`). Logs are one plain file per call, readable without
Hold's help; `hold logs` prints them, and the screen of a TTY call is what
`peek` shows.

## The surface

`hold` is already a verb. There is no `run` command.

```sh
hold -d <cmd> [args...]     # start on hold (detached), print the 64-hex run id
hold -it <cmd> [args...]    # start attached on a PTY
hold -dt <cmd> [args...]    # start detached on a PTY (a screen to drive)
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
                            # captured output, plain text, always script-safe
hold peek <call>            # a TTY call's screen: text (--ruler), --json, or
                            #  --image a color PNG with ruler, cursor marks, footer
hold send <call> TEXT -k KEY  # type and press keys, encoded for the program's
                            #  modes; --paste, --mouse ACTION,COL,ROW, --wait MS
hold wait <call>            # until it exits (prints the code, docker wait), or
                            #  --idle MS / --match TEXT / --regex RE on the screen
hold resize <call> C R      # resize its terminal (SIGWINCH)
hold msg <call> TEXT        # message an agent running under Hold; the sender
                            #  ($HOLD_CALL_ID) is verified and told how to reply
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

There are two listings, and they answer different questions.

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

## Screens, not guesses

A TTY call's broker keeps an exact headless terminal, so nothing about the
screen is inferred: `hold peek` reads it, `hold inspect` reports where fds
0/1/2 point, and redirected stdio stays purposeful, not a failure.

## What leaves the core

| Today | Fate |
| --- | --- |
| `run` verb and its parser paths | delete — flags move to the bare/`-d`/`-it` forms |
| profiles, alias store, `profile`/`profiles`/`export`/`import`/`commit` | delete — replaced by save/rename on run records |
| grants, sudoers pinning, elevation, `--cap-add`/`--cap-drop`/`--privileged` launch flags | deleted (executed 2026-07-04) — Hold launches with the invoker's rights; `--system` selects scope; the flags are rejected honestly |
| captive CLI (`src/runtime/captive.c`) and the `hcli` idea | delete — with profiles gone there is nothing to edit |
| viewer (`src/viewer/`), the `HLOGIDX` sidecar, `logs --filter/--similar/--replay` | removed in the agent-terminal turn (2026-11): logs are one plain file; `hold logs` prints it (`-f`, `-n`), `grep` filters, `peek` shows the screen |
| `hold shell` / `hold on` / `hold off` | removed in the agent-terminal turn (2026-11), with foreground adoption; `-dt` plus the screen verbs replace watching a program by hand |

## What stays (the hard-won plumbing)

Hash-derived 64-hex call IDs with generated `adjective_noun` names; the
raw-log capture path; the console broker, now carrying a headless
screen (see [screen.md](screen.md)); the call record store with restart-append; stop/kill escalation
with target revalidation; the test-suite discipline (every surface shape
asserted).
