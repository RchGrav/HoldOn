# Screens: peek, send, wait, resize, msg

[Docs index](index.md) | [Repository README](../README.md) | Related: [Console](console.md), [Implementation invariants](SPEC.md)

A call placed with `-t` runs on a PTY owned by its **console broker**, a
process born with the call and gone with it (Hold stays daemonless). The
broker writes every output byte to the call's log, and also feeds it to a
headless terminal — vendored [libvterm](../third_party/libvterm/) 0.3.3 —
so it always knows the call's screen: every cell's characters, colors and
attributes, the cursor, and the program's input modes.

## Layers

| Code | Role |
| --- | --- |
| `src/screen/screen.c` | The terminal wrapper: feed, resize, snapshot, mode-aware key/paste/mouse encoding, the HSNAP1 format, plain text, repaint. Depends on core only. |
| `src/screen/render.c` | Snapshot to PNG: 8x16 cells (GNU Unifont subset, procedural box drawing/blocks/Braille), ruler, cursor ring, mouse reticle, footer, and a small deflate encoder. |
| `src/console/broker.c` | Owns the PTY; one attach client plus up to 16 control clients; waits, queued messages, the final screen. |
| `src/console/control.c` | The client side of a control request. |
| `src/runtime/screen_cmds.c` | `hold peek / send / wait / resize / msg`. |

## The control protocol

Every connection to a call's socket starts undecided. A client that opens
with the 8-byte magic `holdctl1` is a **control client**: it sends one
request as a big-endian u32 length plus text, receives one reply in the same
framing, and is closed. Anything else is the attach client (`holdv1` framed,
or raw passthrough — a connection silent for 250 ms becomes a read-only raw
attach). Requests and replies are documented beside the constants in
`include/hold/console_internal.h`:

```text
peek [alt=1]                             -> ok\n + HSNAP1 snapshot (alt=1: last full-screen frame)
send\n text <hex> | paste <hex> | key <name> | mouse <action> <btn> <row> <col>
                                         -> ok seq=N [mouse=off]
resize rows=R cols=C                     -> ok rows=R cols=C
wait [idle=MS] [since=SEQ] [match=<hex>] [regex=1] [exit=1] [timeout=MS]
                                         -> ok reason=idle|match|exit ... | ended ... | timeout ...
msg [from=<hex>] [quiet=MS] [timeout=MS] [submit=0]\n <hex text>
                                         -> ok delivered verified=0|1
```

`seq` is the count of output bytes the broker has seen; `hold send --seq`
prints it and `hold wait --since SEQ` requires output beyond it, which
closes the send-then-wait race completely. Without `--since`, `--idle`
counts silence from the moment the wait began.

## Query replies and attached terminals

Programs ask their terminal questions (cursor position, device attributes).
With no one attached, the headless terminal answers them, so programs that
wait for replies work under `-dt`. While a human terminal is attached, it
answers for itself and the broker drops its own replies so nothing is
answered twice. A newly attached terminal is repainted from the screen —
cells, colors, input modes, cursor — rather than replayed from bytes.

## HSNAP1

The snapshot format, little-endian: magic `HSNAP1\0\0`; u16 rows, cols,
cursor row, cursor col; a mode byte (cursor visible, alternate screen,
application cursor keys, application keypad, bracketed paste, focus
reporting, SGR mouse) and the mouse tracking mode; the broker's metadata
(u64 output counter, u32 idle ms, flags for running / has exit code / has
mouse / size set by resize, i32 exit code, u16 last mouse row and col); a
u16-length title; then rows x cols cells of `u32 ch[3]` (base character and
two combining marks; 0 = blank), u8 width (1, 2, or 0 for the right half of
a wide character), u8 attributes, u8 default-color flags, RGB foreground,
RGB background.

When the target exits, the broker writes the final snapshot to
`<id>.screen` beside `<id>.log`; `hold peek` reads it for ended calls and
purge removes it with the other call artifacts.

Full-screen programs draw on the alternate screen and switch back when they
exit, which leaves the normal screen showing none of what they displayed.
When output contains the switch-back (`CSI ?1049l`, `?1047l`, `?47l`), the
broker feeds the screen up to it, keeps a snapshot of that last full-screen
frame, then feeds the rest. `hold peek --last-alt` asks for it (`peek
alt=1`); at exit it is saved as `<id>.altscreen`.

## Messages between calls

`hold msg` queues text in the *receiver's* broker, which types it in once
the screen has been quiet for the quiet period (default 1 s) or the timeout
passes, as a bracketed paste when the program enabled mode 2004 (otherwise
with newlines flattened so it cannot submit early), then presses Enter. The
sender claims its call by `HOLD_CALL_ID`; the receiver reads the connecting
process's pid from the socket, and the claim is **verified** only if that
process's session is the claimed call's recorded session. The header then
names the call and how to reply; an unverifiable claim is marked
`(unverified)`. Delivery is capped at 20 messages a minute per receiver.
