#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"
#include "hold/screen.h"

#include <regex.h>

/* The screen verbs: hold peek / send / wait / resize. Each resolves one call,
 * then talks to its console broker over the control protocol. An ended call's
 * last screen is read from <id>.screen beside its log. */

#define WAIT_EXIT_TIMEOUT 7
#define WAIT_EXIT_ENDED 8
#define SEND_EXIT_MOUSE_OFF 9 /* the program has mouse reporting off */

struct screen_target {
    struct hold_resolved_target target;
    struct hold_run_record r;
    enum run_state st;
    char label[ALIAS_MAX_LEN + 16];
};

static int open_screen_target(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *command,
                              const char *token,
                              struct screen_target *t) {
    bool hit = false, ok = false;
    memset(t, 0, sizeof(*t));
    int rc = hold_resolve_single(inv, user_store, system_store, command, command, token, &t->target, &hit);
    if (!hit) return rc == 0 ? 5 : rc;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&t->target, false, &t->r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    t->st = hold_eval_state(&t->r, hold_boot_id_or_null(boot));
    if (t->r.has_name && t->r.name[0]) snprintf(t->label, sizeof(t->label), "%s", t->r.name);
    else snprintf(t->label, sizeof(t->label), "%.12s", t->r.id);
    return 0;
}

static void close_screen_target(struct screen_target *t) {
    hold_free_run_record(&t->r);
}

static int no_screen(const struct screen_target *t) {
    fprintf(stderr, "hold: %s has no terminal screen; place TTY calls with -t (e.g. hold -dt <cmd>)\n", t->label);
    return 5;
}

/* Send one control request; *reply is the malloc'd reply. rc 0 when the
 * broker answered, else a nonzero exit status (message printed). */
static int control(const struct screen_target *t, const char *req, size_t len, unsigned char **reply, size_t *reply_len) {
    if (hold_console_request(t->r.console_sock, req, len, reply, reply_len) != 0) {
        fprintf(stderr, "hold: cannot reach %s's console: %s\n", t->label, strerror(errno));
        return 3;
    }
    return 0;
}

static int reply_error(const struct screen_target *t, const unsigned char *reply) {
    const char *msg = (const char *)reply;
    if (!strncmp(msg, "error ", 6)) msg += 6;
    fprintf(stderr, "hold: %s: %.*s\n", t->label, (int)strcspn(msg, "\n"), msg);
    return 1;
}

static int read_saved_frame(const struct screen_target *t, const char *suffix, struct hold_snapshot *snap) {
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s%s", t->target.store.log_dir, t->r.id, suffix) != 0) return -1;
    char *buf = NULL;
    if (hold_read_owned_file_no_symlink(path, &buf) != 0 || !buf) return -1;
    /* read_owned_file NUL-terminates; the snapshot is binary, so take its size. */
    struct stat st;
    int rc = stat(path, &st) == 0 ? hold_snapshot_parse((unsigned char *)buf, (size_t)st.st_size, snap) : -1;
    free(buf);
    return rc;
}

static int read_saved_screen(const struct screen_target *t, struct hold_snapshot *snap) {
    return read_saved_frame(t, ".screen", snap);
}

/* Live screen from the broker, else the saved final frame. With last_alt,
 * the last full-screen (alternate) frame the program left instead. */
static int load_snapshot(const struct screen_target *t, struct hold_snapshot *snap, bool last_alt) {
    if (!t->r.has_console) return no_screen(t);
    if (t->st == STATE_RUNNING) {
        unsigned char *reply = NULL;
        size_t len = 0;
        const char *req = last_alt ? "peek alt=1" : "peek";
        if (hold_console_request(t->r.console_sock, req, strlen(req), &reply, &len) == 0) {
            int rc = 0;
            if (len >= 3 && !memcmp(reply, "ok\n", 3)) {
                if (hold_snapshot_parse(reply + 3, len - 3, snap) != 0) {
                    fprintf(stderr, "hold: %s: malformed screen snapshot\n", t->label);
                    rc = 3;
                }
            } else {
                rc = reply_error(t, reply);
            }
            free(reply);
            return rc;
        }
        /* The call may have just ended: fall through to the saved frame. */
    }
    if (last_alt) {
        if (read_saved_frame(t, ".altscreen", snap) == 0) return 0;
        fprintf(stderr, "hold: %s left no full-screen (alternate) frame\n", t->label);
        return 5;
    }
    if (read_saved_screen(t, snap) == 0) {
        struct hold_snapshot alt;
        if (read_saved_frame(t, ".altscreen", &alt) == 0) {
            hold_snapshot_free(&alt);
            fprintf(stderr, "hold: note: %s's last full-screen frame is shown by hold peek %s --last-alt\n",
                    t->label, t->label);
        }
        return 0;
    }
    fprintf(stderr, "hold: %s has ended and left no saved screen - see 'hold logs %s'\n", t->label, t->label);
    return 5;
}

/* "300", "300ms", "2s", "1.5s", "1m" -> milliseconds; -1 on garbage. */
static long parse_duration_ms(const char *s) {
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || errno || v < 0) return -1;
    double mult = 1;
    if (!*end || !strcmp(end, "ms")) mult = 1;
    else if (!strcmp(end, "s")) mult = 1000;
    else if (!strcmp(end, "m")) mult = 60000;
    else return -1;
    double ms = v * mult;
    return ms > 86400000.0 ? -1 : (long)ms;
}

static int need_value(int i, int argc, const char *flag) {
    if (i + 1 >= argc) {
        fprintf(stderr, "hold: error: %s needs a value\n", flag);
        return -1;
    }
    return 0;
}

/* "--flag=value" -> "--flag" "value" for the screen verbs' value flags, so
 * both spellings work. Returns a new argv (caller frees the array only). */
static char **split_eq_flags(int argc, char **argv, int *out_argc) {
    char **out = calloc((size_t)argc * 2 + 1, sizeof(*out));
    int n = 0;
    if (!out) {
        *out_argc = argc;
        return NULL;
    }
    for (int i = 0; i < argc; i++) {
        char *eq = (argv[i][0] == '-' && argv[i][1] == '-') ? strchr(argv[i], '=') : NULL;
        if (eq && eq > argv[i] + 2) {
            *eq = '\0'; /* argv strings are writable */
            out[n++] = argv[i];
            out[n++] = eq + 1;
        } else {
            out[n++] = argv[i];
        }
    }
    *out_argc = n;
    return out;
}

/* ---- peek ----------------------------------------------------------------- */

static const char *mouse_mode_name(uint8_t m) {
    switch (m) {
    case HOLD_MOUSE_CLICK: return "click";
    case HOLD_MOUSE_DRAG: return "drag";
    case HOLD_MOUSE_MOVE: return "move";
    default: return "none";
    }
}

static void write_peek_json(const struct screen_target *t, const struct hold_snapshot *s, const char *image, FILE *f) {
    fprintf(f, "{\n  \"call\": \"%s\",\n  \"name\": ", t->r.id);
    if (t->r.has_name && t->r.name[0]) {
        fputc('"', f);
        hold_json_escape(f, t->r.name);
        fputc('"', f);
    } else {
        fputs("null", f);
    }
    fprintf(f, ",\n  \"running\": %s,\n  \"exit_code\": ", s->meta.running ? "true" : "false");
    if (s->meta.has_exit_code) fprintf(f, "%d", s->meta.exit_code);
    else fputs("null", f);
    fprintf(f, ",\n  \"rows\": %u,\n  \"cols\": %u,\n", s->rows, s->cols);
    fprintf(f, "  \"cursor\": {\"row\": %u, \"col\": %u, \"visible\": %s},\n", s->cursor_row, s->cursor_col,
            s->modes.cursor_visible ? "true" : "false");
    fputs("  \"title\": \"", f);
    hold_json_escape(f, s->title);
    fprintf(f, "\",\n  \"alt_screen\": %s,\n", s->modes.alt_screen ? "true" : "false");
    fprintf(f, "  \"mouse\": \"%s\",\n", mouse_mode_name(s->modes.mouse));
    fprintf(f, "  \"bracketed_paste\": %s,\n", s->modes.bracketed_paste ? "true" : "false");
    fprintf(f, "  \"cursor_keys\": \"%s\",\n", s->modes.app_cursor_keys ? "application" : "normal");
    fprintf(f, "  \"output_seq\": %" PRIu64 ",\n  \"idle_ms\": %u,\n", s->meta.output_seq, s->meta.idle_ms);
    fprintf(f, "  \"size_set_by\": \"%s\",\n", s->meta.size_by_agent ? "resize" : "terminal");
    if (s->meta.has_mouse) fprintf(f, "  \"last_mouse\": {\"row\": %u, \"col\": %u},\n", s->meta.mouse_row, s->meta.mouse_col);
    else fputs("  \"last_mouse\": null,\n", f);
    if (image) {
        fputs("  \"image\": \"", f);
        hold_json_escape(f, image);
        fputs("\",\n", f);
    }
    fputs("  \"lines\": [", f);
    size_t cap = (size_t)s->cols * 4 * HOLD_CELL_CHARS + 1;
    char *line = malloc(cap);
    for (int r = 0; line && r < s->rows; r++) {
        hold_snapshot_row_text(s, r, line, cap);
        fputs(r ? ",\n    \"" : "\n    \"", f);
        hold_json_escape(f, line);
        fputc('"', f);
    }
    free(line);
    fputs("\n  ]\n}\n", f);
}

static const char peek_usage[] =
    "usage: hold peek <target> [--json] [--ruler] [--last-alt] [--image FILE [--no-ruler] [--cursor-ring] [--scale N]]\n";

static int peek_action_impl(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         int argc,
                         char **argv) {
    const char *token = NULL, *image = NULL;
    bool json = false, text_ruler = false, want_text = false, last_alt = false;
    struct hold_render_options ro;
    hold_render_options_init(&ro);
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--json")) json = true;
        else if (!strcmp(a, "--text")) want_text = true;
        else if (!strcmp(a, "--last-alt")) last_alt = true;
        else if (!strcmp(a, "--ruler")) text_ruler = true, ro.ruler = true;
        else if (!strcmp(a, "--no-ruler")) ro.ruler = false;
        else if (!strcmp(a, "--cursor-ring")) ro.cursor_ring = true;
        else if (!strcmp(a, "--no-mouse-marker")) ro.mouse_marker = false;
        else if (!strcmp(a, "--no-footer")) ro.footer = false;
        else if (!strcmp(a, "--image") || !strcmp(a, "-o")) {
            if (need_value(i, argc, a) != 0) return 1;
            image = argv[++i];
        } else if (!strcmp(a, "--scale")) {
            if (need_value(i, argc, a) != 0) return 1;
            ro.scale = atoi(argv[++i]);
            if (ro.scale < 1 || ro.scale > 4) {
                fprintf(stderr, "hold: error: --scale must be 1-4\n");
                return 1;
            }
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(peek_usage, stdout);
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "hold: error: unknown flag '%s'\n%s", a, peek_usage);
            return 1;
        } else if (!token) token = a;
        else {
            fputs(peek_usage, stderr);
            return 1;
        }
    }
    if (!token) {
        fputs(peek_usage, stderr);
        return 1;
    }
    struct screen_target t;
    int rc = open_screen_target(inv, user_store, system_store, "peek", token, &t);
    if (rc != 0) return rc;
    struct hold_snapshot snap;
    rc = load_snapshot(&t, &snap, last_alt);
    if (rc != 0) {
        close_screen_target(&t);
        return rc;
    }
    if (image) {
        FILE *f = !strcmp(image, "-") ? stdout : fopen(image, "wb");
        if (!f || hold_snapshot_write_png(&snap, &ro, f) != 0 || (f != stdout && fclose(f) != 0)) {
            fprintf(stderr, "hold: failed to write image %s: %s\n", image, strerror(errno));
            rc = 3;
        }
    }
    if (rc == 0 && (!image || want_text || json) && !(image && !strcmp(image, "-"))) {
        if (json) write_peek_json(&t, &snap, image, stdout);
        else hold_snapshot_write_text(&snap, stdout, text_ruler);
        if (fflush(stdout) != 0) rc = 3;
    }
    hold_snapshot_free(&snap);
    close_screen_target(&t);
    return rc;
}

/* ---- send ----------------------------------------------------------------- */

struct reqbuf {
    char *p;
    size_t len, cap;
};

static int rb_add(struct reqbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) return -1;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static int rb_str(struct reqbuf *b, const char *s) { return rb_add(b, s, strlen(s)); }

static int rb_hex(struct reqbuf *b, const char *s, size_t n) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        char two[2] = {hx[(unsigned char)s[i] >> 4], hx[(unsigned char)s[i] & 15]};
        if (rb_add(b, two, 2) != 0) return -1;
    }
    return 0;
}

static char *slurp_stdin(size_t *n) {
    struct reqbuf b = {0};
    char chunk[4096];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), stdin)) > 0)
        if (rb_add(&b, chunk, r) != 0) {
            free(b.p);
            return NULL;
        }
    *n = b.len;
    return b.p ? b.p : strdup("");
}

static int add_key(struct reqbuf *b, struct hold_screen *probe, const char *name) {
    if (hold_screen_send_key(probe, name) != 0) {
        fprintf(stderr, "hold: error: unknown key '%s' (try Enter, Tab, Escape, Up, F5, C-c, M-x)\n", name);
        return -1;
    }
    hold_screen_discard_output(probe);
    return rb_str(b, "key ") || rb_str(b, name) || rb_str(b, "\n") ? -1 : 0;
}

static const char send_usage[] =
    "usage: hold send <target> [TEXT]... [--text TEXT] [-k KEY]... [--keys \"K1 K2\"] [--enter] [--paste TEXT|-]\n"
    "                 [--mouse ACTION,COL,ROW[,BUTTON]] [--stdin] [--wait MS [--timeout MS]] [--seq]\n";

static int run_wait(const struct screen_target *t, const char *args, bool print_exit);

static int send_action_impl(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         int argc,
                         char **argv) {
    const char *token = NULL;
    long wait_ms = -1, timeout_ms = 10000;
    bool print_seq = false, literal = false, any_op = false, mouse_op = false;
    struct reqbuf b = {0};
    struct hold_screen *probe = hold_screen_new(24, 80);
    int rc = 1;
    if (!probe || rb_str(&b, "send\n") != 0) goto out;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!token && !literal && (a[0] != '-' || !a[1])) {
            token = a;
            continue;
        }
        if (literal || a[0] != '-' || !a[1]) {
            if (!token) {
                token = a;
                continue;
            }
            if (rb_str(&b, "text ") || rb_hex(&b, a, strlen(a)) || rb_str(&b, "\n")) goto out;
            any_op = true;
        } else if (!strcmp(a, "--")) {
            literal = true;
        } else if (!strcmp(a, "-k") || !strcmp(a, "--key")) {
            if (need_value(i, argc, a) != 0 || add_key(&b, probe, argv[++i]) != 0) goto out;
            any_op = true;
        } else if (!strcmp(a, "--keys")) {
            if (need_value(i, argc, a) != 0) goto out;
            char *list = strdup(argv[++i]);
            if (!list) goto out;
            for (char *save = NULL, *k = strtok_r(list, " ,", &save); k; k = strtok_r(NULL, " ,", &save)) {
                if (add_key(&b, probe, k) != 0) {
                    free(list);
                    goto out;
                }
                any_op = true;
            }
            free(list);
        } else if (!strcmp(a, "--text")) {
            /* literal text that may itself start with a dash */
            if (need_value(i, argc, a) != 0) goto out;
            const char *v = argv[++i];
            if (rb_str(&b, "text ") || rb_hex(&b, v, strlen(v)) || rb_str(&b, "\n")) goto out;
            any_op = true;
        } else if (!strcmp(a, "--enter")) {
            if (add_key(&b, probe, "Enter") != 0) goto out;
            any_op = true;
        } else if (!strcmp(a, "--paste") || !strcmp(a, "--stdin")) {
            bool paste = a[2] == 'p';
            const char *v = NULL;
            if (paste) {
                if (need_value(i, argc, a) != 0) goto out;
                v = argv[++i];
            }
            size_t n = 0;
            char *data = (!paste || !strcmp(v, "-")) ? slurp_stdin(&n) : strdup(v);
            if (!data) goto out;
            if (data != v && paste && strcmp(v, "-")) n = strlen(data);
            int bad = rb_str(&b, paste ? "paste " : "text ") || rb_hex(&b, data, n) || rb_str(&b, "\n");
            free(data);
            if (bad) goto out;
            any_op = true;
        } else if (!strcmp(a, "--mouse")) {
            if (need_value(i, argc, a) != 0) goto out;
            char action[32];
            int col = -1, row = -1, button = 1;
            int got = sscanf(argv[++i], "%31[^,],%d,%d,%d", action, &col, &row, &button);
            if (got < 3 || col < 0 || row < 0) {
                fprintf(stderr, "hold: error: --mouse wants ACTION,COL,ROW[,BUTTON] (e.g. click,10,4)\n");
                goto out;
            }
            char line[96];
            snprintf(line, sizeof(line), "mouse %s %d %d %d\n", action, button, row, col);
            if (rb_str(&b, line) != 0) goto out;
            any_op = mouse_op = true;
        } else if (!strcmp(a, "--wait") || !strcmp(a, "--timeout")) {
            if (need_value(i, argc, a) != 0) goto out;
            long v = parse_duration_ms(argv[++i]);
            if (v < 0) {
                fprintf(stderr, "hold: error: bad duration '%s'\n", argv[i]);
                goto out;
            }
            if (a[2] == 'w') wait_ms = v;
            else timeout_ms = v;
        } else if (!strcmp(a, "--seq")) {
            print_seq = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(send_usage, stdout);
            rc = 0;
            goto out;
        } else {
            fprintf(stderr, "hold: error: unknown flag '%s' (send text starting with - via --text)\n%s", a, send_usage);
            goto out;
        }
    }
    if (!token || !any_op) {
        fputs(send_usage, stderr);
        goto out;
    }
    struct screen_target t;
    rc = open_screen_target(inv, user_store, system_store, "send", token, &t);
    if (rc != 0) goto out;
    if (!t.r.has_console) {
        rc = no_screen(&t);
    } else if (t.st != STATE_RUNNING) {
        fprintf(stderr, "hold: %s has ended - see 'hold peek %s'\n", t.label, t.label);
        rc = WAIT_EXIT_ENDED;
    } else {
        unsigned char *reply = NULL;
        size_t len = 0;
        rc = control(&t, b.p, b.len, &reply, &len);
        if (rc == 0 && strncmp((char *)reply, "ok", 2) != 0) {
            rc = reply_error(&t, reply);
        } else if (rc == 0) {
            const char *seq = strstr((char *)reply, "seq=");
            unsigned long long sv = seq ? strtoull(seq + 4, NULL, 10) : 0;
            bool mouse_off = mouse_op && strstr((char *)reply, "mouse=off");
            if (mouse_off)
                fprintf(stderr, "hold: %s has mouse reporting off; the mouse event was not delivered\n", t.label);
            if (mouse_op && strstr((char *)reply, "mouse=clamped"))
                fprintf(stderr, "hold: note: %s uses the legacy mouse encoding, which cannot reach past column/row 222; "
                                "the event landed at the edge\n", t.label);
            if (print_seq) printf("%llu\n", sv);
            if (wait_ms >= 0) {
                char args[160];
                snprintf(args, sizeof(args), "wait idle=%ld since=%llu timeout=%ld", wait_ms ? wait_ms : 1, sv, timeout_ms);
                rc = run_wait(&t, args, false);
            }
            /* An undelivered mouse event is a failed step for a script. */
            if (mouse_off && rc == 0) rc = SEND_EXIT_MOUSE_OFF;
        }
        free(reply);
    }
    close_screen_target(&t);
out:
    free(b.p);
    hold_screen_free(probe);
    return rc;
}

/* ---- wait ----------------------------------------------------------------- */

/* Ask the broker; map its answer to an exit status. */
static int run_wait(const struct screen_target *t, const char *args, bool print_exit) {
    unsigned char *reply = NULL;
    size_t len = 0;
    int rc = control(t, args, strlen(args), &reply, &len);
    if (rc != 0) return rc;
    const char *r = (const char *)reply;
    const char *ec = strstr(r, "exit_code=");
    int code = ec ? atoi(ec + 10) : 0;
    if (!strncmp(r, "ok", 2)) {
        if (print_exit && strstr(r, "reason=exit")) printf("%d\n", code);
        rc = 0;
    } else if (!strncmp(r, "timeout", 7)) {
        fprintf(stderr, "hold: %s: wait timed out\n", t->label);
        rc = WAIT_EXIT_TIMEOUT;
    } else if (!strncmp(r, "ended", 5)) {
        fprintf(stderr, "hold: %s ended (exit code %d) before the wait condition held\n", t->label, code);
        rc = WAIT_EXIT_ENDED;
    } else {
        rc = reply_error(t, reply);
    }
    free(reply);
    return rc;
}

/* docker wait for any call (no console needed): poll the record. */
static int wait_record_exit(const struct screen_target *t, long timeout_ms) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct hold_run_record cur;
        char path[HOLD_PATH_MAX];
        if (hold_load_record_by_id(t->target.store.record_dir, t->r.id, &cur, path, sizeof(path)) != 0) {
            fprintf(stderr, "hold: %s was removed while waiting\n", t->label);
            return 5;
        }
        char boot[128];
        enum run_state st = hold_eval_state(&cur, hold_boot_id_or_null(boot));
        bool has = cur.has_exit_code;
        int code = cur.exit_code;
        hold_free_run_record(&cur);
        if (st != STATE_RUNNING && (has || st == STATE_STALE || st == STATE_FAILED)) {
            printf("%d\n", has ? code : -1);
            return 0;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (timeout_ms > 0 && elapsed >= timeout_ms) {
            fprintf(stderr, "hold: %s: wait timed out\n", t->label);
            return WAIT_EXIT_TIMEOUT;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        nanosleep(&sl, NULL);
    }
}

static const char wait_usage[] =
    "usage: hold wait <target> [--idle MS] [--match TEXT | --regex RE] [--since SEQ] [--exit] [--timeout MS]\n";

static int wait_action_impl(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         int argc,
                         char **argv) {
    const char *token = NULL, *match = NULL, *since = NULL;
    bool regex = false, want_exit = false;
    long idle = -1, timeout = -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--idle") || !strcmp(a, "--timeout")) {
            if (need_value(i, argc, a) != 0) return 1;
            long v = parse_duration_ms(argv[++i]);
            if (v < 0) {
                fprintf(stderr, "hold: error: bad duration '%s'\n", argv[i]);
                return 1;
            }
            if (a[2] == 'i') idle = v;
            else timeout = v;
        } else if (!strcmp(a, "--match") || !strcmp(a, "--regex")) {
            if (need_value(i, argc, a) != 0) return 1;
            match = argv[++i];
            regex = a[2] == 'r';
        } else if (!strcmp(a, "--since")) {
            if (need_value(i, argc, a) != 0) return 1;
            since = argv[++i];
            if (strspn(since, "0123456789") != strlen(since) || !*since) {
                fprintf(stderr, "hold: error: --since wants the number hold send --seq printed\n");
                return 1;
            }
        } else if (!strcmp(a, "--exit")) {
            want_exit = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(wait_usage, stdout);
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "hold: error: unknown flag '%s'\n%s", a, wait_usage);
            return 1;
        } else if (!token) token = a;
        else {
            fputs(wait_usage, stderr);
            return 1;
        }
    }
    if (!token) {
        fputs(wait_usage, stderr);
        return 1;
    }
    bool screen_cond = idle >= 0 || match;
    if (!screen_cond) want_exit = true;
    /* A screen condition defaults to a 30 s bound so an agent never hangs;
     * waiting for exit alone is unbounded unless --timeout says otherwise. */
    if (timeout < 0) timeout = screen_cond ? 30000 : 0;

    struct screen_target t;
    int rc = open_screen_target(inv, user_store, system_store, "wait", token, &t);
    if (rc != 0) return rc;
    if (!screen_cond) {
        rc = (t.r.has_console && t.st == STATE_RUNNING) ? -1 : wait_record_exit(&t, timeout);
        if (rc != -1) {
            close_screen_target(&t);
            return rc;
        }
    } else if (!t.r.has_console) {
        rc = no_screen(&t);
        close_screen_target(&t);
        return rc;
    }
    if (t.st != STATE_RUNNING) {
        /* Ended: a match may still hold on the final frame. */
        struct hold_snapshot snap;
        rc = WAIT_EXIT_ENDED;
        if (match && read_saved_screen(&t, &snap) == 0) {
            char *text = NULL;
            size_t n = 0;
            FILE *f = open_memstream(&text, &n);
            if (f) {
                hold_snapshot_write_text(&snap, f, false);
                fclose(f);
                if (!regex && text && strstr(text, match)) rc = 0;
                if (regex && text) {
                    regex_t re;
                    if (regcomp(&re, match, REG_EXTENDED | REG_NOSUB | REG_NEWLINE) == 0) {
                        if (regexec(&re, text, 0, NULL, 0) == 0) rc = 0;
                        regfree(&re);
                    }
                }
            }
            free(text);
            hold_snapshot_free(&snap);
        }
        if (rc != 0) fprintf(stderr, "hold: %s has already ended\n", t.label);
        close_screen_target(&t);
        return rc;
    }
    struct reqbuf b = {0};
    char num[64];
    rc = rb_str(&b, "wait");
    if (idle >= 0) {
        snprintf(num, sizeof(num), " idle=%ld", idle ? idle : 1);
        rc |= rb_str(&b, num);
    }
    if (since) rc |= rb_str(&b, " since=") | rb_str(&b, since);
    if (match) {
        rc |= rb_str(&b, regex ? " regex=1 match=" : " match=");
        rc |= rb_hex(&b, match, strlen(match));
    }
    if (want_exit) rc |= rb_str(&b, " exit=1");
    snprintf(num, sizeof(num), " timeout=%ld", timeout);
    rc |= rb_str(&b, num);
    rc = rc ? 1 : run_wait(&t, b.p, !screen_cond);
    free(b.p);
    close_screen_target(&t);
    return rc;
}

/* ---- resize --------------------------------------------------------------- */

static const char resize_usage[] = "usage: hold resize <target> <cols> <rows>   (or <cols>x<rows>)\n";

int hold_cmd_resize_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           int argc,
                           char **argv) {
    if (argc >= 1 && (!strcmp(argv[0], "-h") || !strcmp(argv[0], "--help"))) {
        fputs(resize_usage, stdout);
        return 0;
    }
    int cols = 0, rows = 0;
    char x = 0;
    if (argc == 2 && sscanf(argv[1], "%d%c%d", &cols, &x, &rows) == 3 && (x == 'x' || x == 'X')) {
    } else if (argc == 3) {
        cols = atoi(argv[1]);
        rows = atoi(argv[2]);
    } else {
        fputs(resize_usage, stderr);
        return 1;
    }
    if (cols < 1 || rows < 1 || cols > 1000 || rows > 1000) {
        fprintf(stderr, "hold: error: size must be 1-1000 columns and rows\n");
        return 1;
    }
    struct screen_target t;
    int rc = open_screen_target(inv, user_store, system_store, "resize", argv[0], &t);
    if (rc != 0) return rc;
    if (!t.r.has_console) {
        rc = no_screen(&t);
    } else if (t.st != STATE_RUNNING) {
        fprintf(stderr, "hold: %s has ended\n", t.label);
        rc = WAIT_EXIT_ENDED;
    } else {
        char req[64];
        snprintf(req, sizeof(req), "resize rows=%d cols=%d", rows, cols);
        unsigned char *reply = NULL;
        size_t len = 0;
        rc = control(&t, req, strlen(req), &reply, &len);
        if (rc == 0 && strncmp((char *)reply, "ok", 2) != 0) rc = reply_error(&t, reply);
        free(reply);
    }
    close_screen_target(&t);
    return rc;
}

/* ---- msg ------------------------------------------------------------------ */

static const char msg_usage[] =
    "usage: hold msg <target> TEXT... [--stdin] [--now] [--settle MS] [--timeout MS] [--no-enter]\n";

/* Agent-to-agent messaging: the text is typed into the target's prompt with a
 * "[msg from <sender>; reply: ...]" header, once the target's screen has been
 * quiet for --settle MS (default 1s; --now skips the wait). The sender is this
 * process's own call ($HOLD_CALL_NAME), verified by the receiving broker. */
static int msg_action_impl(const struct hold_invocation *inv,
                        const struct hold_store *user_store,
                        const struct hold_store *system_store,
                        int argc,
                        char **argv) {
    const char *token = NULL;
    long quiet = 1000, timeout = 60000;
    bool submit = true, from_stdin = false;
    struct reqbuf text = {0};
    int rc = 1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--now")) {
            quiet = 0;
        } else if (!strcmp(a, "--settle") || !strcmp(a, "--timeout")) {
            if (need_value(i, argc, a) != 0) goto out;
            long v = parse_duration_ms(argv[++i]);
            if (v < 0) {
                fprintf(stderr, "hold: error: bad duration '%s'\n", argv[i]);
                goto out;
            }
            if (a[2] == 's') quiet = v;
            else timeout = v;
        } else if (!strcmp(a, "--no-enter")) {
            submit = false;
        } else if (!strcmp(a, "--stdin")) {
            from_stdin = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(msg_usage, stdout);
            rc = 0;
            goto out;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "hold: error: unknown flag '%s'\n%s", a, msg_usage);
            goto out;
        } else if (!token) {
            token = a;
        } else {
            if ((text.len && rb_str(&text, " ")) || rb_str(&text, a)) goto out;
        }
    }
    if (from_stdin) {
        size_t n = 0;
        char *in = slurp_stdin(&n);
        if (!in) goto out;
        while (n > 0 && (in[n - 1] == '\n' || in[n - 1] == '\r')) n--;
        int bad = (text.len && rb_str(&text, " ")) || rb_add(&text, in, n);
        free(in);
        if (bad) goto out;
    }
    if (!token || text.len == 0) {
        fputs(msg_usage, stderr);
        goto out;
    }
    struct screen_target t;
    rc = open_screen_target(inv, user_store, system_store, "msg", token, &t);
    if (rc != 0) goto out;
    if (!t.r.has_console) {
        rc = no_screen(&t);
    } else if (t.st != STATE_RUNNING) {
        fprintf(stderr, "hold: %s has ended\n", t.label);
        rc = WAIT_EXIT_ENDED;
    } else {
        struct reqbuf req = {0};
        char num[96];
        snprintf(num, sizeof(num), "msg quiet=%ld timeout=%ld%s", quiet, timeout, submit ? "" : " submit=0");
        int bad = rb_str(&req, num);
        /* The exact id verifies unambiguously; the receiver shows the name. */
        const char *from = getenv("HOLD_CALL_ID");
        if (!from || !*from) from = getenv("HOLD_CALL_NAME");
        if (from && *from) bad |= rb_str(&req, " from=") | rb_hex(&req, from, strlen(from));
        bad |= rb_str(&req, "\n") | rb_hex(&req, text.p, text.len);
        unsigned char *reply = NULL;
        size_t len = 0;
        rc = bad ? 1 : control(&t, req.p, req.len, &reply, &len);
        if (rc == 0 && strncmp((char *)reply, "ok", 2) != 0) rc = reply_error(&t, reply);
        free(reply);
        free(req.p);
    }
    close_screen_target(&t);
out:
    free(text.p);
    return rc;
}

/* ---- entry points: accept --flag=value as well as --flag value ------------ */

#define SCREEN_VERB_ENTRY(verb)                                                              \
    int hold_cmd_##verb##_action(const struct hold_invocation *inv,                          \
                                 const struct hold_store *user_store,                        \
                                 const struct hold_store *system_store, int argc, char **argv) { \
        int n = 0;                                                                           \
        char **split = split_eq_flags(argc, argv, &n);                                       \
        int rc = verb##_action_impl(inv, user_store, system_store, split ? n : argc,         \
                                    split ? split : argv);                                   \
        free(split);                                                                         \
        return rc;                                                                           \
    }

SCREEN_VERB_ENTRY(peek)
SCREEN_VERB_ENTRY(send)
SCREEN_VERB_ENTRY(wait)
SCREEN_VERB_ENTRY(msg)
