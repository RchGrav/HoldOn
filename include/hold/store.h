#pragma once
#ifndef HOLD_STORE_H
#define HOLD_STORE_H

#include "hold/config.h"
#include "hold/types.h"

int hold_init_user_store_from_home(const char *home, struct hold_store *store);
int hold_ensure_user_store_for_current_user(struct hold_store *store);
int hold_ensure_invoking_user_store(const struct hold_invocation *inv, struct hold_store *store);
int hold_init_system_store(struct hold_store *store);
int hold_ensure_system_store(struct hold_store *store);
int hold_write_record_atomic(const char *dir, const struct hold_run_record *r, int argc, char **argv, char *out_json_path, size_t out_n);
int hold_write_public_index_atomic(const struct hold_store *store, const struct hold_run_record *r,
                                     const char *observed_ports_csv);
int hold_mark_run_finished(const struct hold_store *store, const char *id, int status);

/* Two-phase creation, step 1: derive a collision-free hashed run id
 * (hold-run-v1 material: exe, cwd, timestamp_ns, launcher pid, argc, argv[],
 * counter; <=1024 probes) and hold its .<id>.reserve marker (O_EXCL 0600).
 * A candidate colliding with ANY existing store material (record, .log,
 * .reserve, .sock, public projection) is skipped. Commit is
 * hold_write_record_atomic, which unlinks the reserve after the record
 * rename; abort with hold_abort_run_reservation. */
int hold_reserve_run_id(const struct hold_store *store,
                        const char *resolved_exec_path,
                        int argc, char **argv,
                        const char *cwd,
                        int64_t start_unix_ns,
                        char out_id[ID_STR_LEN]);
/* The adoption variant of the same two-phase reserve: scope tag
 * hold-run-adopt-v1 (so launched and adopted ids never collide), material
 * from the observed process (exe, cwd, created_ns, pid, pgid, argv), and an
 * extra collision check against avoid_public_store's public projections
 * (adopting into a user store must not shadow a system-managed id). */
int hold_reserve_adopted_run_id(const struct hold_store *store,
                                const struct hold_store *avoid_public_store,
                                const char *observed_exe,
                                int argc, char **argv,
                                const char *cwd,
                                pid_t pid, pid_t pgid,
                                int64_t start_unix_ns,
                                char out_id[ID_STR_LEN]);
void hold_abort_run_reservation(const struct hold_store *store, const char *id);
/* The running-record base builder: stamps every field the launch and adoption
 * paths set identically (ids, group identity, timestamps, state, owner, log).
 * Callers add their own provenance (recipe/observed/boot/name/console). */
void hold_record_init_running(struct hold_run_record *r,
                              const char *id,
                              const char *log_path,
                              pid_t pid, pid_t pgid, pid_t sid,
                              int64_t start_unix_ns,
                              int64_t created_unix_ns);
int hold_load_record(const char *path, struct hold_run_record *r);
void hold_free_run_record(struct hold_run_record *r);
int hold_load_public_index(const char *path, struct hold_public_index *pi);
int hold_load_public_index_by_id(const struct hold_store *store, const char *id, struct hold_public_index *pi);
int hold_load_record_by_id(const char *dir, const char *id, struct hold_run_record *r, char *path, size_t n);

/* THE id resolver: exact 64-hex id if its file exists, else a prefix matching
 * exactly one <id>.json in dir. Works on record and public-index directories. */
int hold_resolve_record_id(const char *dir, const char *token, char *resolved, size_t n);

/* Record iteration: fn runs per <id>.json with the loaded record, or r == NULL
 * when the file is corrupt (unloadable, invalid, or embedded id != filename).
 * The record is freed after fn returns; a nonzero return stops the walk and
 * becomes hold_for_each_record's return value. An unopenable dir walks empty. */
typedef int (*hold_record_fn)(const char *id, const char *path, struct hold_run_record *r, void *ctx);
int hold_for_each_record(const char *record_dir, hold_record_fn fn, void *ctx);

#endif /* HOLD_STORE_H */
