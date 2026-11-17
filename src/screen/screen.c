#include "hold/config.h"
#include "hold/screen.h"

#include "vterm.h"
/* Read-only access to libvterm's input modes (DECCKM, keypad, bracketed paste,
 * mouse protocol), which its public API does not expose. Pinned to the
 * vendored 0.3.3; see third_party/libvterm/README.md. */
#include "../../third_party/libvterm/src/vterm_internal.h"

/* ---- the live terminal --------------------------------------------------- */

struct hold_screen {
    VTerm *vt;
    VTermScreen *vs;
    VTermState *state;
    char *out;      /* pending bytes for the PTY */
    size_t out_len, out_cap;
    char title[256];
    size_t title_len;
    bool title_open;
};

static void screen_output_cb(const char *s, size_t len, void *user) {
    struct hold_screen *scr = user;
    if (scr->out_len + len > scr->out_cap) {
        size_t cap = scr->out_cap ? scr->out_cap : 1024;
        while (cap < scr->out_len + len) cap *= 2;
        char *p = realloc(scr->out, cap);
        if (!p) return; /* drop rather than corrupt: replies are best effort */
        scr->out = p;
        scr->out_cap = cap;
    }
    memcpy(scr->out + scr->out_len, s, len);
    scr->out_len += len;
}

static int screen_settermprop_cb(VTermProp prop, VTermValue *val, void *user) {
    struct hold_screen *scr = user;
    if (prop != VTERM_PROP_TITLE) return 1;
    if (val->string.initial) {
        scr->title_len = 0;
        scr->title_open = true;
    }
    if (scr->title_open && val->string.str) {
        size_t room = sizeof(scr->title) - 1 - scr->title_len;
        size_t n = val->string.len < room ? val->string.len : room;
        memcpy(scr->title + scr->title_len, val->string.str, n);
        scr->title_len += n;
    }
    scr->title[scr->title_len] = '\0';
    if (val->string.final) scr->title_open = false;
    return 1;
}

static const VTermScreenCallbacks screen_callbacks = {
    .settermprop = screen_settermprop_cb,
};

static int clamp_dim(int v, int max) {
    if (v < 1) return 1;
    return v > max ? max : v;
}

struct hold_screen *hold_screen_new(int rows, int cols) {
    struct hold_screen *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->vt = vterm_new(clamp_dim(rows, 1000), clamp_dim(cols, 1000));
    if (!s->vt) {
        free(s);
        return NULL;
    }
    vterm_set_utf8(s->vt, 1);
    vterm_output_set_callback(s->vt, screen_output_cb, s);
    s->vs = vterm_obtain_screen(s->vt);
    s->state = vterm_obtain_state(s->vt);
    vterm_screen_enable_altscreen(s->vs, 1);
    vterm_screen_set_callbacks(s->vs, &screen_callbacks, s);
    vterm_screen_reset(s->vs, 1);
    return s;
}

void hold_screen_free(struct hold_screen *s) {
    if (!s) return;
    vterm_free(s->vt);
    free(s->out);
    free(s);
}

void hold_screen_feed(struct hold_screen *s, const char *bytes, size_t n) {
    if (s && n > 0) vterm_input_write(s->vt, bytes, n);
}

void hold_screen_resize(struct hold_screen *s, int rows, int cols) {
    if (s) vterm_set_size(s->vt, clamp_dim(rows, 1000), clamp_dim(cols, 1000));
}

void hold_screen_size(const struct hold_screen *s, int *rows, int *cols) {
    vterm_get_size(s->vt, rows, cols);
}

void hold_screen_modes(const struct hold_screen *s, struct hold_screen_modes *m) {
    const VTermState *st = s->state;
    memset(m, 0, sizeof(*m));
    m->cursor_visible = st->mode.cursor_visible;
    m->alt_screen = st->mode.alt_screen;
    m->app_cursor_keys = st->mode.cursor;
    m->app_keypad = st->mode.keypad;
    m->bracketed_paste = st->mode.bracketpaste;
    m->focus_report = st->mode.report_focus;
    if (st->mouse_flags & MOUSE_WANT_MOVE) m->mouse = HOLD_MOUSE_MOVE;
    else if (st->mouse_flags & MOUSE_WANT_DRAG) m->mouse = HOLD_MOUSE_DRAG;
    else if (st->mouse_flags & MOUSE_WANT_CLICK) m->mouse = HOLD_MOUSE_CLICK;
    m->mouse_sgr = st->mouse_protocol == MOUSE_SGR;
}

size_t hold_screen_take_output(struct hold_screen *s, char *buf, size_t cap) {
    size_t n = s->out_len < cap ? s->out_len : cap;
    memcpy(buf, s->out, n);
    memmove(s->out, s->out + n, s->out_len - n);
    s->out_len -= n;
    return n;
}

void hold_screen_discard_output(struct hold_screen *s) {
    s->out_len = 0;
}

/* ---- input encoding ------------------------------------------------------ */

static const struct {
    const char *name;
    VTermKey key;
} named_keys[] = {
    {"Enter", VTERM_KEY_ENTER}, {"Return", VTERM_KEY_ENTER}, {"Tab", VTERM_KEY_TAB},
    {"Backspace", VTERM_KEY_BACKSPACE}, {"BSpace", VTERM_KEY_BACKSPACE},
    {"Escape", VTERM_KEY_ESCAPE}, {"Esc", VTERM_KEY_ESCAPE},
    {"Up", VTERM_KEY_UP}, {"Down", VTERM_KEY_DOWN}, {"Left", VTERM_KEY_LEFT}, {"Right", VTERM_KEY_RIGHT},
    {"Insert", VTERM_KEY_INS}, {"Delete", VTERM_KEY_DEL}, {"Del", VTERM_KEY_DEL},
    {"Home", VTERM_KEY_HOME}, {"End", VTERM_KEY_END},
    {"PageUp", VTERM_KEY_PAGEUP}, {"PgUp", VTERM_KEY_PAGEUP},
    {"PageDown", VTERM_KEY_PAGEDOWN}, {"PgDn", VTERM_KEY_PAGEDOWN},
};

static int parse_modifiers(const char **name) {
    int mod = 0;
    const char *p = *name;
    while (p[0] && p[1] == '-' && p[2]) {
        switch (p[0]) {
        case 'C': case 'c': mod |= VTERM_MOD_CTRL; break;
        case 'M': case 'm': case 'A': case 'a': mod |= VTERM_MOD_ALT; break;
        case 'S': case 's': mod |= VTERM_MOD_SHIFT; break;
        default: *name = p; return mod;
        }
        p += 2;
    }
    *name = p;
    return mod;
}

/* Decode one UTF-8 codepoint; returns its length, or 0 when s is not exactly
 * one well-formed codepoint followed by NUL. */
static size_t single_codepoint(const char *s, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    size_t len;
    uint32_t c;
    if (u[0] < 0x80) { len = 1; c = u[0]; }
    else if ((u[0] & 0xe0) == 0xc0) { len = 2; c = u[0] & 0x1f; }
    else if ((u[0] & 0xf0) == 0xe0) { len = 3; c = u[0] & 0x0f; }
    else if ((u[0] & 0xf8) == 0xf0) { len = 4; c = u[0] & 0x07; }
    else return 0;
    for (size_t i = 1; i < len; i++) {
        if ((u[i] & 0xc0) != 0x80) return 0;
        c = (c << 6) | (u[i] & 0x3f);
    }
    if (u[len] != '\0') return 0;
    *cp = c;
    return len;
}

int hold_screen_send_key(struct hold_screen *s, const char *name) {
    if (!s || !name || !*name) return -1;
    int mod = parse_modifiers(&name);
    for (size_t i = 0; i < sizeof(named_keys) / sizeof(named_keys[0]); i++) {
        if (!strcasecmp(name, named_keys[i].name)) {
            vterm_keyboard_key(s->vt, named_keys[i].key, (VTermModifier)mod);
            return 0;
        }
    }
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        char *end = NULL;
        long n = strtol(name + 1, &end, 10);
        if (end && *end == '\0' && n >= 1 && n <= 24) {
            vterm_keyboard_key(s->vt, VTERM_KEY_FUNCTION((int)n), (VTermModifier)mod);
            return 0;
        }
    }
    uint32_t cp = 0;
    if (!strcasecmp(name, "Space")) cp = ' ';
    else if (!single_codepoint(name, &cp)) return -1;
    /* Classic control bytes for Ctrl (C-c is 0x03, C-m is CR, C-[ is ESC);
     * libvterm would send CSI-u for some, which most programs do not read. */
    if ((mod & VTERM_MOD_CTRL) && !(mod & VTERM_MOD_SHIFT) &&
        (cp == ' ' || cp == '@' || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
         (cp >= '[' && cp <= '_') || cp == '?')) {
        char seq[2];
        size_t n = 0;
        if (mod & VTERM_MOD_ALT) seq[n++] = 0x1b;
        if (cp == '?') seq[n++] = 0x7f;
        else if (cp == ' ' || cp == '@') seq[n++] = 0x00;
        else seq[n++] = (char)((cp >= 'a' && cp <= 'z' ? cp - 32 : cp) & 0x1f);
        screen_output_cb(seq, n, s);
        return 0;
    }
    vterm_keyboard_unichar(s->vt, cp, (VTermModifier)mod);
    return 0;
}

void hold_screen_send_paste(struct hold_screen *s, const char *text, size_t n) {
    vterm_keyboard_start_paste(s->vt); /* emits ESC[200~ only in mode 2004 */
    screen_output_cb(text, n, s);
    vterm_keyboard_end_paste(s->vt);
}

int hold_screen_send_mouse(struct hold_screen *s, const char *action, int button, int row, int col) {
    if (!s->state->mouse_flags) return 1;
    int rows = 0, cols = 0;
    vterm_get_size(s->vt, &rows, &cols);
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (row >= rows) row = rows - 1;
    if (col >= cols) col = cols - 1;
    /* The legacy byte encoding stops at cell 223 (one byte, offset 33);
     * libvterm clamps silently, so report it. */
    bool clamped = s->state->mouse_protocol == MOUSE_X10 && (col > 222 || row > 222);
    if (button < 1 || button > 3) button = 1;
    if (!strcmp(action, "scroll-up") || !strcmp(action, "scroll-down")) {
        vterm_mouse_move(s->vt, row, col, VTERM_MOD_NONE);
        int b = action[7] == 'u' ? 4 : 5;
        vterm_mouse_button(s->vt, b, true, VTERM_MOD_NONE);
        return clamped ? 2 : 0;
    }
    vterm_mouse_move(s->vt, row, col, VTERM_MOD_NONE);
    if (!strcmp(action, "press")) vterm_mouse_button(s->vt, button, true, VTERM_MOD_NONE);
    else if (!strcmp(action, "release")) vterm_mouse_button(s->vt, button, false, VTERM_MOD_NONE);
    else if (!strcmp(action, "click")) {
        vterm_mouse_button(s->vt, button, true, VTERM_MOD_NONE);
        vterm_mouse_button(s->vt, button, false, VTERM_MOD_NONE);
    } else if (!strcmp(action, "move") || !strcmp(action, "drag")) {
        /* the move above already reported motion (drag if a button is held) */
    } else {
        return -1;
    }
    return clamped ? 2 : 0;
}

/* ---- snapshots ----------------------------------------------------------- */

static void color_rgb(const VTermScreen *vs, VTermColor c, uint8_t out[3]) {
    vterm_screen_convert_color_to_rgb(vs, &c);
    out[0] = c.rgb.red;
    out[1] = c.rgb.green;
    out[2] = c.rgb.blue;
}

int hold_screen_snapshot(const struct hold_screen *s, struct hold_snapshot *out) {
    memset(out, 0, sizeof(*out));
    int rows = 0, cols = 0;
    vterm_get_size(s->vt, &rows, &cols);
    out->rows = (uint16_t)rows;
    out->cols = (uint16_t)cols;
    out->cells = calloc((size_t)rows * (size_t)cols, sizeof(*out->cells));
    if (!out->cells) return -1;
    VTermPos cur;
    vterm_state_get_cursorpos(s->state, &cur);
    out->cursor_row = (uint16_t)(cur.row < 0 ? 0 : cur.row);
    out->cursor_col = (uint16_t)(cur.col < 0 ? 0 : (cur.col >= cols ? cols - 1 : cur.col));
    hold_screen_modes(s, &out->modes);
    memcpy(out->title, s->title, sizeof(out->title));
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            VTermScreenCell vc;
            struct hold_cell *hc = &out->cells[(size_t)r * cols + c];
            if (!vterm_screen_get_cell(s->vs, (VTermPos){.row = r, .col = c}, &vc)) continue;
            if (vc.chars[0] == (uint32_t)-1) {
                hc->width = 0;
            } else {
                hc->width = (uint8_t)vc.width;
                for (int i = 0; i < HOLD_CELL_CHARS && i < VTERM_MAX_CHARS_PER_CELL && vc.chars[i]; i++)
                    hc->ch[i] = vc.chars[i];
            }
            if (vc.attrs.bold) hc->attrs |= HOLD_ATTR_BOLD;
            if (vc.attrs.italic) hc->attrs |= HOLD_ATTR_ITALIC;
            if (vc.attrs.underline) hc->attrs |= HOLD_ATTR_UNDERLINE;
            if (vc.attrs.reverse) hc->attrs |= HOLD_ATTR_REVERSE;
            if (vc.attrs.strike) hc->attrs |= HOLD_ATTR_STRIKE;
            if (vc.attrs.blink) hc->attrs |= HOLD_ATTR_BLINK;
            if (vc.attrs.conceal) hc->attrs |= HOLD_ATTR_CONCEAL;
            if (VTERM_COLOR_IS_DEFAULT_FG(&vc.fg)) hc->flags |= HOLD_CELL_FG_DEFAULT;
            if (VTERM_COLOR_IS_DEFAULT_BG(&vc.bg)) hc->flags |= HOLD_CELL_BG_DEFAULT;
            color_rgb(s->vs, vc.fg, hc->fg);
            color_rgb(s->vs, vc.bg, hc->bg);
        }
    }
    return 0;
}

void hold_snapshot_free(struct hold_snapshot *snap) {
    if (!snap) return;
    free(snap->cells);
    snap->cells = NULL;
}

/* ---- HSNAP1 wire format (little-endian) ----------------------------------
 *   "HSNAP1\0\0" | u16 rows cols cursor_row cursor_col | u8 mode_bits mouse
 *   | u64 output_seq | u32 idle_ms | u8 meta_bits | i32 exit_code
 *   | u16 mouse_row mouse_col | u16 title_len, title | cells:
 *   rows*cols x (u32 ch[3] | u8 width attrs flags | u8 fg[3] bg[3]) */

#define SNAP_MAGIC "HSNAP1\0\0"
#define SNAP_MAGIC_LEN 8
#define SNAP_CELL_BYTES (4 * HOLD_CELL_CHARS + 3 + 6)

struct wbuf {
    unsigned char *p;
    size_t len;
};

static void put8(struct wbuf *w, uint8_t v) { w->p[w->len++] = v; }
static void put16(struct wbuf *w, uint16_t v) { put8(w, (uint8_t)v); put8(w, (uint8_t)(v >> 8)); }
static void put32(struct wbuf *w, uint32_t v) { put16(w, (uint16_t)v); put16(w, (uint16_t)(v >> 16)); }
static void put64(struct wbuf *w, uint64_t v) { put32(w, (uint32_t)v); put32(w, (uint32_t)(v >> 32)); }

int hold_snapshot_serialize(const struct hold_snapshot *snap, unsigned char **out, size_t *out_len) {
    size_t ncells = (size_t)snap->rows * snap->cols;
    size_t title_len = strnlen(snap->title, sizeof(snap->title) - 1);
    size_t cap = SNAP_MAGIC_LEN + 8 + 2 + 8 + 4 + 1 + 4 + 4 + 2 + title_len + ncells * SNAP_CELL_BYTES;
    struct wbuf w = {malloc(cap), 0};
    if (!w.p) return -1;
    memcpy(w.p, SNAP_MAGIC, SNAP_MAGIC_LEN);
    w.len = SNAP_MAGIC_LEN;
    put16(&w, snap->rows);
    put16(&w, snap->cols);
    put16(&w, snap->cursor_row);
    put16(&w, snap->cursor_col);
    const struct hold_screen_modes *m = &snap->modes;
    put8(&w, (uint8_t)(m->cursor_visible | m->alt_screen << 1 | m->app_cursor_keys << 2 |
                       m->app_keypad << 3 | m->bracketed_paste << 4 | m->focus_report << 5 |
                       m->mouse_sgr << 6));
    put8(&w, m->mouse);
    const struct hold_screen_meta *mt = &snap->meta;
    put64(&w, mt->output_seq);
    put32(&w, mt->idle_ms);
    put8(&w, (uint8_t)(mt->running | mt->has_exit_code << 1 | mt->has_mouse << 2 | mt->size_by_agent << 3));
    put32(&w, (uint32_t)mt->exit_code);
    put16(&w, mt->mouse_row);
    put16(&w, mt->mouse_col);
    put16(&w, (uint16_t)title_len);
    memcpy(w.p + w.len, snap->title, title_len);
    w.len += title_len;
    for (size_t i = 0; i < ncells; i++) {
        const struct hold_cell *c = &snap->cells[i];
        for (int k = 0; k < HOLD_CELL_CHARS; k++) put32(&w, c->ch[k]);
        put8(&w, c->width);
        put8(&w, c->attrs);
        put8(&w, c->flags);
        for (int k = 0; k < 3; k++) put8(&w, c->fg[k]);
        for (int k = 0; k < 3; k++) put8(&w, c->bg[k]);
    }
    *out = w.p;
    *out_len = w.len;
    return 0;
}

struct rbuf {
    const unsigned char *p;
    size_t len, pos;
    bool bad;
};

static uint8_t get8(struct rbuf *r) {
    if (r->pos + 1 > r->len) { r->bad = true; return 0; }
    return r->p[r->pos++];
}
static uint16_t get16(struct rbuf *r) { uint16_t lo = get8(r); return (uint16_t)(lo | (uint16_t)get8(r) << 8); }
static uint32_t get32(struct rbuf *r) { uint32_t lo = get16(r); return lo | (uint32_t)get16(r) << 16; }
static uint64_t get64(struct rbuf *r) { uint64_t lo = get32(r); return lo | (uint64_t)get32(r) << 32; }

int hold_snapshot_parse(const unsigned char *buf, size_t len, struct hold_snapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    if (len < SNAP_MAGIC_LEN || memcmp(buf, SNAP_MAGIC, SNAP_MAGIC_LEN) != 0) {
        errno = EINVAL;
        return -1;
    }
    struct rbuf r = {buf, len, SNAP_MAGIC_LEN, false};
    snap->rows = get16(&r);
    snap->cols = get16(&r);
    snap->cursor_row = get16(&r);
    snap->cursor_col = get16(&r);
    uint8_t mb = get8(&r);
    snap->modes.cursor_visible = mb & 1;
    snap->modes.alt_screen = mb >> 1 & 1;
    snap->modes.app_cursor_keys = mb >> 2 & 1;
    snap->modes.app_keypad = mb >> 3 & 1;
    snap->modes.bracketed_paste = mb >> 4 & 1;
    snap->modes.focus_report = mb >> 5 & 1;
    snap->modes.mouse_sgr = mb >> 6 & 1;
    snap->modes.mouse = get8(&r);
    snap->meta.output_seq = get64(&r);
    snap->meta.idle_ms = get32(&r);
    uint8_t xb = get8(&r);
    snap->meta.running = xb & 1;
    snap->meta.has_exit_code = xb >> 1 & 1;
    snap->meta.has_mouse = xb >> 2 & 1;
    snap->meta.size_by_agent = xb >> 3 & 1;
    snap->meta.exit_code = (int32_t)get32(&r);
    snap->meta.mouse_row = get16(&r);
    snap->meta.mouse_col = get16(&r);
    uint16_t tl = get16(&r);
    if (r.bad || r.pos + tl > len || tl >= sizeof(snap->title) || snap->rows == 0 || snap->cols == 0 ||
        snap->rows > 1000 || snap->cols > 1000) {
        errno = EINVAL;
        return -1;
    }
    memcpy(snap->title, buf + r.pos, tl);
    snap->title[tl] = '\0';
    r.pos += tl;
    size_t ncells = (size_t)snap->rows * snap->cols;
    if (len - r.pos != ncells * SNAP_CELL_BYTES) {
        errno = EINVAL;
        return -1;
    }
    snap->cells = calloc(ncells, sizeof(*snap->cells));
    if (!snap->cells) return -1;
    for (size_t i = 0; i < ncells; i++) {
        struct hold_cell *c = &snap->cells[i];
        for (int k = 0; k < HOLD_CELL_CHARS; k++) c->ch[k] = get32(&r);
        c->width = get8(&r);
        c->attrs = get8(&r);
        c->flags = get8(&r);
        for (int k = 0; k < 3; k++) c->fg[k] = get8(&r);
        for (int k = 0; k < 3; k++) c->bg[k] = get8(&r);
    }
    if (snap->cursor_row >= snap->rows) snap->cursor_row = (uint16_t)(snap->rows - 1);
    if (snap->cursor_col >= snap->cols) snap->cursor_col = (uint16_t)(snap->cols - 1);
    return 0;
}

/* ---- text ---------------------------------------------------------------- */

size_t hold_utf8_encode(uint32_t cp, char *out, size_t n) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp < 0x80) {
        if (n < 1) return 0;
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (n < 2) return 0;
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        if (n < 3) return 0;
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    if (n < 4) return 0;
    out[0] = (char)(0xf0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

void hold_snapshot_row_text(const struct hold_snapshot *snap, int row, char *out, size_t n) {
    size_t len = 0, keep = 0;
    if (n == 0) return;
    for (int c = 0; c < snap->cols && row >= 0 && row < snap->rows; c++) {
        const struct hold_cell *cell = &snap->cells[(size_t)row * snap->cols + c];
        if (cell->width == 0) continue;
        if (cell->ch[0] == 0) {
            if (len + 1 >= n) break;
            out[len++] = ' ';
            continue;
        }
        for (int k = 0; k < HOLD_CELL_CHARS && cell->ch[k]; k++) {
            size_t w = hold_utf8_encode(cell->ch[k], out + len, n - 1 - len);
            if (w == 0) goto done;
            len += w;
        }
        if (cell->ch[0] != ' ') keep = len; /* explicit trailing spaces trim too */
    }
done:
    out[keep] = '\0';
}

int hold_snapshot_write_text(const struct hold_snapshot *snap, FILE *f, bool ruler) {
    size_t cap = (size_t)snap->cols * 4 * HOLD_CELL_CHARS + 1;
    char *line = malloc(cap);
    if (!line) return -1;
    int digits = snap->rows > 100 ? 3 : 2;
    if (ruler) {
        fprintf(f, "%*s ", digits, "");
        for (int c = 0; c < snap->cols; c++) fputc(c % 10 == 0 ? '0' + (c / 10) % 10 : ' ', f);
        fprintf(f, "\n%*s ", digits, "");
        for (int c = 0; c < snap->cols; c++) fputc('0' + c % 10, f);
        fputc('\n', f);
    }
    for (int r = 0; r < snap->rows; r++) {
        hold_snapshot_row_text(snap, r, line, cap);
        if (ruler) fprintf(f, "%*d|", digits, r);
        fputs(line, f);
        fputc('\n', f);
    }
    free(line);
    return ferror(f) ? -1 : 0;
}

/* ---- repaint ------------------------------------------------------------- */

static int buf_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 4096;
        while (nc < *len + n + 1) nc *= 2;
        char *p = realloc(*buf, nc);
        if (!p) return -1;
        *buf = p;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int buf_printf(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    return buf_append(buf, len, cap, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

static bool same_pen(const struct hold_cell *a, const struct hold_cell *b) {
    return a->attrs == b->attrs && a->flags == b->flags && !memcmp(a->fg, b->fg, 3) && !memcmp(a->bg, b->bg, 3);
}

static int emit_pen(const struct hold_cell *c, char **buf, size_t *len, size_t *cap) {
    char sgr[96];
    int n = snprintf(sgr, sizeof(sgr), "\033[0");
    if (c->attrs & HOLD_ATTR_BOLD) n += snprintf(sgr + n, sizeof(sgr) - n, ";1");
    if (c->attrs & HOLD_ATTR_ITALIC) n += snprintf(sgr + n, sizeof(sgr) - n, ";3");
    if (c->attrs & HOLD_ATTR_UNDERLINE) n += snprintf(sgr + n, sizeof(sgr) - n, ";4");
    if (c->attrs & HOLD_ATTR_BLINK) n += snprintf(sgr + n, sizeof(sgr) - n, ";5");
    if (c->attrs & HOLD_ATTR_REVERSE) n += snprintf(sgr + n, sizeof(sgr) - n, ";7");
    if (c->attrs & HOLD_ATTR_CONCEAL) n += snprintf(sgr + n, sizeof(sgr) - n, ";8");
    if (c->attrs & HOLD_ATTR_STRIKE) n += snprintf(sgr + n, sizeof(sgr) - n, ";9");
    if (!(c->flags & HOLD_CELL_FG_DEFAULT))
        n += snprintf(sgr + n, sizeof(sgr) - n, ";38;2;%u;%u;%u", c->fg[0], c->fg[1], c->fg[2]);
    if (!(c->flags & HOLD_CELL_BG_DEFAULT))
        n += snprintf(sgr + n, sizeof(sgr) - n, ";48;2;%u;%u;%u", c->bg[0], c->bg[1], c->bg[2]);
    n += snprintf(sgr + n, sizeof(sgr) - n, "m");
    return buf_append(buf, len, cap, sgr, (size_t)n < sizeof(sgr) ? (size_t)n : sizeof(sgr) - 1);
}

int hold_snapshot_repaint(const struct hold_snapshot *snap, char **buf, size_t *len, size_t *cap) {
    static const char clear[] = "\033[0m\033[H\033[2J";
    if (buf_append(buf, len, cap, clear, sizeof(clear) - 1) != 0) return -1;
    for (int r = 0; r < snap->rows; r++) {
        const struct hold_cell *pen = NULL;
        /* Stop at the last non-blank default cell: the clear covers the rest. */
        int last = snap->cols - 1;
        while (last >= 0) {
            const struct hold_cell *c = &snap->cells[(size_t)r * snap->cols + last];
            if (c->ch[0] != 0 || c->width == 0 || c->attrs || (c->flags & HOLD_CELL_BG_DEFAULT) == 0) break;
            last--;
        }
        if (last < 0) continue; /* blank row: the clear already drew it */
        if (buf_printf(buf, len, cap, "\033[%d;1H", r + 1) != 0) return -1;
        for (int col = 0; col <= last; col++) {
            const struct hold_cell *c = &snap->cells[(size_t)r * snap->cols + col];
            if (c->width == 0) continue;
            if (!pen || !same_pen(pen, c)) {
                if (emit_pen(c, buf, len, cap) != 0) return -1;
                pen = c;
            }
            if (c->ch[0] == 0) {
                if (buf_append(buf, len, cap, " ", 1) != 0) return -1;
                continue;
            }
            for (int k = 0; k < HOLD_CELL_CHARS && c->ch[k]; k++) {
                char u[4];
                size_t w = hold_utf8_encode(c->ch[k], u, sizeof(u));
                if (buf_append(buf, len, cap, u, w) != 0) return -1;
            }
        }
        if (pen && buf_append(buf, len, cap, "\033[0m", 4) != 0) return -1;
    }
    const struct hold_screen_modes *m = &snap->modes;
    if (buf_printf(buf, len, cap, "\033[?1%c\033%c\033[?2004%c\033[?1004%c",
                   m->app_cursor_keys ? 'h' : 'l', m->app_keypad ? '=' : '>',
                   m->bracketed_paste ? 'h' : 'l', m->focus_report ? 'h' : 'l') != 0)
        return -1;
    if (m->mouse != HOLD_MOUSE_NONE &&
        buf_printf(buf, len, cap, "\033[?%dh%s",
                   m->mouse == HOLD_MOUSE_MOVE ? 1003 : m->mouse == HOLD_MOUSE_DRAG ? 1002 : 1000,
                   m->mouse_sgr ? "\033[?1006h" : "") != 0)
        return -1;
    return buf_printf(buf, len, cap, "\033[%d;%dH\033[?25%c", snap->cursor_row + 1, snap->cursor_col + 1,
                      m->cursor_visible ? 'h' : 'l');
}
