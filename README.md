# grutty

[![CI](https://github.com/RchGrav/HoldOn/actions/workflows/ci.yml/badge.svg)](https://github.com/RchGrav/HoldOn/actions/workflows/ci.yml)
[![Multi-arch Release](https://github.com/RchGrav/HoldOn/actions/workflows/release.yml/badge.svg)](https://github.com/RchGrav/HoldOn/actions/workflows/release.yml)

**A tty that scripts and agents can see and drive.** grutty runs any command
as a durable **call** and, for terminal programs, keeps a live headless
screen you can read as text or as a color image, type into, wait on, and
resize — so a test script or an AI agent can work a TUI the way a person
would. Agents running under it can message each other.

It also keeps host processes alive, visible, and controllable after your
terminal, script, or agent moves on. A call is one held process group with a
durable 64-hex ID, a generated `adjective_noun` name, captured logs, and —
with `-t` — its screen. No daemon, no config server, one static binary.

More than nohup, less than systemd. (Formerly Hold; the name is Grunthos the
Flatulent's green putty, with a tty hiding in it.)

```sh
grutty -d python3 -m http.server 8080   # place a call in the background
grutty -dt --name app htop              # a TUI, held on its own terminal
grutty peek app                         # its screen, as plain text
grutty peek app --image app.png         # ...or as a color PNG with a ruler
grutty send app -k F6 -k Down --wait 300
grutty wait app --match "Sort by"       # until the screen shows it
grutty end app                          # end it politely
```

## Install

```sh
curl -LsSf https://github.com/RchGrav/HoldOn/releases/latest/download/install.sh | sh
```

System-wide (installs to `/usr/local/bin` with sudo):

```sh
curl -LsSf https://github.com/RchGrav/HoldOn/releases/latest/download/install.sh | sh -s -- --system
```

Or build from source with any C11 compiler:

```sh
make && ./grutty --help
```

Details, checksums, and offline installs: [docs/install.md](docs/install.md).

Grutty does **not** create containers. It does not isolate filesystems, publish
ports, mount volumes, or pretend a host process has Docker networking. It
manages ordinary host processes and tells the truth about them. Where the
output *looks* like Docker — the call table, the flags, `-d`/`-it` — that is
deliberate familiarity, nothing more.

## What Grutty is for

Development servers, CI helper processes, temporary workers, integration-test
dependencies, a qemu you want to walk away from, and interactive programs you
want to put in the background and come back to.

```text
nohup / &   too little state and safety
Grutty        durable call handle, logs, save/redial, attach/detach
systemd     machine service supervision and boot policy
Docker      containers, images, isolation, networking
```

## Placing calls

```sh
grutty <cmd> [args...]        # foreground: stream its output
grutty -d <cmd> [args...]     # detached: prints the bare 64-hex call id
grutty -it <cmd> [args...]    # attached on a PTY (Ctrl-P Ctrl-Q detaches, leaving it running)
grutty -dt <cmd> [args...]    # on a PTY, detached: drive it with peek/send/wait
grutty <id|name>              # redial: restart a retained call from its recipe
```

Flags mirror Docker where they exist (and cluster the same way: `-dit`):
`-d`, `-i`, `-t`, `-e`, `--env-file`,
`--name`, `--rm`, `--restart`, `--detach-keys`. Fake substrate flags
(`-p`, `-P`, `-v`) are honestly rejected. Use `--` when the command could be
mistaken for a call name.

A redialed call replays its recorded recipe, including its session mode: a
call placed with `-it` reattaches, one placed with `-d` detaches again.

## Driving a TUI: peek, send, wait, resize

A call placed with `-t` (`-it` to watch it, `-dt` to leave it running) gets a
headless terminal that follows every byte the program writes. Anything that
can run a command can then debug a terminal program end to end — a test
script, a CI step, or an AI agent.

```sh
grutty -dt --name ed -- vim notes.txt

grutty peek ed                  # the visible screen, plain UTF-8, one line per row
grutty peek ed --ruler          # with column and row numbers
grutty peek ed --json           # + cursor, modes, title, output counter, idle time
grutty peek ed --image ed.png   # color PNG: ruler margins, cursor marked in red,
                              # footer "CURSOR: col 4, row 0   SIZE: 80x24"
grutty peek ed --image ed.png --cursor-ring

grutty send ed i "hello" -k Escape ":wq" --enter   # text and keys, in order
grutty send ed --paste "$(cat snippet.txt)"        # bracketed paste if enabled
grutty send ed --mouse click,12,4                  # SGR mouse at col 12, row 4
grutty send ed -k C-c --wait 300                   # then wait until it settles

grutty wait ed --idle 500                # screen quiet for 500 ms
grutty wait ed --match "written"         # text appears (--regex for patterns)
grutty wait ed                           # until it exits; prints the exit code

grutty resize ed 120 40                  # SIGWINCH and a reflowed screen
```

Keys are encoded for the program's current modes — arrows switch to
application form under DECCKM, paste is bracketed only when the program asked
for it — so what you send is what a real terminal would send. `wait` exits
0 when its condition holds, 7 on timeout, 8 if the call ended first. When a
call ends, its last screen is kept: `grutty peek` still shows how it left
things.

## Agents: run harnesses, check on them, message between them

An agent harness is a TUI like any other. Place one under Grutty and any other
process — you, a script, another agent — can watch and steer it:

```sh
grutty -dt --name worker -- claude      # or codex, aider, ...
grutty peek worker                      # what is it doing?
grutty msg worker "Run the tests in tests/redial and report back."
```

Every call knows its own address: Grutty sets `GRUTTY_CALL_ID` and
`GRUTTY_CALL_NAME` in its environment, and `GRUTTY_CALL_PARENT` when it was
launched from inside another call. `grutty msg` types the text into the
receiver's prompt — once its screen has been quiet for a second — with a
header naming the sender and how to reply:

```text
[msg from lead; reply: grutty msg lead "..."] Run the tests in tests/redial and report back.
```

The receiving call verifies the sender: the header names a call only when the
sending process really runs inside it; anything else is marked
`(unverified)`. Delivery is capped at 20 messages a minute per receiver, so two
polite agents cannot reply to each other forever.

## Managing calls

```sh
grutty list                   # your ledger: your calls, live and past
grutty ps                     # the Docker view: running calls, machine-wide
grutty attach <target>        # pick a call back up (Ctrl-P Ctrl-Q detaches)
grutty end <target>           # end politely: TERM, then KILL (stop is an alias)
grutty kill <target>          # KILL now, when it won't listen
grutty logs <target> [-f]     # captured output as plain text
grutty inspect <target>       # everything visible at your access level, JSON
grutty ports <target>         # ports in use by the call's process group
grutty stats <target>         # live CPU/memory/pids stream
grutty save <target>          # protect a call from purge
grutty rename <target> <name> # give it a meaningful name
grutty purge [<target>] [-a] [--force]   # the one removal verb
                            # (rm, prune, and drop are aliases)
```

Targets are call IDs, ID prefixes, or names. Ambiguity is refused, never
guessed. Saved calls survive every purge except a targeted `--force`:

```text
$ grutty purge web
grutty: 'web' is saved — purging a saved call requires --force
  grutty purge web --force
```

## Logs

A call's output is captured as raw bytes in one plain file, readable by
anything. `grutty logs` prints it:

```sh
grutty logs web               # everything captured
grutty logs -f web            # follow until the call ends
grutty logs -n 100 web        # the newest 100 lines (pipe to grep to filter)
```

For a TTY call, the log is the byte stream; `grutty peek` is the screen. Output
a program sends somewhere else
on purpose — its own logfile, a redirect — is respected, not captured; `grutty
inspect` reports where the call's stdio actually points.

## Safety model

Grutty is daemonless: every command reopens recorded state and revalidates
before acting. A TTY call's screen lives in that call's own console broker —
born with the call, gone with it — and `peek`, `send`, `wait`, and `msg` talk
to it directly over the call's private socket. Managed calls get their own process group and session; stop
and kill are delivered to the group after the recorded identity is
revalidated, so a recycled PID is never signaled by mistake. stdout carries
machine data; human notes go to stderr; `--quiet` silences them.

## Build

```sh
make
./grutty --help
```

Requires a C11 compiler and POSIX; Linux and macOS are tested. The design
contract lives in [docs/grutty-identity.md](docs/grutty-identity.md).

Vendored: [libvterm](third_party/libvterm/) 0.3.3 (MIT) is the headless
terminal; a subset of [GNU Unifont](third_party/unifont/) (SIL OFL 1.1) draws
`peek --image`.
