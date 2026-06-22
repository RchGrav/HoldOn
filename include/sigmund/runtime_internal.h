#pragma once
#ifndef SIGMUND_RUNTIME_INTERNAL_H
#define SIGMUND_RUNTIME_INTERNAL_H

#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/runtime.h"

struct alias_match {
    char id[16];
    enum run_state state;
    char started_at[64];
};

struct alias_match_list {
    struct alias_match *items;
    size_t count;
    bool alias_known;
};

void sigmund_report_session_escapees(const struct sigmund_run_record *r);
enum run_state sigmund_eval_state(const struct sigmund_run_record *r, const char *current_boot);
int sigmund_tail_log_until_exit(const struct sigmund_run_record *r, bool from_end, bool follow_until_exit);
void sigmund_rollback_spawned_group(pid_t pid, pid_t pgid);
bool sigmund_wait_target_group_gone(const struct sigmund_run_record *r, int timeout_ms);
int sigmund_do_signal_action(const struct sigmund_store *store, const char *id, int sig, bool graceful, bool *already_done);
const char *sigmund_state_str(enum run_state s);
int sigmund_prune_one_run(const struct sigmund_store *store, const char *id, const char *boot, bool allow_stale, bool *removed);
int sigmund_ensure_run_recorded_under_alias(const struct sigmund_store *store, const char *id, const char *alias);
void sigmund_free_alias_match_list(struct alias_match_list *list);
bool sigmund_command_all_allowed(const char *command);
bool sigmund_record_matches_alias_intent(const char *command, const struct sigmund_run_record *r, enum run_state st);
int sigmund_collect_private_alias_matches(const struct sigmund_store *store,
                                         const char *alias,
                                         const char *command,
                                         struct alias_match_list *list);
int sigmund_resolve_target(const struct sigmund_invocation *inv,
                          const struct sigmund_store *current_user_store,
                          const struct sigmund_store *system_store,
                          const char *token,
                          struct sigmund_resolved_target *out);
int sigmund_report_not_found(const char *token);
int sigmund_resolve_action_token(const struct sigmund_invocation *inv,
                                const struct sigmund_store *current_user_store,
                                const struct sigmund_store *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct sigmund_resolved_target **targets_out,
                                int *count_out);
int sigmund_elevate_with_sudo_targets(const char *program,
                               const char *command,
                               char **original_tokens,
                               const struct sigmund_resolved_target *targets,
                               int ntargets,
                               bool all,
                               bool print_cmd);
int sigmund_perform_profile_start(const struct sigmund_invocation *inv,
                                 const struct sigmund_store *store,
                                 bool tail,
                                 bool console_mode,
                                 const char *hash,
                                 const char *alias);

#endif /* SIGMUND_RUNTIME_INTERNAL_H */
