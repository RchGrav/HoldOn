# Docker familiarity contract

[Docs index](index.md) | [Hold identity](hold-identity.md)

Status: normative, subordinate to [hold-identity.md](hold-identity.md).
Hold's verbs are its own; what it borrows from Docker is **flag behavior**
and **table appearance**, because that muscle memory is worth keeping. When
Hold's behavior disagrees with this document, Hold is wrong. Tests assert the
shapes written here.

## Flags

These flags borrow their `docker run` spelling and meaning, on Hold's bare
launch form (`hold [flags] <cmd> ...`). Familiar shapes, not parity: Hold runs
host processes, not containers, and has no daemon to be every call's parent.

```text
-d, --detach        detached; stdout is exactly one line: the full 64-hex
                    call id, nothing else
-i, --interactive   keep non-PTY stdin open
-t, --tty           allocate a PTY/console
-e, --env           set launch environment (KEY=VALUE)
    --env-file      load KEY=VALUE lines
    --name          choose the call's name (otherwise generated)
    --rm            remove call record/log after exit
    --restart       no | always | unless-stopped | on-failure[:N]
    --detach-keys   detach sequence (default ctrl-p,ctrl-q)
```

The command is resolved against the invoking environment's `PATH`;
`-e`/`--env-file` configure the held process's environment only, never
Hold's own.

Foreground launches print **nothing** of their own — only the process
output — and exit with the held process's exit status once it ends
(`docker run` parity). Interrupting the tail leaves the call held and
exits 0. Detach with `Ctrl-P Ctrl-Q`, exactly like Docker.

Flags that would require a container substrate are rejected with an honest
message, never faked: `-p`/`-P` (Hold observes real host ports instead;
see `hold ports` and the PORTS column) and `-v` (host paths are just paths).
Flags that grant or remove privilege are rejected the same way, never
remapped: `--privileged` and `--cap-add`/`--cap-drop` — Hold launches
ordinary host processes with the invoker's rights, and store scope is
selected with `--system`.

Two launch flags are Hold-native rather than Docker-borrowed:

```text
-w, --wait              keep a waiter as the call's parent, recording its exit
                        code for list and hold wait (clusters: -dw)
    --restart-delay N   seconds between restart attempts (requires --restart)
```

A plain `-d` call leaves nothing behind: one fork, a new session, output
straight into its log, exec. Nothing then records how it ended, so it lists as
`Exited 2 minutes ago` and `hold wait` prints `-1`. `-w` keeps a waiter;
foreground launches, `--rm`, `--restart`, and root-managed calls always have
one (a root call's public entry must be updated when it ends). TTY calls
record their exit through their broker.

## The call table

`ps` and `list` both render Docker's table look, but they are distinct verbs
(`ps` is no longer an alias of `list`; see hold-identity.md). `ps` is the
Docker mirror — a machine-wide, running-first view with Docker's own columns,
minus IMAGE (Hold has no image analogue and does not fake one):

```text
CALL ID   COMMAND   CREATED   STATUS   PORTS   NAMES
```

`list` is Hold's scoped ledger and adds a USER column in Docker's IMAGE slot:

```text
CALL ID   USER   COMMAND   CREATED   STATUS   PORTS   NAMES
```

- Columns are content-sized, never fixed widths that shear.
- CREATED is humanized: `Less than a second ago`, `2 minutes ago`,
  `2 days ago`. Never a raw ISO timestamp in the table.
- STATUS uses Docker phrasing: `Up 2 minutes`, `Exited (0) 2 days ago`,
  `Created` — plus Hold's honest extras: `Exited 2 days ago` (no code) for a
  plain `-d` call, `Stale 2 days`, `Exited (?)` for a death its waiter did
  not witness, and a ` (saved)` suffix on protected calls.
- COMMAND is double-quoted and ellipsized; NAMES is always present for a
  user's calls. A call the caller may not read (a global call in a normal
  user's view) renders `hidden` in its USER and COMMAND cells.
- PORTS shows sockets actually observed in use by the call's process group,
  Docker-formatted: `127.0.0.1:8080/tcp`, `[::]:80/tcp`, comma-separated.

## Output discipline

stdout carries machine data only: IDs, tables, logs, JSON. Human notes,
hints, and errors go to stderr; `--quiet` suppresses normal human status.
Docker-familiar surfaces print nothing Docker would not print.
