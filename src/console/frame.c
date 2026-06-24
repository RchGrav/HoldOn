#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/console_internal.h"

static uint16_t load_be16(const unsigned char *p);
static void store_be16(unsigned char *p, uint16_t v);
static int apply_pty_size(int master, const unsigned char *payload, size_t len);
static int broker_process_framed_client(struct console_client_state *state, int master);

void hold_console_replay_init(struct console_replay_buffer *replay) {
    memset(replay, 0, sizeof(*replay));
    replay->data = malloc(CONSOLE_REPLAY_LIMIT);
    if (replay->data) {
        replay->cap = CONSOLE_REPLAY_LIMIT;
    }
}

void hold_console_replay_free(struct console_replay_buffer *replay) {
    free(replay->data);
    memset(replay, 0, sizeof(*replay));
}

void hold_console_replay_append(struct console_replay_buffer *replay,
                                  const void *buf,
                                  size_t n) {
    if (!replay->data || replay->cap == 0 || n == 0) {
        return;
    }
    const unsigned char *p = buf;
    for (size_t i = 0; i < n; i++) {
        if (replay->len < replay->cap) {
            replay->data[(replay->start + replay->len) % replay->cap] = p[i];
            replay->len++;
        } else {
            replay->data[replay->start] = p[i];
            replay->start = (replay->start + 1) % replay->cap;
        }
    }
}

int hold_console_replay_write(const struct console_replay_buffer *replay, int fd) {
    if (!replay->data || replay->len == 0) {
        return 0;
    }
    size_t first = replay->cap - replay->start;
    if (first > replay->len) {
        first = replay->len;
    }
    if (hold_write_all(fd, replay->data + replay->start, first) != 0) {
        return -1;
    }
    if (replay->len > first &&
        hold_write_all(fd, replay->data, replay->len - first) != 0) {
        return -1;
    }
    return 0;
}

static uint16_t load_be16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void store_be16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

int hold_write_console_frame(int fd, unsigned char type, const void *payload, uint16_t len) {
    unsigned char header[CONSOLE_FRAME_HEADER_LEN];
    header[0] = type;
    store_be16(header + 1, len);
    if (hold_write_all(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (len > 0 && hold_write_all(fd, payload, len) != 0) {
        return -1;
    }
    return 0;
}

int hold_send_console_resize(int fd, const struct winsize *ws) {
    if (!ws || ws->ws_row == 0 || ws->ws_col == 0) {
        return 0;
    }
    unsigned char payload[4];
    store_be16(payload, ws->ws_row);
    store_be16(payload + 2, ws->ws_col);
    return hold_write_console_frame(fd, CONSOLE_FRAME_RESIZE, payload, sizeof(payload));
}

int hold_maybe_get_terminal_size(struct winsize *ws) {
    memset(ws, 0, sizeof(*ws));
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) {
        return 0;
    }
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) {
        return 0;
    }
    return -1;
}

static int apply_pty_size(int master, const unsigned char *payload, size_t len) {
    if (len != 4) {
        errno = EPROTO;
        return -1;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = load_be16(payload);
    ws.ws_col = load_be16(payload + 2);
    if (ws.ws_row == 0 || ws.ws_col == 0) {
        return 0;
    }
    return ioctl(master, TIOCSWINSZ, &ws);
}

static int broker_process_framed_client(struct console_client_state *state, int master) {
    while (state->pending_len >= CONSOLE_FRAME_HEADER_LEN) {
        unsigned char type = state->pending[0];
        uint16_t len = load_be16(state->pending + 1);
        size_t frame_len = CONSOLE_FRAME_HEADER_LEN + (size_t)len;
        if (state->pending_len < frame_len) {
            return 0;
        }

        const unsigned char *payload = state->pending + CONSOLE_FRAME_HEADER_LEN;
        if (type == CONSOLE_FRAME_DATA) {
            if (len > 0 && hold_write_all(master, payload, len) != 0) {
                return -1;
            }
        } else if (type == CONSOLE_FRAME_RESIZE) {
            (void)apply_pty_size(master, payload, len);
        } else if (type == CONSOLE_FRAME_DETACH) {
            return 1;
        }

        memmove(state->pending, state->pending + frame_len, state->pending_len - frame_len);
        state->pending_len -= frame_len;
    }
    return 0;
}

int hold_broker_process_client_input(struct console_client_state *state,
                                       int master,
                                       const unsigned char *buf,
                                       size_t n) {
    if (n == 0) {
        return 0;
    }
    if (!state->decided && state->pending_len + n > sizeof(state->pending)) {
        state->decided = true;
        state->framed = false;
        if (state->pending_len > 0 && hold_write_all(master, state->pending, state->pending_len) != 0) {
            return -1;
        }
        state->pending_len = 0;
    }
    if (state->decided && !state->framed) {
        return hold_write_all(master, buf, n);
    }

    if (state->pending_len + n > sizeof(state->pending)) {
        errno = EOVERFLOW;
        return -1;
    }
    memcpy(state->pending + state->pending_len, buf, n);
    state->pending_len += n;

    if (!state->decided) {
        size_t cmp_len = state->pending_len < CONSOLE_ATTACH_MAGIC_LEN ? state->pending_len : CONSOLE_ATTACH_MAGIC_LEN;
        if (memcmp(state->pending, CONSOLE_ATTACH_MAGIC, cmp_len) != 0) {
            state->decided = true;
            state->framed = false;
            if (hold_write_all(master, state->pending, state->pending_len) != 0) {
                return -1;
            }
            state->pending_len = 0;
            return 0;
        }
        if (state->pending_len < CONSOLE_ATTACH_MAGIC_LEN) {
            return 0;
        }
        state->decided = true;
        state->framed = true;
        memmove(state->pending,
                state->pending + CONSOLE_ATTACH_MAGIC_LEN,
                state->pending_len - CONSOLE_ATTACH_MAGIC_LEN);
        state->pending_len -= CONSOLE_ATTACH_MAGIC_LEN;
    }

    if (state->framed) {
        return broker_process_framed_client(state, master);
    }
    return 0;
}

void hold_make_raw_termios(const struct termios *in, struct termios *out) {
    *out = *in;
    out->c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    out->c_oflag &= (tcflag_t)~OPOST;
    out->c_cflag |= CS8;
    out->c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    out->c_cc[VMIN] = 1;
    out->c_cc[VTIME] = 0;
}
