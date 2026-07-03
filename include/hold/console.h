#pragma once
#ifndef HOLD_CONSOLE_H
#define HOLD_CONSOLE_H

#include "hold/config.h"
#include "hold/types.h"

/* Public console entry points used by the runtime/CLI layers. The frame, replay
 * and broker internals live in console_internal.h.
 *
 * The broker supports one interactive client at a time. Additional authorized
 * clients receive a short "already attached" error and are closed. */
int hold_format_console_sock_path(const struct hold_store *store,
                             const char *id,
                             char *out,
                             size_t n);
int hold_console_set_detach_keys(const unsigned char *keys, size_t len);
void hold_run_console_broker(int parent_pipe,
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
int hold_run_native_console(const char *sock_path);

/* Serve an already-running PTY (hold shell adoption): the target is not the
 * broker's child, must never be killed by broker cleanup, and its exit is
 * detected via PTY EOF or group/session liveness. All fds are opened by the
 * caller before forking so this path has no failure handshake. */
void hold_run_console_broker_adopted(const struct hold_store *store,
                                       const char *run_id,
                                       const char *sock_path,
                                       int listener,
                                       int master,
                                       int logfd,
                                       int logidxfd,
                                       uid_t owner_uid,
                                       pid_t adopted_pgid,
                                       pid_t adopted_sid,
                                       pid_t hup_pid);

#endif /* HOLD_CONSOLE_H */
