#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/term.h"
#include "hold/screen.h"
#include "hold/console_internal.h"

#include <regex.h>

/* The console broker: owns a call's PTY master, captures every output byte to
 * the indexed log, and keeps a headless terminal (the screen layer) in step
 * with it. Clients reach it over the call's private Unix socket:
 *
 *   - one attach client at a time (hold attach / hold -it): framed or raw
 *     terminal bridge, repainted from the live screen when it connects;
 *   - any number of control clients (hold peek / send / wait / resize): one
 *     length-prefixed request, one length-prefixed reply, then close.
 *
 * When the target exits, the final screen is written beside the log so the
 * last frame stays inspectable after the socket is gone. */

#define BROKER_MAX_CONTROL 16
#define BROKER_CONTROL_MAX_REQUEST (1u << 20)
#define BROKER_WRITE_TIMEOUT_MS 5000
/* A connection silent this long is a read-only raw attach (e.g. socat -u). */
#define BROKER_SNIFF_MS 250
#define BROKER_MAX_MSGS 16
/* hold msg loop guard: at most this many deliveries per receiver per minute. */
#define BROKER_MSG_RATE 20

/* The child-mode broker's forked target group, set right after the pid is
 * reported and cleared at every reap site. The SIGTERM forwarder reads it so a
 * TERM aimed at the broker (e.g. a raw kill of the broker pid) reaches the held
 * group instead of orphaning it. */
static volatile pid_t g_broker_forward_target = 0;

static void broker_forward_term(int signo) {
    (void)signo;
    pid_t t = g_broker_forward_target;
    if (t > 1) {
        kill(-t, SIGTERM);
    }
}

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- broker context: every fd/path the broker owns, one cleanup path ---- */

struct broker {
    const struct hold_store *store;
    const char *run_id;
    const char *log_path;
    const char *sock_path; /* NULL/"" = nothing to unlink */
    int parent_pipe;
    int listener;
    int master;
    int logfd;
    int logidxfd;
    pid_t target; /* >0: our held child (waited, SIGKILLed on cleanup) */
    uid_t owner_uid;
    bool have_allowed_peer_uid;
    uid_t allowed_peer_uid;
    struct hold_term_tui_detect tui; /* tags the sidecar pty once a TUI shows */
    struct hold_screen *screen;
    uint64_t output_seq;
    int64_t last_output_ms;
    bool size_by_agent;
    bool have_mouse;
    uint16_t mouse_row, mouse_col;
    bool target_done;
    int target_status;
    uint64_t next_gen;
    struct pending_msg {
        bool used;
        char *text; /* header + body, ready to type */
        size_t len;
        bool submit;
        bool verified;
        int64_t start_ms, deadline_ms;
        uint32_t quiet_ms;
        struct control_client *waiter; /* reply here if still the same client */
        uint64_t waiter_gen;
    } msgs[BROKER_MAX_MSGS];
    int64_t sent_ms[BROKER_MSG_RATE];
    int sent_idx;
};

static void broker_cleanup_and_exit(struct broker *b, int exit_code) {
    if (b->target > 0) {
        int st = 0;
        kill(b->target, SIGKILL);
        while (waitpid(b->target, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
    if (b->parent_pipe >= 0) close(b->parent_pipe);
    if (b->listener >= 0) close(b->listener);
    if (b->master >= 0) close(b->master);
    if (b->logidxfd >= 0) close(b->logidxfd);
    if (b->logfd >= 0) close(b->logfd);
    if (b->sock_path && *b->sock_path) unlink(b->sock_path);
    hold_screen_free(b->screen);
    _exit(exit_code);
}

static void broker_fail_errno(struct broker *b, int err) {
    if (err == 0) {
        err = EIO;
    }
    (void)hold_write_all(b->parent_pipe, &err, sizeof(err));
    broker_cleanup_and_exit(b, 127);
}

static int set_fd_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    return 0;
}

/* Client sockets are nonblocking; a reader that falls behind gets a bounded
 * wait rather than an instant drop. */
static int write_all_wait(int fd, const void *buf, size_t n) {
    const char *p = buf;
    int64_t deadline = now_ms() + BROKER_WRITE_TIMEOUT_MS;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int64_t left = deadline - now_ms();
            if (left <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            (void)poll(&pfd, 1, (int)left);
            continue;
        }
        if (w == 0) errno = EIO;
        return -1;
    }
    return 0;
}

static bool authorize_client(const struct broker *b, int client) {
    uid_t peer_uid = (uid_t)-1;
    if (hold_console_peer_uid(client, &peer_uid) == 0 &&
        (peer_uid == 0 || peer_uid == b->owner_uid ||
         (b->have_allowed_peer_uid && peer_uid == b->allowed_peer_uid))) {
        return true;
    }
    static const char msg[] = "hold: console attach denied\n";
    (void)hold_write_all(client, msg, sizeof(msg) - 1);
    return false;
}

/* Everything the terminal wants sent to the program (query replies, encoded
 * input) goes to the master. While a human terminal is attached it answers
 * the program's queries itself, so the screen's replies are dropped then. */
static void flush_screen_output(struct broker *b) {
    char buf[4096];
    size_t n;
    while ((n = hold_screen_take_output(b->screen, buf, sizeof(buf))) > 0) {
        (void)hold_write_all(b->master, buf, n);
    }
}

static void apply_size(struct broker *b, int rows, int cols, bool by_agent) {
    if (rows < 1 || cols < 1 || rows > 1000 || cols > 1000) return;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    /* The kernel delivers SIGWINCH to the PTY's foreground group itself. */
    (void)ioctl(b->master, TIOCSWINSZ, &ws);
    hold_screen_resize(b->screen, rows, cols);
    b->size_by_agent = by_agent;
}

static int exit_code_of(int st) {
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 255;
}

static int take_snapshot(const struct broker *b, struct hold_snapshot *snap) {
    if (hold_screen_snapshot(b->screen, snap) != 0) return -1;
    snap->meta.output_seq = b->output_seq;
    int64_t idle = now_ms() - b->last_output_ms;
    snap->meta.idle_ms = idle < 0 ? 0 : idle > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)idle;
    snap->meta.running = !b->target_done;
    snap->meta.size_by_agent = b->size_by_agent;
    snap->meta.has_mouse = b->have_mouse;
    snap->meta.mouse_row = b->mouse_row;
    snap->meta.mouse_col = b->mouse_col;
    if (b->target_done) {
        snap->meta.has_exit_code = true;
        snap->meta.exit_code = exit_code_of(b->target_status);
    }
    return 0;
}

/* ---- the attach client: magic sniff, then 3-byte frames or raw ---------- */

struct attach_client {
    int fd;            /* -1 = no client attached */
    bool input_closed; /* client half-closed its write side */
    bool decided;
    bool framed;
    bool needs_repaint;
    unsigned char pending[16384];
    size_t pending_len;
};

static void drop_attach(struct attach_client *c) {
    if (c->fd >= 0) {
        close(c->fd);
    }
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static int process_framed(struct broker *b, struct attach_client *c) {
    while (c->pending_len >= CONSOLE_FRAME_HEADER_LEN) {
        unsigned char type = c->pending[0];
        uint16_t len = console_load_be16(c->pending + 1);
        size_t frame_len = CONSOLE_FRAME_HEADER_LEN + (size_t)len;
        if (c->pending_len < frame_len) {
            return 0;
        }
        const unsigned char *payload = c->pending + CONSOLE_FRAME_HEADER_LEN;
        if (type == CONSOLE_FRAME_DATA) {
            if (len > 0 && hold_write_all(b->master, payload, len) != 0) {
                return -1;
            }
        } else if (type == CONSOLE_FRAME_RESIZE) {
            if (len == 4) apply_size(b, console_load_be16(payload), console_load_be16(payload + 2), false);
        } else if (type == CONSOLE_FRAME_DETACH) {
            return 1;
        }
        memmove(c->pending, c->pending + frame_len, c->pending_len - frame_len);
        c->pending_len -= frame_len;
    }
    return 0;
}

/* Until CONSOLE_ATTACH_MAGIC_LEN bytes decide the mode, input is buffered; a
 * magic prefix selects framed mode, anything else replays as raw passthrough.
 * Returns nonzero to drop the client (1 = clean detach frame). */
static int process_attach_input(struct broker *b, struct attach_client *c, const unsigned char *buf, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (c->decided && !c->framed) {
        return hold_write_all(b->master, buf, n);
    }
    if (c->pending_len + n > sizeof(c->pending)) {
        if (c->decided) {
            errno = EOVERFLOW;
            return -1;
        }
        c->decided = true;
        if (c->pending_len > 0 && hold_write_all(b->master, c->pending, c->pending_len) != 0) {
            return -1;
        }
        c->pending_len = 0;
        return hold_write_all(b->master, buf, n);
    }
    memcpy(c->pending + c->pending_len, buf, n);
    c->pending_len += n;
    if (!c->decided) {
        size_t cmp = c->pending_len < CONSOLE_ATTACH_MAGIC_LEN ? c->pending_len : CONSOLE_ATTACH_MAGIC_LEN;
        if (memcmp(c->pending, CONSOLE_ATTACH_MAGIC, cmp) != 0) {
            c->decided = true;
            if (hold_write_all(b->master, c->pending, c->pending_len) != 0) {
                return -1;
            }
            c->pending_len = 0;
            return 0;
        }
        if (c->pending_len < CONSOLE_ATTACH_MAGIC_LEN) {
            return 0;
        }
        c->decided = true;
        c->framed = true;
        c->pending_len -= CONSOLE_ATTACH_MAGIC_LEN;
        memmove(c->pending, c->pending + CONSOLE_ATTACH_MAGIC_LEN, c->pending_len);
    }
    return process_framed(b, c);
}

/* A new attach client sees the live screen, not a byte replay. */
static int repaint_attach(struct broker *b, struct attach_client *c) {
    struct hold_snapshot snap;
    if (take_snapshot(b, &snap) != 0) return -1;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int rc = hold_snapshot_repaint(&snap, &buf, &len, &cap);
    hold_snapshot_free(&snap);
    if (rc == 0) rc = write_all_wait(c->fd, buf, len);
    free(buf);
    return rc;
}

/* ---- control clients: one request, one reply ---------------------------- */

enum control_phase { CTL_SNIFF = 0, CTL_READING, CTL_WAITING, CTL_MSG };

struct wait_request {
    int64_t start_ms;
    int64_t deadline_ms; /* 0 = no timeout */
    uint32_t idle_ms;    /* 0 = no idle condition */
    bool has_since;
    uint64_t since;
    bool want_exit;
    bool has_match;
    bool match_regex;
    char *match;         /* literal text, or the regex source */
    regex_t re;
    bool re_ok;
};

struct control_client {
    int fd; /* -1 = free slot */
    enum control_phase phase;
    int64_t accepted_ms;
    uint64_t gen;   /* distinguishes reuses of this slot */
    pid_t peer_pid; /* 0 = unknown */
    unsigned char *buf;
    size_t len, cap;
    struct wait_request wait;
};

static void drop_control(struct control_client *c) {
    if (c->fd >= 0) close(c->fd);
    free(c->buf);
    free(c->wait.match);
    if (c->wait.re_ok) regfree(&c->wait.re);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

/* Reply = be32 length + (header text [+ binary body]); the client is then closed. */
static void control_reply(struct control_client *c, const char *header, const void *body, size_t body_len) {
    size_t hl = strlen(header);
    unsigned char len[4];
    console_store_be32(len, (uint32_t)(hl + body_len));
    if (write_all_wait(c->fd, len, 4) == 0 && write_all_wait(c->fd, header, hl) == 0 && body_len > 0)
        (void)write_all_wait(c->fd, body, body_len);
    drop_control(c);
}

static void control_replyf(struct control_client *c, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    control_reply(c, msg, NULL, 0);
}

static int hex_value(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Decode hex in place; returns the decoded length or -1. */
static ssize_t hex_decode(char *s) {
    size_t n = strlen(s);
    if (n % 2) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hex_value((unsigned char)s[i]), lo = hex_value((unsigned char)s[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        s[i / 2] = (char)(hi << 4 | lo);
    }
    s[n / 2] = '\0';
    return (ssize_t)(n / 2);
}

/* "key=value" lookup in a space-separated argument line. */
static const char *kv_get(const char *line, const char *key) {
    size_t kl = strlen(key);
    for (const char *tok = line; tok && *tok;) {
        const char *sp = strchr(tok, ' ');
        if (!strncmp(tok, key, kl) && tok[kl] == '=') {
            return tok + kl + 1;
        }
        tok = sp ? sp + 1 : NULL;
    }
    return NULL;
}

static void handle_peek(struct broker *b, struct control_client *c) {
    struct hold_snapshot snap;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    if (take_snapshot(b, &snap) != 0) {
        control_replyf(c, "error out of memory\n");
        return;
    }
    int rc = hold_snapshot_serialize(&snap, &blob, &blob_len);
    hold_snapshot_free(&snap);
    if (rc != 0) {
        control_replyf(c, "error out of memory\n");
        return;
    }
    control_reply(c, "ok\n", blob, blob_len);
    free(blob);
}

static void handle_send(struct broker *b, struct control_client *c, char *ops) {
    uint64_t seq = b->output_seq;
    bool mouse_off = false, mouse_clamped = false;
    if (b->target_done) {
        control_replyf(c, "error the call has ended\n");
        return;
    }
    for (char *line = ops; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = '\0';
        if (!strcmp(line, "text") || !strcmp(line, "paste")) {
            ssize_t n = arg ? hex_decode(arg) : -1;
            if (n < 0) {
                control_replyf(c, "error malformed %s\n", line);
                return;
            }
            if (line[0] == 'p') {
                hold_screen_send_paste(b->screen, arg, (size_t)n);
                flush_screen_output(b);
            } else if (n > 0) {
                (void)hold_write_all(b->master, arg, (size_t)n);
            }
        } else if (!strcmp(line, "key")) {
            if (!arg || hold_screen_send_key(b->screen, arg) != 0) {
                control_replyf(c, "error unknown key '%s'\n", arg ? arg : "");
                return;
            }
            flush_screen_output(b);
        } else if (!strcmp(line, "mouse")) {
            char action[32];
            int button = 1, row = 0, col = 0;
            if (!arg || sscanf(arg, "%31s %d %d %d", action, &button, &row, &col) != 4) {
                control_replyf(c, "error malformed mouse\n");
                return;
            }
            int mr = hold_screen_send_mouse(b->screen, action, button, row, col);
            if (mr < 0) {
                control_replyf(c, "error unknown mouse action '%s'\n", action);
                return;
            }
            if (mr == 1) mouse_off = true;
            if (mr == 2) mouse_clamped = true;
            b->have_mouse = true;
            b->mouse_row = (uint16_t)(row < 0 ? 0 : row);
            b->mouse_col = (uint16_t)(col < 0 ? 0 : col);
            flush_screen_output(b);
        } else if (*line) {
            control_replyf(c, "error unknown send op '%s'\n", line);
            return;
        }
        line = nl ? nl + 1 : NULL;
    }
    control_replyf(c, "ok seq=%" PRIu64 "%s%s\n", seq, mouse_off ? " mouse=off" : "",
                   mouse_clamped ? " mouse=clamped" : "");
}

static void handle_resize(struct broker *b, struct control_client *c, const char *args) {
    const char *r = kv_get(args, "rows"), *k = kv_get(args, "cols");
    int rows = r ? atoi(r) : 0, cols = k ? atoi(k) : 0;
    if (b->target_done) {
        control_replyf(c, "error the call has ended\n");
        return;
    }
    if (rows < 1 || cols < 1 || rows > 1000 || cols > 1000) {
        control_replyf(c, "error size must be 1-1000 rows and columns\n");
        return;
    }
    apply_size(b, rows, cols, true);
    control_replyf(c, "ok rows=%d cols=%d\n", rows, cols);
}

static bool screen_matches(struct broker *b, struct wait_request *w) {
    struct hold_snapshot snap;
    if (hold_screen_snapshot(b->screen, &snap) != 0) return false;
    size_t cap = (size_t)snap.rows * ((size_t)snap.cols * 4 * HOLD_CELL_CHARS + 1) + 1;
    char *text = malloc(cap);
    bool hit = false;
    if (text) {
        size_t len = 0;
        text[0] = '\0';
        for (int r = 0; r < snap.rows; r++) {
            hold_snapshot_row_text(&snap, r, text + len, cap - len);
            len += strlen(text + len);
            if (len + 1 < cap) text[len++] = '\n';
            text[len] = '\0';
        }
        hit = w->match_regex ? (w->re_ok && regexec(&w->re, text, 0, NULL, 0) == 0) : strstr(text, w->match) != NULL;
        free(text);
    }
    hold_snapshot_free(&snap);
    return hit;
}

static void handle_wait(struct control_client *c, const char *args) {
    struct wait_request *w = &c->wait;
    memset(w, 0, sizeof(*w));
    w->start_ms = now_ms();
    const char *v;
    if ((v = kv_get(args, "idle"))) w->idle_ms = (uint32_t)strtoul(v, NULL, 10);
    if ((v = kv_get(args, "timeout"))) {
        long t = strtol(v, NULL, 10);
        if (t > 0) w->deadline_ms = w->start_ms + t;
    }
    if ((v = kv_get(args, "since"))) {
        w->has_since = true;
        w->since = strtoull(v, NULL, 10);
    }
    if ((v = kv_get(args, "exit"))) w->want_exit = *v == '1';
    if ((v = kv_get(args, "regex"))) w->match_regex = *v == '1';
    if ((v = kv_get(args, "match"))) {
        size_t vl = strcspn(v, " \n");
        w->match = strndup(v, vl);
        if (!w->match || hex_decode(w->match) < 0) {
            control_replyf(c, "error malformed match\n");
            return;
        }
        w->has_match = true;
        if (w->match_regex) {
            int rc = regcomp(&w->re, w->match, REG_EXTENDED | REG_NOSUB | REG_NEWLINE);
            if (rc != 0) {
                char err[128];
                regerror(rc, &w->re, err, sizeof(err));
                control_replyf(c, "error bad regex: %s\n", err);
                return;
            }
            w->re_ok = true;
        }
    }
    c->phase = CTL_WAITING;
}

static int64_t quiet_since(const struct broker *b, const struct wait_request *w) {
    /* Without --since, silence counts only from when the wait began, so a
     * reply to input sent just before the wait is still awaited. */
    if (w->has_since || b->last_output_ms > w->start_ms) return b->last_output_ms;
    return w->start_ms;
}

/* Check one waiter; replies (and frees the slot) when a condition holds. */
static void check_wait(struct broker *b, struct control_client *c, int64_t now) {
    struct wait_request *w = &c->wait;
    /* With --since, a match counts only on a screen changed after SEQ:
     * text that was already there is not an answer to what was just sent. */
    bool match_fresh = !w->has_since || b->output_seq > w->since;
    if (w->has_match && match_fresh && screen_matches(b, w)) {
        control_replyf(c, "ok reason=match seq=%" PRIu64 "\n", b->output_seq);
        return;
    }
    if (b->target_done) {
        bool asked = w->want_exit || (!w->has_match && !w->idle_ms);
        control_replyf(c, "%s reason=exit seq=%" PRIu64 " exit_code=%d\n", asked ? "ok" : "ended", b->output_seq,
                       exit_code_of(b->target_status));
        return;
    }
    if (w->idle_ms) {
        /* Settled means drawn, then quiet: silence before the program's first
         * output (or, with --since, before fresh output) never counts. */
        bool fresh = w->has_since ? b->output_seq > w->since : b->output_seq > 0;
        if (fresh && now - quiet_since(b, w) >= (int64_t)w->idle_ms) {
            control_replyf(c, "ok reason=idle seq=%" PRIu64 "\n", b->output_seq);
            return;
        }
    }
    if (w->deadline_ms && now >= w->deadline_ms) {
        control_replyf(c, "timeout seq=%" PRIu64 "\n", b->output_seq);
    }
}

/* Milliseconds until this waiter could next change state by time alone. */
static int64_t wait_due_in(const struct broker *b, const struct control_client *c, int64_t now) {
    const struct wait_request *w = &c->wait;
    int64_t due = INT64_MAX;
    bool fresh = w->has_since ? b->output_seq > w->since : b->output_seq > 0;
    if (w->idle_ms && fresh) due = quiet_since(b, w) + w->idle_ms - now;
    if (w->deadline_ms && w->deadline_ms - now < due) due = w->deadline_ms - now;
    return due < 0 ? 0 : due;
}

struct name_lookup {
    const char *name;
    char id[ID_STR_LEN];
    int matches;
};

static int match_record_name(const char *id, const char *path, struct hold_run_record *r, void *ctx) {
    (void)path;
    struct name_lookup *nl = ctx;
    if (r->has_name && !strcmp(r->name, nl->name)) {
        snprintf(nl->id, sizeof(nl->id), "%s", id);
        nl->matches++;
    }
    return 0;
}

/* Who is speaking: the claimed call (the sender's HOLD_CALL_ID, or a name)
 * is verified when the connecting process belongs to that call's session or
 * descends from its leader. */
static bool verify_sender(const struct broker *b, pid_t peer, const char *claim, char *label, size_t n) {
    snprintf(label, n, "%s", claim);
    if (!b->store || peer <= 0) return false;
    char id[ID_STR_LEN];
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_resolve_record_id(b->store->record_dir, claim, id, sizeof(id)) != 0) {
        struct name_lookup nl = {.name = claim};
        (void)hold_for_each_record(b->store->record_dir, match_record_name, &nl);
        if (nl.matches != 1) return false;
        memcpy(id, nl.id, sizeof(id));
    }
    if (hold_load_record_by_id(b->store->record_dir, id, &r, path, sizeof(path)) != 0) return false;
    if (r.has_name && r.name[0]) snprintf(label, n, "%s", r.name);
    /* Same session as the call, or a descendant of its leader: agent
     * harnesses (Claude Code among them) run each tool command in a new
     * session, but it still descends from the harness the call launched. */
    pid_t peer_sid = getsid(peer);
    bool ok = (peer_sid > 0 && r.sid > 0 && peer_sid == r.sid) || hold_pid_has_ancestor(peer, r.pid);
    hold_free_run_record(&r);
    return ok;
}

static void handle_msg(struct broker *b, struct control_client *c, const char *args, char *body) {
    if (b->target_done) {
        control_replyf(c, "error the call has ended\n");
        return;
    }
    int64_t now = now_ms();
    int recent = 0;
    for (int i = 0; i < BROKER_MSG_RATE; i++)
        if (b->sent_ms[i] && now - b->sent_ms[i] < 60000) recent++;
    if (recent >= BROKER_MSG_RATE) {
        control_replyf(c, "error rate limit: %d messages a minute to one call\n", BROKER_MSG_RATE);
        return;
    }
    int slot = -1;
    for (int i = 0; i < BROKER_MAX_MSGS && slot < 0; i++)
        if (!b->msgs[i].used) slot = i;
    if (slot < 0) {
        control_replyf(c, "error too many undelivered messages\n");
        return;
    }
    char *nl = strchr(body, '\n');
    if (nl) *nl = '\0';
    ssize_t blen = hex_decode(body);
    if (blen < 0) {
        control_replyf(c, "error malformed message\n");
        return;
    }
    char header[320] = "[msg via hold] ";
    bool verified = false;
    const char *v = kv_get(args, "from");
    if (v) {
        char claim[ALIAS_MAX_LEN + ID_STR_LEN + 1];
        size_t vl = strcspn(v, " ");
        if (vl / 2 < sizeof(claim) && vl > 0) {
            memcpy(claim, v, vl);
            claim[vl] = '\0';
            char label[ALIAS_MAX_LEN + ID_STR_LEN + 1];
            if (hex_decode(claim) > 0) {
                verified = verify_sender(b, c->peer_pid, claim, label, sizeof(label));
                if (verified)
                    snprintf(header, sizeof(header), "[msg from %.100s; reply: hold msg %.100s \"...\"] ", label, label);
                else
                    snprintf(header, sizeof(header), "[msg from %.100s (unverified)] ", label);
            }
        }
    }
    size_t hl = strlen(header);
    struct pending_msg *m = &b->msgs[slot];
    memset(m, 0, sizeof(*m));
    m->text = malloc(hl + (size_t)blen + 1);
    if (!m->text) {
        control_replyf(c, "error out of memory\n");
        return;
    }
    memcpy(m->text, header, hl);
    memcpy(m->text + hl, body, (size_t)blen);
    m->len = hl + (size_t)blen;
    m->text[m->len] = '\0';
    m->used = true;
    m->verified = verified;
    m->submit = !((v = kv_get(args, "submit")) && *v == '0');
    m->quiet_ms = (v = kv_get(args, "quiet")) ? (uint32_t)strtoul(v, NULL, 10) : 1000;
    long timeout = (v = kv_get(args, "timeout")) ? strtol(v, NULL, 10) : 60000;
    m->start_ms = now;
    m->deadline_ms = now + (timeout > 0 ? timeout : 60000);
    m->waiter = c;
    m->waiter_gen = c->gen;
    b->sent_ms[b->sent_idx] = now;
    b->sent_idx = (b->sent_idx + 1) % BROKER_MSG_RATE;
    c->phase = CTL_MSG;
}

/* Type a queued message: bracketed paste when the program asked for it, else
 * newlines flattened so it cannot submit early; then Enter. */
static void deliver_msg(struct broker *b, struct pending_msg *m) {
    struct hold_screen_modes modes;
    hold_screen_modes(b->screen, &modes);
    if (modes.bracketed_paste) {
        hold_screen_send_paste(b->screen, m->text, m->len);
        flush_screen_output(b);
    } else {
        for (size_t i = 0; i < m->len; i++)
            if (m->text[i] == '\n' || m->text[i] == '\r') m->text[i] = ' ';
        (void)hold_write_all(b->master, m->text, m->len);
    }
    if (m->submit) (void)hold_write_all(b->master, "\r", 1);
    struct control_client *w = m->waiter;
    if (w && w->fd >= 0 && w->gen == m->waiter_gen && w->phase == CTL_MSG)
        control_replyf(w, "ok delivered verified=%d\n", m->verified ? 1 : 0);
    free(m->text);
    memset(m, 0, sizeof(*m));
}

/* The oldest queued message goes in once the screen has been quiet for its
 * quiet period (counted from when it was queued) or its deadline passed.
 * One per pass: the program's echo then resets the quiet clock. */
static void pump_msgs(struct broker *b, int64_t now) {
    struct pending_msg *oldest = NULL;
    for (int i = 0; i < BROKER_MAX_MSGS; i++)
        if (b->msgs[i].used && (!oldest || b->msgs[i].start_ms < oldest->start_ms)) oldest = &b->msgs[i];
    if (!oldest || b->target_done) return;
    int64_t quiet_from = b->last_output_ms > oldest->start_ms ? b->last_output_ms : oldest->start_ms;
    if (now - quiet_from >= (int64_t)oldest->quiet_ms || now >= oldest->deadline_ms) deliver_msg(b, oldest);
}

static int64_t msgs_due_in(const struct broker *b, int64_t now) {
    int64_t due = INT64_MAX;
    for (int i = 0; i < BROKER_MAX_MSGS; i++) {
        const struct pending_msg *m = &b->msgs[i];
        if (!m->used) continue;
        int64_t quiet_from = b->last_output_ms > m->start_ms ? b->last_output_ms : m->start_ms;
        int64_t d = quiet_from + m->quiet_ms - now;
        if (m->deadline_ms - now < d) d = m->deadline_ms - now;
        if (d < due) due = d;
    }
    return due < 0 ? 0 : due;
}

static void dispatch_control(struct broker *b, struct control_client *c) {
    char *req = (char *)c->buf + 4;
    char *nl = strchr(req, '\n');
    char *rest = nl ? nl + 1 : req + strlen(req);
    if (nl) *nl = '\0';
    char *args = strchr(req, ' ');
    if (args) *args++ = '\0';
    else args = req + strlen(req);
    if (!strcmp(req, "peek")) handle_peek(b, c);
    else if (!strcmp(req, "send")) handle_send(b, c, rest);
    else if (!strcmp(req, "resize")) handle_resize(b, c, args);
    else if (!strcmp(req, "msg")) handle_msg(b, c, args, rest);
    else if (!strcmp(req, "wait")) {
        handle_wait(c, args);
        if (c->fd >= 0) check_wait(b, c, now_ms());
    } else control_replyf(c, "error unknown request '%s'\n", req);
}

/* Read what is available; dispatch once the length-prefixed request is whole. */
static void read_control(struct broker *b, struct control_client *c) {
    if (c->phase == CTL_WAITING || c->phase == CTL_MSG) {
        /* A waiter that hangs up is done (a queued msg is still delivered);
         * anything else it sends is ignored. */
        char sink[256];
        ssize_t n = read(c->fd, sink, sizeof(sink));
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) drop_control(c);
        return;
    }
    if (c->cap - c->len < 4096) {
        size_t cap = c->cap ? c->cap * 2 : 8192;
        unsigned char *p = realloc(c->buf, cap + 1);
        if (!p) {
            drop_control(c);
            return;
        }
        c->buf = p;
        c->cap = cap;
    }
    ssize_t n = read(c->fd, c->buf + c->len, c->cap - c->len);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        drop_control(c);
        return;
    }
    if (n < 0) return;
    c->len += (size_t)n;
    if (c->len < 4) return;
    uint32_t want = console_load_be32(c->buf);
    if (want > BROKER_CONTROL_MAX_REQUEST) {
        control_replyf(c, "error request too large\n");
        return;
    }
    if (c->len < 4 + (size_t)want) return;
    c->buf[4 + want] = '\0';
    dispatch_control(b, c);
}

/* ---- the final frame ----------------------------------------------------- */

/* <id>.screen beside <id>.log: the last screen, for hold peek after exit. */
static void write_final_screen(struct broker *b) {
    size_t ll = b->log_path ? strlen(b->log_path) : 0;
    if (ll < 5 || strcmp(b->log_path + ll - 4, ".log") != 0) return;
    char path[HOLD_PATH_MAX], tmp[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%.*s.screen", (int)(ll - 4), b->log_path) != 0 ||
        hold_checked_snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) != 0)
        return;
    struct hold_snapshot snap;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    if (take_snapshot(b, &snap) != 0) return;
    int rc = hold_snapshot_serialize(&snap, &blob, &blob_len);
    hold_snapshot_free(&snap);
    if (rc != 0) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        bool ok = hold_write_all(fd, blob, blob_len) == 0;
        if (close(fd) != 0) ok = false;
        if (!ok || rename(tmp, path) != 0) unlink(tmp);
    }
    free(blob);
}

/* ---- the one serve loop ------------------------------------------------- */

static void reap_target(struct broker *b, bool block) {
    if (b->target_done || b->target <= 0) return;
    int st = 0;
    pid_t got;
    do {
        got = waitpid(b->target, &st, block ? 0 : WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got == b->target) {
        b->target_done = true;
        b->target_status = st;
    }
    if (got == b->target || (got < 0 && errno == ECHILD)) {
        b->target = -1;
        g_broker_forward_target = 0;
    }
}

/* Route a freshly accepted connection once its first bytes are readable:
 * control magic -> control request; anything else -> the attach client. */
static void sniff_connection(struct broker *b, struct control_client *c, struct attach_client *cl) {
    unsigned char head[CONSOLE_CONTROL_MAGIC_LEN];
    ssize_t n = recv(c->fd, head, sizeof(head), MSG_PEEK);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        drop_control(c);
        return;
    }
    if (n <= 0) return;
    size_t cmp = (size_t)n < CONSOLE_CONTROL_MAGIC_LEN ? (size_t)n : CONSOLE_CONTROL_MAGIC_LEN;
    if (memcmp(head, CONSOLE_CONTROL_MAGIC, cmp) == 0) {
        if ((size_t)n < CONSOLE_CONTROL_MAGIC_LEN) return; /* the rest is coming */
        if (read(c->fd, head, sizeof(head)) != (ssize_t)sizeof(head)) {
            drop_control(c);
            return;
        }
        c->phase = CTL_READING;
        read_control(b, c);
        return;
    }
    if (cl->fd >= 0) {
        static const char msg[] = "hold: console already attached\n";
        (void)hold_write_all(c->fd, msg, sizeof(msg) - 1);
        drop_control(c);
        return;
    }
    memset(cl, 0, sizeof(*cl));
    cl->fd = c->fd;
    cl->needs_repaint = true;
    c->fd = -1;
    drop_control(c);
}

/* A connection that never spoke within BROKER_SNIFF_MS becomes a raw,
 * read-only attach so output-only clients still see the screen. */
static void sniff_timeout(struct control_client *c, struct attach_client *cl, int64_t now) {
    if (c->phase != CTL_SNIFF || now - c->accepted_ms < BROKER_SNIFF_MS) return;
    if (cl->fd >= 0) {
        static const char msg[] = "hold: console already attached\n";
        (void)hold_write_all(c->fd, msg, sizeof(msg) - 1);
        drop_control(c);
        return;
    }
    memset(cl, 0, sizeof(*cl));
    cl->fd = c->fd;
    cl->decided = true; /* raw */
    cl->needs_repaint = true;
    c->fd = -1;
    drop_control(c);
}

/* b->target is our child: waitpid lifecycle, killed on cleanup. Never returns. */
static void broker_serve(struct broker *b) {
    /* The serve loop always ends in _exit, so no handler save/restore. */
    signal(SIGPIPE, SIG_IGN);

    struct attach_client cl;
    memset(&cl, 0, sizeof(cl));
    cl.fd = -1;
    struct control_client ctl[BROKER_MAX_CONTROL];
    memset(ctl, 0, sizeof(ctl));
    for (int i = 0; i < BROKER_MAX_CONTROL; i++) ctl[i].fd = -1;
    bool target_marked = false;
    b->last_output_ms = now_ms();
    while (1) {
        if (!b->target_done) {
            reap_target(b, false);
            if (b->target_done && b->store && b->run_id && *b->run_id) {
                /* Best-effort while serving: an instantly-exiting target
                 * can beat the parent's record write, so a failure here
                 * is retried patiently after the serve loop ends. */
                target_marked = hold_mark_run_finished(b->store, b->run_id, b->target_status) == 0;
            }
        }

        struct pollfd pfds[3 + BROKER_MAX_CONTROL];
        int ctl_idx[BROKER_MAX_CONTROL];
        nfds_t nfds = 0;
        pfds[nfds++] = (struct pollfd){.fd = b->master, .events = POLLIN};
        nfds_t listener_idx = nfds;
        pfds[nfds++] = (struct pollfd){.fd = b->listener, .events = POLLIN};
        int client_idx = -1;
        if (cl.fd >= 0 && !cl.input_closed) {
            client_idx = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = cl.fd, .events = POLLIN};
        }
        int64_t now = now_ms();
        int64_t timeout = 1000;
        int64_t msg_due = msgs_due_in(b, now);
        if (msg_due < timeout) timeout = msg_due;
        for (int i = 0; i < BROKER_MAX_CONTROL; i++) {
            ctl_idx[i] = -1;
            if (ctl[i].fd < 0) continue;
            ctl_idx[i] = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = ctl[i].fd, .events = POLLIN};
            if (ctl[i].phase == CTL_WAITING) {
                int64_t due = wait_due_in(b, &ctl[i], now);
                if (due < timeout) timeout = due;
            } else if (ctl[i].phase == CTL_SNIFF) {
                int64_t due = ctl[i].accepted_ms + BROKER_SNIFF_MS - now;
                if (due < timeout) timeout = due < 0 ? 0 : due;
            }
        }

        int pr = poll(pfds, nfds, (int)timeout);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (pr > 0 && (pfds[listener_idx].revents & POLLIN)) {
            int next = accept(b->listener, NULL, NULL);
            if (next >= 0) {
                int slot = -1;
                for (int i = 0; i < BROKER_MAX_CONTROL && slot < 0; i++)
                    if (ctl[i].fd < 0) slot = i;
                if (set_fd_nonblocking(next) != 0 || !authorize_client(b, next) || slot < 0) {
                    close(next);
                } else {
                    /* Every connection starts undecided; its first bytes
                     * route it to control or attach (polled next pass). */
                    memset(&ctl[slot], 0, sizeof(ctl[slot]));
                    ctl[slot].fd = next;
                    ctl[slot].phase = CTL_SNIFF;
                    ctl[slot].accepted_ms = now_ms();
                    ctl[slot].gen = ++b->next_gen;
                    if (hold_console_peer_pid(next, &ctl[slot].peer_pid) != 0) ctl[slot].peer_pid = 0;
                }
            }
        }

        for (int i = 0; pr > 0 && i < BROKER_MAX_CONTROL; i++) {
            struct control_client *c = &ctl[i];
            if (c->fd < 0 || ctl_idx[i] < 0 || !(pfds[ctl_idx[i]].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
                continue;
            if (c->phase == CTL_SNIFF) sniff_connection(b, c, &cl);
            else read_control(b, c);
        }

        if (client_idx >= 0 && cl.fd >= 0 && !cl.input_closed &&
            (pfds[client_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            unsigned char buf[4096];
            ssize_t n = read(cl.fd, buf, sizeof(buf));
            if (n > 0) {
                if (process_attach_input(b, &cl, buf, (size_t)n) != 0) {
                    drop_attach(&cl);
                }
            } else if (n == 0) {
                if (!cl.decided && cl.pending_len > 0) {
                    (void)hold_write_all(b->master, cl.pending, cl.pending_len);
                    cl.pending_len = 0;
                }
                cl.decided = true;
                cl.input_closed = true;
            } else if (errno != EAGAIN && errno != EINTR) {
                drop_attach(&cl);
            }
        }
        if (cl.fd >= 0 && cl.needs_repaint && cl.decided) {
            cl.needs_repaint = false;
            if (repaint_attach(b, &cl) != 0) drop_attach(&cl);
        }

        if (pr > 0 && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            char buf[4096];
            ssize_t n = hold_term_pump_master(b->master, b->logfd, b->logidxfd, &b->tui, buf, sizeof(buf));
            if (n > 0) {
                b->output_seq += (uint64_t)n;
                b->last_output_ms = now_ms();
                hold_screen_feed(b->screen, buf, (size_t)n);
                if (cl.fd >= 0) hold_screen_discard_output(b->screen);
                else flush_screen_output(b);
                /* Before the repaint, the client gets nothing: the repaint
                 * already includes these bytes. */
                if (cl.fd >= 0 && !cl.needs_repaint && write_all_wait(cl.fd, buf, (size_t)n) != 0) {
                    drop_attach(&cl);
                }
            } else if (n == 0) {
                break;
            }
        }

        now = now_ms();
        pump_msgs(b, now);
        for (int i = 0; i < BROKER_MAX_CONTROL; i++) {
            if (ctl[i].fd >= 0 && ctl[i].phase == CTL_WAITING) check_wait(b, &ctl[i], now);
            else if (ctl[i].fd >= 0) sniff_timeout(&ctl[i], &cl, now);
        }
        if (cl.fd >= 0 && cl.needs_repaint && cl.decided) {
            cl.needs_repaint = false;
            if (repaint_attach(b, &cl) != 0) drop_attach(&cl);
        }
        if (pr == 0 && b->target_done) {
            break;
        }
    }

    reap_target(b, true);
    if (b->target_done && !target_marked && b->store && b->run_id && *b->run_id) {
        /* The exit stamp must not be lost to the launch race: retry until the
         * parent's record exists. rc 1 means the record was purged while the
         * call was exiting — that removal is final, do not resurrect it. */
        for (int i = 0; i < 50; i++) {
            int mark_rc = hold_mark_run_finished(b->store, b->run_id, b->target_status);
            if (mark_rc == 0 || mark_rc == 1) break;
            struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
            while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
                continue;
            }
        }
    }
    write_final_screen(b);
    /* Pending waiters learn the outcome: their condition may still hold on
     * the final screen, else they see the exit. */
    int64_t now = now_ms();
    for (int i = 0; i < BROKER_MAX_CONTROL; i++) {
        if (ctl[i].fd < 0) continue;
        if (ctl[i].phase == CTL_WAITING) check_wait(b, &ctl[i], now);
        else control_replyf(&ctl[i], "error the call has ended\n");
        if (ctl[i].fd >= 0) drop_control(&ctl[i]);
    }
    for (int i = 0; i < BROKER_MAX_MSGS; i++) free(b->msgs[i].text);
    if (cl.fd >= 0) close(cl.fd);
    broker_cleanup_and_exit(b, 0);
}

/* ---- entry point --------------------------------------------------------- */

void hold_run_console_broker(int parent_pipe,
                               int target_pid_fd,
                               const struct hold_store *store,
                               const char *run_id,
                               const char *log_path,
                               const char *sock_path,
                               uid_t owner_uid,
                               bool have_allowed_peer_uid,
                               uid_t allowed_peer_uid,
                               int argc,
                               char **argv,
                               const char *exec_path,
                               unsigned short init_rows,
                               unsigned short init_cols) {
    struct broker b = {
        .store = store,
        .run_id = run_id,
        .log_path = log_path,
        .sock_path = sock_path,
        .parent_pipe = parent_pipe,
        .listener = -1,
        .master = -1,
        .logfd = -1,
        .logidxfd = -1,
        .target = -1,
        .owner_uid = owner_uid,
        .have_allowed_peer_uid = have_allowed_peer_uid,
        .allowed_peer_uid = allowed_peer_uid,
    };
    if (argc <= 0 || !argv || !argv[0]) {
        broker_fail_errno(&b, EINVAL);
    }
    b.screen = hold_screen_new(init_rows ? init_rows : 24, init_cols ? init_cols : 80);
    if (!b.screen) {
        broker_fail_errno(&b, ENOMEM);
    }
    b.listener = hold_make_console_listener(sock_path);
    if (b.listener < 0) {
        broker_fail_errno(&b, errno);
    }
    b.logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    b.logidxfd = b.logfd >= 0 ? hold_open_log_index_fd(log_path, b.logfd) : -1;
    if (b.logfd < 0) {
        broker_fail_errno(&b, errno);
    }

    /* The one spawn engine puts the target on a fresh PTY (setsid + TIOCSCTTY
     * + dup2 x3 + exec behind the errno handshake). On failure nothing is
     * left open or unreaped, and the errno rides our own handshake pipe up to
     * the launching parent. The broker's listener/log fds are all CLOEXEC, so
     * the target never inherits them past exec. */
    struct hold_term_spawn spawn = {
        .argv = argv,
        .exec_path = exec_path,
        .rows = init_rows,
        .cols = init_cols,
    };
    if (hold_term_pty_spawn(&spawn, &b.master, &b.target) != 0) {
        broker_fail_errno(&b, errno);
    }
    /* Report the real held group to the parent before releasing the handshake:
     * a failed write must still ride the handshake pipe as an errno, and the
     * write-before-close ordering keeps the parent's pid read deadlock-free. */
    if (target_pid_fd >= 0) {
        if (hold_write_all(target_pid_fd, &b.target, sizeof(b.target)) != 0) {
            broker_fail_errno(&b, errno);
        }
        close(target_pid_fd);
    }
    close(parent_pipe);
    b.parent_pipe = -1;

    /* Forward a TERM aimed at the broker to the held group (no SA_RESTART so
     * the serve loop's poll/waitpid see EINTR). The handler is async-signal-
     * safe and also unblocks a TERM arriving during the final blocking waitpid. */
    g_broker_forward_target = b.target;
    struct sigaction term_sa;
    memset(&term_sa, 0, sizeof(term_sa));
    term_sa.sa_handler = broker_forward_term;
    sigemptyset(&term_sa.sa_mask);
    (void)sigaction(SIGTERM, &term_sa, NULL);

    broker_serve(&b);
}
