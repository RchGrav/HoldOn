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
const char *hold_state_str(enum run_state s);
int hold_prune_one_run(const struct hold_store *store, const char *id, const char *boot, bool allow_stale, bool *removed);
/* --rm: wait (up to timeout_ms) for the call's reaper to stamp its exit,
 * then remove it, so the verb that ended or awaited it returns with the
 * name free. No-op for calls without --rm or already gone. */
void hold_auto_remove_settle(const struct hold_store *store, const char *id, int timeout_ms);
/* The exit code of an --rm call removed in the last minute, by id prefix or
 * name: removal leaves it behind so `hold wait` can still report it. */
bool hold_auto_removed_exit(const struct hold_store *store, const char *token, int *code);
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

/* Single-target prologue shared by logs/inspect/tail and the screen verbs:
 * resolve one token (*hit false: return rc), then the requires-root gate and
 * record load (*ok false: return rc). */
/* Whether token names a call (or is malformed, which the caller reports);
 * false only for a well-formed token that matches nothing. Prints nothing. */
bool hold_token_resolves(const struct hold_invocation *inv,
                         const struct hold_store *current_user_store,
                         const struct hold_store *system_store,
                         const char *command,
                         const char *token);
int hold_resolve_single(const struct hold_invocation *inv,
                        const struct hold_store *user_store,
                        const struct hold_store *system_store,
                        const char *command,
                        const char *verb,
                        const char *token,
                        struct hold_resolved_target *out,
                        bool *hit);
int hold_load_target_record(const struct hold_resolved_target *target,
                            bool need_log,
                            struct hold_run_record *r,
                            char *path,
                            size_t path_len,
                            bool *ok);

#endif /* HOLD_RUNTIME_INTERNAL_H */
