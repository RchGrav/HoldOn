#pragma once
#ifndef SIGMUND_RUNTIME_H
#define SIGMUND_RUNTIME_H

#include "sigmund/config.h"
#include "sigmund/types.h"

bool command_accepts_target_tokens(const char *command);
bool start_target_is_within_invoking_home(const struct invocation *inv,
                                                 bool owned,
                                                 const char *command,
                                                 int argc,
                                                 char **argv);
int perform_start(const struct invocation *inv,
                         const struct store_paths *store,
                         bool tail,
                         bool console_mode,
                         int argc,
                         char **argv,
                         const char *exec_path,
                         const char *run_alias);
int cmd_list_normal(const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *alias_filter,
                           bool iso);
int cmd_list_system(const struct store_paths *system_store,
                           const char *alias_filter,
                           bool iso);
enum id_token_scope parse_id_token(const char *token, const char **id_out);
int resolve_public_profile_token(const struct store_paths *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN]);
int cmd_signal_action(const struct invocation *inv,
                             const struct store_paths *user_store,
                             const struct store_paths *system_store,
                             const char *program,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful,
                             bool all,
                             bool print_cmd);
int cmd_tail_action(const struct invocation *inv,
                           const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *program,
                           const char *id_token);
int cmd_dump_action(const struct invocation *inv,
                           const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *program,
                           const char *id_token);
int cmd_console_action(const struct invocation *inv,
                              const struct store_paths *user_store,
                              const struct store_paths *system_store,
                              const char *program,
                              const char *id_token);
int cmd_prune_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            const char *target_token,
                            bool all);
int elevate_start_token(const char *program,
                               bool tail,
                               bool console_mode,
                               const char *token_atom,
                               const char *hash,
                               bool multi,
                               int multi_count);
int cmd_start_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            const struct store_paths *fallback_store,
                            bool tail,
                            bool console_mode,
                            bool multi,
                            int multi_count,
                            int argc,
                            char **argv);
int ensure_start_store_for_command(const struct invocation *inv,
                                          bool requested_system,
                                          bool owned,
                                          const char *command,
                                          int argc,
                                          char **argv,
                                          struct store_paths *store);
int maybe_elevate_requested_system_targets(const char *program,
                                                  const char *command,
                                                  int argc,
                                                  char **argv,
                                                  bool all,
                                                  int *rc_out);
int cmd_alias_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            int argc,
                            char **argv);
int cmd_aliases_action(const struct invocation *inv,
                              const struct store_paths *user_store,
                              const struct store_paths *system_store,
                              bool verbose);
void usage(void);
int cmd_elevated_capability_action(const struct invocation *inv,
                                          const struct store_paths *system_store,
                                          const char *command,
                                          bool tail,
                                          bool console_mode,
                                          int sig,
                                          bool graceful,
                                          int argc,
                                          char **argv);

#endif /* SIGMUND_RUNTIME_H */
