#pragma once
#ifndef SIGMUND_CONSOLE_H
#define SIGMUND_CONSOLE_H

#include "sigmund/config.h"
#include "sigmund/types.h"

/* Public console entry points used by the runtime/CLI layers. The frame, replay
 * and broker internals live in console_internal.h. */
int format_console_sock_path(const struct store_paths *store,
                             const char *id,
                             char *out,
                             size_t n);
void run_console_broker(int parent_pipe,
                        const char *log_path,
                        const char *sock_path,
                        int argc,
                        char **argv,
                        const char *exec_path);
int run_native_console(const char *sock_path);

#endif /* SIGMUND_CONSOLE_H */
