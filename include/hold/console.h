#pragma once
#ifndef HOLD_CONSOLE_H
#define HOLD_CONSOLE_H

#include "hold/config.h"
#include "hold/types.h"

/* Public console entry points used by the runtime/CLI layers. The frame, replay
 * and broker internals live in console_internal.h.
 *
 * The broker serves one interactive attach client at a time (others receive
 * a short "already attached" error) plus short-lived control clients. */
int hold_format_console_sock_path(const struct hold_store *store,
                             const char *id,
                             char *out,
                             size_t n);
int hold_console_set_detach_keys(const unsigned char *keys, size_t len);
/* target_pid_fd: -1 = do not report; otherwise the broker writes its forked
 * target pid exactly once after exec-handshake success, then closes the fd. The
 * write precedes closing parent_pipe so a pid-write failure still reaches the
 * parent's handshake read as an errno; the ordering also guarantees that
 * handshake EOF at the parent implies the pid was already written (or the broker
 * died), so the parent's unbounded pid read cannot deadlock. */
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
                        unsigned short init_cols);
/* One control request to a call's broker (hold peek/send/wait/resize): the
 * reply (NUL-terminated, malloc'd) begins with a text status line; peek's
 * carries an HSNAP1 snapshot after it. -1 with errno on connect/IO failure. */
int hold_console_request(const char *sock_path, const char *request, size_t request_len,
                         unsigned char **reply, size_t *reply_len);

/* The attach client: connect to a call's console socket and bridge the
 * terminal until the detach chord (Ctrl-P Ctrl-Q by default) or the call ends. */
int hold_run_native_console(const char *sock_path);

#endif /* HOLD_CONSOLE_H */
