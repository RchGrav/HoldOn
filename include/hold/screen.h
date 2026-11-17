#pragma once
#ifndef HOLD_SCREEN_H
#define HOLD_SCREEN_H

#include "hold/config.h"

/* The screen layer: a headless terminal (vendored libvterm) that a PTY call's
 * broker feeds every output byte into, plus the snapshot format every viewer
 * of that screen reads. Depends on core only.
 *
 * A snapshot is plain data: grid size, cursor, terminal modes, and one cell
 * per column with its characters, resolved 24-bit colors, and attributes. It
 * serializes to a small self-describing binary blob ("HSNAP1") that travels
 * over the console socket and is written beside the log when the call ends. */

#define HOLD_CELL_CHARS 3 /* base character plus up to two combining marks */

#define HOLD_ATTR_BOLD 0x01u
#define HOLD_ATTR_ITALIC 0x02u
#define HOLD_ATTR_UNDERLINE 0x04u
#define HOLD_ATTR_REVERSE 0x08u
#define HOLD_ATTR_STRIKE 0x10u
#define HOLD_ATTR_BLINK 0x20u
#define HOLD_ATTR_CONCEAL 0x40u

#define HOLD_CELL_FG_DEFAULT 0x01u
#define HOLD_CELL_BG_DEFAULT 0x02u

struct hold_cell {
    uint32_t ch[HOLD_CELL_CHARS]; /* ch[0] == 0: blank */
    uint8_t width;                /* 1, 2 (wide), or 0 (right half of a wide char) */
    uint8_t attrs;                /* HOLD_ATTR_* */
    uint8_t flags;                /* HOLD_CELL_*_DEFAULT */
    uint8_t fg[3], bg[3];         /* resolved RGB, reverse NOT applied */
};

enum hold_mouse_mode { HOLD_MOUSE_NONE = 0, HOLD_MOUSE_CLICK, HOLD_MOUSE_DRAG, HOLD_MOUSE_MOVE };

struct hold_screen_modes {
    bool cursor_visible;
    bool alt_screen;
    bool app_cursor_keys; /* DECCKM: arrows send ESC O x */
    bool app_keypad;      /* DECKPAM */
    bool bracketed_paste; /* mode 2004 */
    bool focus_report;    /* mode 1004 */
    uint8_t mouse;        /* enum hold_mouse_mode */
    bool mouse_sgr;       /* mode 1006 */
};

/* Broker-side facts about the call that travel with a snapshot. */
struct hold_screen_meta {
    uint64_t output_seq;  /* total PTY output bytes seen so far */
    uint32_t idle_ms;     /* milliseconds since the last output byte */
    bool running;
    bool has_exit_code;
    int32_t exit_code;
    bool has_mouse;       /* the last injected mouse event, for the reticle */
    uint16_t mouse_row, mouse_col;
    bool size_by_agent;   /* the current size was set by hold resize */
};

struct hold_snapshot {
    uint16_t rows, cols;
    uint16_t cursor_row, cursor_col;
    struct hold_screen_modes modes;
    struct hold_screen_meta meta;
    char title[256];
    struct hold_cell *cells; /* rows * cols, row-major */
};

struct hold_screen;

struct hold_screen *hold_screen_new(int rows, int cols);
void hold_screen_free(struct hold_screen *s);
void hold_screen_feed(struct hold_screen *s, const char *bytes, size_t n);
void hold_screen_resize(struct hold_screen *s, int rows, int cols);
void hold_screen_size(const struct hold_screen *s, int *rows, int *cols);
void hold_screen_modes(const struct hold_screen *s, struct hold_screen_modes *out);

/* Bytes the terminal wants written to the PTY: replies to the program's
 * queries (cursor position, device attributes) and encoded input from the
 * send helpers below. Returns the count moved into buf. */
size_t hold_screen_take_output(struct hold_screen *s, char *buf, size_t cap);
/* Drop pending output (query replies a real attached terminal will answer). */
void hold_screen_discard_output(struct hold_screen *s);

/* Input encoding, honoring the program's current modes (application cursor
 * keys, bracketed paste, mouse protocol). The bytes land in the output buffer.
 * hold_screen_send_key: a tmux-style key name such as "Enter", "Up", "F5",
 * "C-c", "M-x", "S-Tab", "C-Left"; returns -1 for an unknown name.
 * hold_screen_send_mouse: action is "press", "release", "click" (press then
 * release), "move", "drag", "scroll-up" or "scroll-down"; button 1-3; row and
 * col zero-indexed. Returns 1 if the program has mouse reporting off (nothing
 * was encoded), 0 on success, -1 on a bad action. */
int hold_screen_send_key(struct hold_screen *s, const char *name);
void hold_screen_send_paste(struct hold_screen *s, const char *text, size_t n);
int hold_screen_send_mouse(struct hold_screen *s, const char *action, int button, int row, int col);

/* Snapshot the current screen (allocates cells; free with hold_snapshot_free). */
int hold_screen_snapshot(const struct hold_screen *s, struct hold_snapshot *out);
void hold_snapshot_free(struct hold_snapshot *snap);

/* HSNAP1 encoding. serialize allocates *out (free()); parse fills snap and
 * returns -1 (errno EINVAL) on a malformed or truncated blob. */
int hold_snapshot_serialize(const struct hold_snapshot *snap, unsigned char **out, size_t *out_len);
int hold_snapshot_parse(const unsigned char *buf, size_t len, struct hold_snapshot *snap);

/* The visible screen as plain UTF-8, one line per row (trailing blanks
 * trimmed; wide-character right halves skipped). With ruler, a two-line
 * column ruler is printed first and each row is prefixed with its number. */
int hold_snapshot_write_text(const struct hold_snapshot *snap, FILE *f, bool ruler);
/* Escape sequences that repaint a terminal of the same size to this screen:
 * clear, every cell with its colors and attributes, the program's input modes,
 * and the cursor. Appends to *buf (realloc'd); returns -1 on allocation failure. */
int hold_snapshot_repaint(const struct hold_snapshot *snap, char **buf, size_t *len, size_t *cap);
/* One row's plain text into out (NUL-terminated, truncated to n). */
void hold_snapshot_row_text(const struct hold_snapshot *snap, int row, char *out, size_t n);

/* PNG rendering (src/screen/render.c). Cells are 8x16 pixels times scale,
 * drawn with an embedded GNU Unifont subset; box drawing, blocks, shades and
 * Braille are drawn procedurally. The ruler adds labeled column/row margins
 * with the cursor's column and row marked in red; the cursor ring circles the
 * cursor cell; the mouse marker shows the last injected mouse event; the
 * footer, one blank cell row below the viewport, spells out the cursor
 * position and grid size. */
struct hold_render_options {
    bool ruler;        /* default true */
    bool cursor_ring;  /* default false */
    bool mouse_marker; /* default true (drawn only if a mouse event happened) */
    bool footer;       /* default true: "CURSOR: col C, row R   SIZE: COLSxROWS" */
    int scale;         /* 1-4; 0 (default) = 2 up to 100 columns, else 1 */
};
void hold_render_options_init(struct hold_render_options *o);
int hold_snapshot_write_png(const struct hold_snapshot *snap, const struct hold_render_options *o, FILE *f);

/* UTF-8 encode one codepoint; returns bytes written (1-4), 0 if n too small. */
size_t hold_utf8_encode(uint32_t cp, char *out, size_t n);

#endif /* HOLD_SCREEN_H */
