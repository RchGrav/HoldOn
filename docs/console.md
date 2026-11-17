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

- appends every output byte to the call's log and `HLOGIDX` sidecar (the
  sidecar is tagged `pty` once the stream shows itself to be a full-screen
  program);
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
