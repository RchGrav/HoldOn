#pragma once
#ifndef SIGMUND_CLI_H
#define SIGMUND_CLI_H

#include "sigmund/config.h"
#include "sigmund/types.h"

int sigmund_show_help(const char *topic);
bool sigmund_is_sigmund_owned_command(const char *s);
bool sigmund_cli_command_allows_all(const char *s);
const char *sigmund_cli_command_usage(const char *s);
int sigmund_validate_owned_command_arity(const char *command, int argc);
bool sigmund_parse_positive_count(const char *s, int *out);

#endif /* SIGMUND_CLI_H */
