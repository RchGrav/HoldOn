#pragma once
#ifndef SIGMUND_ACCESS_H
#define SIGMUND_ACCESS_H

#include "sigmund/config.h"
#include "sigmund/types.h"

int sigmund_detect_invocation(struct sigmund_invocation *inv, bool requested_system, bool elevated);
int sigmund_init_invoking_user_store(const struct sigmund_invocation *inv, struct sigmund_store *store);
int sigmund_elevate_with_sudo_canonical(const char *program, int canonical_argc, char **canonical_argv);
int sigmund_elevate_with_sudo_parsed(const char *program,
                                    bool owned,
                                    const char *command,
                                    bool tail,
                                    bool console_mode,
                                    bool all,
                                    bool print_cmd,
                                    bool multi,
                                    int multi_count,
                                    bool force_raw,
                                    int argc,
                                    char **argv);
int sigmund_cmd_grant_revoke_action(const struct sigmund_invocation *inv,
                                   const struct sigmund_store *system_store,
                                   const char *program,
                                   bool grant,
                                   int argc,
                                   char **argv);

#endif /* SIGMUND_ACCESS_H */
