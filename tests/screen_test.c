/* Unit tests for the screen layer (src/screen): the headless terminal, input
 * encoding by mode, HSNAP1 round trips, and repaint fidelity. */
#include "hold/config.h"
#include "hold/screen.h"

static int failures = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

static void feed(struct hold_screen *s, const char *bytes) {
    hold_screen_feed(s, bytes, strlen(bytes));
}

static void row_text(struct hold_screen *s, int row, char *out, size_t n) {
    struct hold_snapshot snap;
    if (hold_screen_snapshot(s, &snap) != 0) {
        out[0] = '\0';
        return;
    }
    hold_snapshot_row_text(&snap, row, out, n);
    hold_snapshot_free(&snap);
}

static size_t take(struct hold_screen *s, char *buf, size_t n) {
    size_t got = hold_screen_take_output(s, buf, n - 1);
    buf[got] = '\0';
    return got;
}

static void test_text_and_cursor(void) {
    struct hold_screen *s = hold_screen_new(5, 20);
    feed(s, "hello\r\n\033[1;31mred\033[0m wide:\xe4\xb8\xad!");
    char line[256];
    row_text(s, 0, line, sizeof(line));
    CHECK(!strcmp(line, "hello"), "row0 '%s'", line);
    row_text(s, 1, line, sizeof(line));
    CHECK(!strcmp(line, "red wide:\xe4\xb8\xad!"), "row1 '%s'", line);
    struct hold_snapshot snap;
    CHECK(hold_screen_snapshot(s, &snap) == 0, "snapshot");
    CHECK(snap.cursor_row == 1 && snap.cursor_col == 12, "cursor %u,%u", snap.cursor_row, snap.cursor_col);
    const struct hold_cell *r = &snap.cells[1 * 20 + 0];
    CHECK(r->ch[0] == 'r' && (r->attrs & HOLD_ATTR_BOLD) && !(r->flags & HOLD_CELL_FG_DEFAULT), "red cell attrs");
    CHECK(r->fg[0] > 150 && r->fg[1] < 100, "red fg %u,%u,%u", r->fg[0], r->fg[1], r->fg[2]);
    CHECK(snap.cells[1 * 20 + 9].width == 2 && snap.cells[1 * 20 + 10].width == 0, "wide char cells");
    hold_snapshot_free(&snap);
    hold_screen_free(s);
}

static void test_keys_follow_modes(void) {
    struct hold_screen *s = hold_screen_new(5, 20);
    char out[64];
    CHECK(hold_screen_send_key(s, "Up") == 0, "Up");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[A"), "normal Up '%s'", out);
    feed(s, "\033[?1h"); /* DECCKM */
    hold_screen_send_key(s, "Up");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033OA"), "app Up '%s'", out);
    hold_screen_send_key(s, "C-c");
    take(s, out, sizeof(out));
    CHECK(out[0] == 0x03 && out[1] == '\0', "C-c");
    hold_screen_send_key(s, "C-m");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\r"), "C-m is CR");
    hold_screen_send_key(s, "Enter");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\r"), "Enter is CR");
    hold_screen_send_key(s, "M-x");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033x"), "M-x");
    CHECK(hold_screen_send_key(s, "NoSuchKey") == -1, "unknown key rejected");
    hold_screen_send_key(s, "F5");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[15~"), "F5 '%s'", out);
    hold_screen_free(s);
}

static void test_paste_and_mouse(void) {
    struct hold_screen *s = hold_screen_new(10, 40);
    char out[128];
    hold_screen_send_paste(s, "a\nb", 3);
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "a\nb"), "paste without 2004 is raw");
    feed(s, "\033[?2004h");
    hold_screen_send_paste(s, "a\nb", 3);
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[200~a\nb\033[201~"), "bracketed paste '%s'", out);
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 5) == 1, "mouse off reports 1");
    feed(s, "\033[?1000h\033[?1006h");
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 5) == 0, "mouse on");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[<0;6;3M\033[<0;6;3m"), "SGR click '%s'", out);
    hold_screen_resize(s, 10, 300);
    feed(s, "\033[?1006l"); /* back to the legacy byte encoding */
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 100) == 0, "legacy encoding fits col 100");
    take(s, out, sizeof(out));
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 250) == 2, "legacy encoding past 222 reports clamping");
    take(s, out, sizeof(out));
    feed(s, "\033[?1006h");
    CHECK(hold_screen_send_mouse(s, "click", 1, 2, 250) == 0, "SGR reaches col 250");
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[<0;251;3M\033[<0;251;3m"), "SGR col 250 '%s'", out);
    struct hold_screen_modes m;
    hold_screen_modes(s, &m);
    CHECK(m.mouse == HOLD_MOUSE_CLICK && m.mouse_sgr && m.bracketed_paste, "modes reported");
    hold_screen_free(s);
}

static void test_query_reply(void) {
    struct hold_screen *s = hold_screen_new(5, 20);
    feed(s, "ab\033[6n"); /* cursor position report */
    char out[64];
    take(s, out, sizeof(out));
    CHECK(!strcmp(out, "\033[1;3R"), "CPR reply '%s'", out);
    hold_screen_free(s);
}

static void test_roundtrip_and_repaint(void) {
    struct hold_screen *s = hold_screen_new(6, 30);
    feed(s, "\033]0;my title\007\033[44;97m blue \033[0m plain\r\n\033[7minv\033[0m \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90\033[3;5H");
    feed(s, "\033[?1h\033[?2004h");
    struct hold_snapshot a, b;
    CHECK(hold_screen_snapshot(s, &a) == 0, "snapshot a");
    a.meta.output_seq = 12345;
    a.meta.running = true;
    a.meta.has_mouse = true;
    a.meta.mouse_row = 2;
    a.meta.mouse_col = 7;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    CHECK(hold_snapshot_serialize(&a, &blob, &blob_len) == 0, "serialize");
    CHECK(hold_snapshot_parse(blob, blob_len, &b) == 0, "parse");
    CHECK(b.rows == 6 && b.cols == 30 && b.cursor_row == 2 && b.cursor_col == 4, "dims/cursor");
    CHECK(!strcmp(b.title, "my title"), "title '%s'", b.title);
    CHECK(b.meta.output_seq == 12345 && b.meta.running && b.meta.has_mouse && b.meta.mouse_col == 7, "meta");
    CHECK(b.modes.app_cursor_keys && b.modes.bracketed_paste, "modes");
    CHECK(!memcmp(a.cells, b.cells, sizeof(*a.cells) * 6 * 30), "cells identical");
    hold_snapshot_free(&b);
    CHECK(hold_snapshot_parse(blob, blob_len - 1, &b) == -1, "truncated blob rejected");
    free(blob);

    char *rep = NULL;
    size_t len = 0, cap = 0;
    CHECK(hold_snapshot_repaint(&a, &rep, &len, &cap) == 0, "repaint");
    CHECK(len > 11 && !memcmp(rep, "\033[0m\033[H\033[2J", 11), "repaint starts with a full clear");
    struct hold_screen *t = hold_screen_new(6, 30);
    hold_screen_feed(t, rep, len);
    struct hold_snapshot c;
    CHECK(hold_screen_snapshot(t, &c) == 0, "snapshot c");
    bool same = true;
    for (size_t i = 0; i < 6u * 30u; i++) {
        const struct hold_cell *x = &a.cells[i], *y = &c.cells[i];
        if (x->ch[0] != y->ch[0] || x->attrs != y->attrs || memcmp(x->fg, y->fg, 3) || memcmp(x->bg, y->bg, 3)) {
            same = false;
            fprintf(stderr, "  cell %zu differs: %u/%u attrs %u/%u\n", i, x->ch[0], y->ch[0], x->attrs, y->attrs);
            break;
        }
    }
    CHECK(same, "repaint reproduces every cell");
    CHECK(c.cursor_row == 2 && c.cursor_col == 4, "repaint cursor %u,%u", c.cursor_row, c.cursor_col);
    CHECK(c.modes.app_cursor_keys && c.modes.bracketed_paste, "repaint restores modes");
    free(rep);
    hold_snapshot_free(&a);
    hold_snapshot_free(&c);
    hold_screen_free(s);
    hold_screen_free(t);
}

static void test_resize_and_text_ruler(void) {
    struct hold_screen *s = hold_screen_new(3, 12);
    feed(s, "abc");
    hold_screen_resize(s, 4, 15);
    int rows, cols;
    hold_screen_size(s, &rows, &cols);
    CHECK(rows == 4 && cols == 15, "resized %dx%d", rows, cols);
    struct hold_snapshot snap;
    hold_screen_snapshot(s, &snap);
    char *buf = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    hold_snapshot_write_text(&snap, f, true);
    fclose(f);
    CHECK(strstr(buf, "   0         1    \n   012345678901234\n 0|abc\n") == buf, "ruler text:\n%s", buf);
    free(buf);
    hold_snapshot_free(&snap);
    hold_screen_free(s);
}

int main(void) {
    test_text_and_cursor();
    test_keys_follow_modes();
    test_paste_and_mouse();
    test_query_reply();
    test_roundtrip_and_repaint();
    test_resize_and_text_ruler();
    if (failures) {
        fprintf(stderr, "screen-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("screen-test: all passed\n");
    return 0;
}
