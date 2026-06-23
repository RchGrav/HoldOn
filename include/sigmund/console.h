#pragma once
#ifndef SIGMUND_CONSOLE_H
#define SIGMUND_CONSOLE_H

#include "sigmund/config.h"
#include "sigmund/types.h"

/* Public console entry points used by the runtime/CLI layers. The frame, replay
 * and broker internals live in console_internal.h. */
int sigmund_format_console_sock_path(const struct sigmund_store *store,
                             const char *id,
                             char *out,
                             size_t n);
void sigmund_run_console_broker(int parent_pipe,
                        const char *log_path,
                        const char *sock_path,
                        uid_t owner_uid,
                        bool have_allowed_peer_uid,
                        uid_t allowed_peer_uid,
                        int argc,
                        char **argv,
                        const char *exec_path);
int sigmund_run_native_console(const char *sock_path);

#endif /* SIGMUND_CONSOLE_H */
