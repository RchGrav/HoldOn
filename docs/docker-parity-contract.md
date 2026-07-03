# Docker familiarity contract

[Docs index](index.md) | [Hold On identity](hold-on-identity.md)

Status: normative, subordinate to [hold-on-identity.md](hold-on-identity.md).
Hold's verbs are its own; what it borrows from Docker is **flag behavior**
and **table appearance**, because that muscle memory is worth keeping. When
Hold's behavior disagrees with this document, Hold is wrong. Tests assert the
shapes written here.

## Flags

These flags behave exactly like their `docker run` counterparts, on Hold's
bare launch form (`hold [flags] <cmd> ...`):

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

Foreground launches print **nothing** of their own — only the process
output. Detach with `Ctrl-P Ctrl-Q`, exactly like Docker.

Flags that would require a container substrate are rejected with an honest
message, never faked: `-p`/`-P` (Hold observes real host ports instead;
see `hold ports` and the PORTS column) and `-v` (host paths are just paths).

## The call table

`hold list` (alias `ps`) renders Docker's table look:

```text
CALL ID   COMMAND   CREATED   STATUS   PORTS   NAMES
```

- Columns are content-sized, never fixed widths that shear.
- CREATED is humanized: `Less than a second ago`, `2 minutes ago`,
  `2 days ago`. Never a raw ISO timestamp in the table.
- STATUS uses Docker phrasing: `Up 2 minutes`, `Exited (0) 2 days ago`,
  `Created` — plus Hold's honest extras: `Stale 2 days`, and a ` (saved)`
  suffix on protected calls.
- COMMAND is double-quoted and ellipsized; NAMES is always present for a
  user's calls.
- PORTS shows sockets actually observed in use by the call's process group.

## Output discipline

stdout carries machine data only: IDs, tables, logs, JSON. Human notes,
hints, and errors go to stderr; `--quiet` suppresses normal human status.
Docker-familiar surfaces print nothing Docker would not print.
