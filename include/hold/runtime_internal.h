#pragma once
#ifndef HOLD_RUNTIME_INTERNAL_H
#define HOLD_RUNTIME_INTERNAL_H

#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"

void hold_report_session_escapees(const struct hold_run_record *r);
enum run_state hold_eval_state(const struct hold_run_record *r, const char *current_boot);
int hold_tail_log_until_exit(const struct hold_run_record *r, bool from_end, bool follow_until_exit);
void hold_rollback_spawned_group(pid_t pid, pid_t pgid);
bool hold_wait_target_group_gone(const struct hold_run_record *r, int timeout_ms);
int hold_do_signal_action(const struct hold_store *store, const char *id, int sig, bool graceful, bool *already_done);
const char *hold_state_str(enum run_state s);
int hold_prune_one_run(const struct hold_store *store, const char *id, const char *boot, bool allow_stale, bool *removed);
bool hold_record_matches_alias_intent(const char *command, const struct hold_run_record *r, enum run_state st);
int hold_report_not_found(const char *token);
int hold_report_requires_root(const char *token);
int hold_resolve_action_token(const struct hold_invocation *inv,
                                const struct hold_store *current_user_store,
                                const struct hold_store *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct hold_resolved_target **targets_out,
                                int *count_out);

#endif /* HOLD_RUNTIME_INTERNAL_H */
