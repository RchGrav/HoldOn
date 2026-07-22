#pragma once
#ifndef HOLD_CLI_H
#define HOLD_CLI_H

#include "hold/config.h"
#include "hold/types.h"

/* Flags the owned-command sweep can set — one bool per spec-gated flag. The
 * pre-scan seeds requested_system/quiet; the sweep may add to them. */
struct hold_cli_owned_opts {
    bool all;              /* --all, and -a wherever --all is accepted */
    bool live_only;        /* -l/--live (list) */
    bool system_scope;     /* -s (list/purge store scope) */
    bool user_scope;       /* -u/--user (list/purge store scope) */
    bool force;            /* --force (purge family) */
    bool print_cmd;        /* --print (end/stop/kill) */
    bool no_stream;        /* --no-stream (stats) */
    bool requested_system; /* --system, the global store selector */
    bool quiet;            /* --quiet */
    bool saw_delimiter;    /* a literal `--` was seen */
};

int hold_show_help(const char *topic);
bool hold_cli_command_is_parser_owned(const char *s);
const char *hold_cli_command_usage(const char *s);
const char *hold_cli_command_canon(const char *s);
int hold_validate_owned_command_arity(const char *command, int argc);
int hold_cli_collect_owned_args(const char *command, int argc, char **argv,
                                char **cmd_argv, int *cmd_argc,
                                struct hold_cli_owned_opts *opts);

#endif /* HOLD_CLI_H */
