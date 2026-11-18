# Console

[Docs index](index.md) | Related: [Screens](screen.md), [Launcher](launcher.md)

A call placed with `-t` runs on a pseudo-terminal owned by its **console
broker**: `hold -it <cmd>` attaches to it at once, `hold -dt <cmd>` leaves it
running in the background, and `hold attach <target>` picks it back up later
(`console` is an alias). `Ctrl-P Ctrl-Q` detaches without stopping the call;
`--detach-keys` changes the sequence.

The broker is forked per call and lives exactly as long as the call. It owns
the PTY master and a private socket under the store's `console/` directory
(0700 directory, 0600 socket; peers are checked by uid), and it:

- appends every output byte to the call's log;
- feeds the same bytes to a headless terminal, which `hold peek`, `send`,
  `wait`, `resize`, and `msg` use (see [screen.md](screen.md));
- serves one attach client at a time — a second gets "console already
  attached" — plus short-lived control clients;
- repaints a newly attached terminal from the live screen (cells, colors,
  input modes, cursor), so an idle reattach shows the real current state;
- forwards window-size changes to the PTY (the kernel signals SIGWINCH);
- stamps the call's exit status when the target ends and writes its final
  screen beside the log.

The attach client saves the terminal settings, switches to raw mode and the
alternate screen, and on detach resets the modes a program may have left on
(mouse reporting, bracketed paste, application keys, a hidden cursor) before
restoring the terminal. Non-TTY attaches stream stdin/stdout without screen
switching; a raw client that only reads (`socat -u`) is served after a short
silence.

The wire protocol: a client opening with `holdv1\0\0` speaks 3-byte
`type + be16 length` frames (`D` data, `W` resize, `X` detach); a client
opening with `holdctl1` is a control client; anything else is raw
passthrough.

## Where Unix sockets are denied

Some sandboxes (ChatGPT's code environment among them) refuse
`socket(AF_UNIX)` with `EPERM`. There the broker serves the same console over
FIFOs instead, automatically; `HOLD_CONSOLE_TRANSPORT=fifo` forces it
anywhere (useful for tests). Nothing is given up: peek, send, wait, resize,
msg with verified senders, and attach with detach and reattach all work.

The endpoint is a FIFO (0600) at the usual `<id>.sock` path, and clients pick
the transport by its file type. It only ever carries fixed-size 17-byte
introductions — `kind` (`C` control, `A` attach), the 6-character suffix of
a private `<id>.sock.XXXXXX` directory the client made beside it, and the
client's pid — which are atomic, so concurrent clients never interleave. The
directory holds an `in`/`out` FIFO pair: the broker opens it, removes the
directory, and answers one byte on `out` (`R` ready, `B` busy, `D` denied).
From then on the pair is the connection, speaking the framed protocol above
(control clients skip the `holdctl1` magic; the kind already said it).

Access follows the socket rules. The client accepts an endpoint it owns (root
accepts the call owner's, and hands its channel to that owner); the broker
admits root, the call's owner, or the sudo-named user, judged by who owns the
channel's FIFOs, which must be private. A message sender's claimed pid is
believed only if that process holds the channel's `in` FIFO open (Linux
`/proc/<pid>/fd`, macOS libproc) — the identity the socket gets from peer
credentials. Channels left half-made by a client that died are removed by the
broker when it exits and by `hold purge`.

On macOS, `poll()` never reports end-of-file on a FIFO (Linux does), so on
quiet passes the attach client and the broker probe their FIFOs with
non-blocking reads to notice a party that has gone.

When the program can produce no more output — it exited, or it closed its
terminal and runs on — the broker stops listening at once (either transport)
and saves the final screen before waiting for the process, so `hold peek`
answers immediately with that screen and says whether the program still runs.
