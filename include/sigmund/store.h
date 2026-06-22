#pragma once
#ifndef SIGMUND_STORE_H
#define SIGMUND_STORE_H

#include "sigmund/config.h"
#include "sigmund/types.h"

int chown_root_if_root(const char *path);
int init_user_store_from_home(const char *home, struct sigmund_store *store);
int ensure_user_store_for_current_user(struct sigmund_store *store);
int ensure_invoking_user_store(const struct sigmund_invocation *inv, struct sigmund_store *store);
int init_system_store(struct sigmund_store *store);
int ensure_system_store(struct sigmund_store *store);
int gen_id_for_store(const struct sigmund_store *primary,
                            const struct sigmund_store *avoid_public_store,
                            const struct sigmund_store *avoid_user_store,
                            char *out,
                            size_t out_n);
void profile_hash_for_argv(const char *binary_path, int argc, char **argv, char out[PROFILE_HASH_STR_LEN]);
int write_record_atomic(const char *dir, const struct sigmund_run_record *r, int argc, char **argv, char *out_json_path, size_t out_n);
int write_public_index_atomic(const struct sigmund_store *store, const struct sigmund_run_record *r);
void free_profile(struct sigmund_profile *p);
int write_profile_atomic(const struct sigmund_store *store,
                                const char *hash,
                                const char *binary_path,
                                int argc,
                                char **argv);
int load_profile_by_hash(const struct sigmund_store *store, const char *hash, struct sigmund_profile *profile);
void free_aliases(struct sigmund_alias *entries, size_t count);
int load_aliases(const struct sigmund_store *store, struct sigmund_alias **entries_out, size_t *count_out);
int alias_lookup_hash(const struct sigmund_store *store, const char *alias, char hash[PROFILE_HASH_STR_LEN]);
int alias_upsert_hash(const struct sigmund_store *store, const char *alias, const char *hash);
int alias_lookup_recipe(const struct sigmund_store *store, const char *alias, struct sigmund_profile *recipe);
int alias_upsert_recipe(const struct sigmund_store *store,
                               const char *alias,
                               const char *binary_path,
                               int argc,
                               char **argv);
int load_record(const char *path, struct sigmund_run_record *r);
int load_public_index(const char *path, struct sigmund_public_index *pi);
int load_public_index_by_id(const struct sigmund_store *store, const char *id, struct sigmund_public_index *pi);
int load_record_by_id(const char *dir, const char *id, struct sigmund_run_record *r, char *path, size_t n);
int parse_alias_cap_atom(const char *atom,
                                char alias[ALIAS_MAX_LEN + 1],
                                char hash[PROFILE_HASH_STR_LEN]);
int verify_system_alias_cap(const struct sigmund_store *system_store,
                                   const char *alias,
                                   const char *hash);
bool alias_exists_in_store(const struct sigmund_store *store, const char *alias);
int profile_exists_in_store(const struct sigmund_store *store, const char *hash);

#endif /* SIGMUND_STORE_H */
