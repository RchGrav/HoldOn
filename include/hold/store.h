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
