#pragma once
#ifndef SIGMUND_CONSOLE_INTERNAL_H
#define SIGMUND_CONSOLE_INTERNAL_H

/* Console-private protocol constants, attach-state structs, and the SIGWINCH
 * flag. Included only by the console translation units; not part of the public
 * console.h surface. */
#include "sigmund/config.h"

#define CONSOLE_ATTACH_MAGIC "SIGMUND1"
#define CONSOLE_ATTACH_MAGIC_LEN 8
#define CONSOLE_FRAME_DATA 'D'
#define CONSOLE_FRAME_RESIZE 'W'
#define CONSOLE_FRAME_DETACH 'X'
#define CONSOLE_FRAME_HEADER_LEN 3
#define CONSOLE_ATTACH_DETACH 0x1d
#define CONSOLE_REPLAY_LIMIT (64 * 1024)

struct console_client_state {
    bool framed;
    bool decided;
    unsigned char pending[16384];
    size_t pending_len;
};

struct console_replay_buffer {
    unsigned char *data;
    size_t cap;
    size_t len;
    size_t start;
};

extern volatile sig_atomic_t g_console_resized;

/* Cross-file console internals (frame/replay/broker/attach share these). */
void handle_console_sigwinch(int signo);
void console_replay_init(struct console_replay_buffer *replay);
void console_replay_free(struct console_replay_buffer *replay);
void console_replay_append(struct console_replay_buffer *replay, const void *buf, size_t n);
int console_replay_write(const struct console_replay_buffer *replay, int fd);
int write_console_frame(int fd, unsigned char type, const void *payload, uint16_t len);
int send_console_resize(int fd, const struct winsize *ws);
int maybe_get_terminal_size(struct winsize *ws);
int broker_process_client_input(struct console_client_state *state, int master,
                                const unsigned char *buf, size_t n);
int make_console_listener(const char *sock_path);
int open_console_pty(int *master_out, int *slave_out);
int connect_console_socket(const char *sock_path);
void make_raw_termios(const struct termios *in, struct termios *out);

#endif /* SIGMUND_CONSOLE_INTERNAL_H */
