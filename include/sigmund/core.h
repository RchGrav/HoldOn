#pragma once
#ifndef SIGMUND_CORE_H
#define SIGMUND_CORE_H

#include "sigmund/config.h"
#include "sigmund/types.h"

struct sha256_ctx {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t off;
};

void sigmund_die_errno(const char *msg);
void sigmund_sig_note(const struct sigmund_invocation *inv, const char *fmt, ...);
int sigmund_checked_snprintf(char *dst, size_t n, const char *fmt, ...);
bool sigmund_has_suffix(const char *s, const char *suffix);
bool sigmund_valid_id(const char *id);
bool sigmund_record_json_filename_id(const char *name, char *id, size_t n);
bool sigmund_valid_id_prefix(const char *id);
bool sigmund_valid_profile_hash(const char *hash);
bool sigmund_valid_alias(const char *alias);
bool sigmund_valid_record(const struct sigmund_run_record *r);
int sigmund_mkdir_p0700(const char *dir);
int sigmund_read_file_trim(const char *path, char *buf, size_t n);
bool sigmund_path_exists(const char *path);
void sigmund_sha256_init(struct sha256_ctx *c);
void sigmund_sha256_final(struct sha256_ctx *c, unsigned char out[32]);
void sigmund_hex_encode(const unsigned char *bytes, size_t n, char *out, size_t out_n);
int sigmund_rand_bytes(uint8_t *buf, size_t n);
int sigmund_mkdir_p_mode(const char *dir, mode_t mode);
int sigmund_parse_uid_env(const char *s, uid_t *out);
int sigmund_parse_gid_env(const char *s, gid_t *out);
int sigmund_write_all(int fd, const void *buf, size_t n);
void sigmund_json_escape(FILE *f, const char *s);
int sigmund_write_json_argv(FILE *f, int argc, char **argv);
void sigmund_sha256_update_nul_field(struct sha256_ctx *ctx, const char *s);
int sigmund_append_cmd_human(char *dst, size_t n, size_t *off, const char *arg);
int sigmund_format_argv_human(char *dst, size_t n, int argc, char **argv);
const char *sigmund_skip_ws(const char *p);
int sigmund_parse_json_string(const char *p, char *out, size_t n, const char **endp);
int sigmund_skip_json_value(const char **pp);
int sigmund_json_find_key(const char *j, const char *k, const char **v);
int sigmund_json_get_i64(const char *j, const char *k, int64_t *out);
int sigmund_json_get_bool(const char *j, const char *k, bool *out);
int sigmund_json_get_u64(const char *j, const char *k, uint64_t *out);
int sigmund_json_get_str(const char *j, const char *k, char *out, size_t n);
int sigmund_json_get_argv_display(const char *j, char *out, size_t n);
void sigmund_free_argv_alloc(char **argv, int argc);
int sigmund_json_get_argv_alloc(const char *j, char ***argv_out, int *argc_out);
int sigmund_json_get_args_alloc(const char *j, char ***argv_out, int *argc_out);
int sigmund_read_owned_file_no_symlink(const char *path, char **out);
int sigmund_fsync_dir_path(const char *dir);
int sigmund_copy_argv(char ***out, int argc, char **argv);
int sigmund_read_small_file(const char *path, char **out);
int sigmund_read_exec_handshake(int fd, int *child_errno);
void sigmund_format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n);
void sigmund_format_relative_age(int64_t start_unix_ns, char *out, size_t n);
bool sigmund_valid_runid_selector(const char *sel);
bool sigmund_valid_target_atom(const char *id);

#endif /* SIGMUND_CORE_H */
