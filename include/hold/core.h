#pragma once
#ifndef HOLD_CORE_H
#define HOLD_CORE_H

#include "hold/config.h"
#include "hold/types.h"

struct sha256_ctx {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t off;
};

/* util */
_Noreturn void hold_die_errno(const char *msg);
void hold_sig_note(const struct hold_invocation *inv, const char *fmt, ...);
int hold_checked_snprintf(char *dst, size_t n, const char *fmt, ...);
bool hold_has_suffix(const char *s, const char *suffix);
int hold_write_all(int fd, const void *buf, size_t n);
void hold_close_stdio_to_devnull(void);
int hold_format_argv_human(char *dst, size_t n, int argc, char **argv);
int hold_read_exec_handshake(int fd, int *child_errno);
void hold_format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n);
void hold_format_duration_human(int64_t seconds, char *out, size_t n);
bool hold_parse_rfc3339_utc_to_ns(const char *s, int64_t *out_ns);
const char *hold_run_id_display(const char *id, char out[ID_DISPLAY_HEX_LEN + 1]);
int hold_copy_argv(char ***out, int argc, char **argv);

/* validate */
bool hold_valid_id(const char *id);
bool hold_valid_id_prefix(const char *id);
bool hold_valid_alias(const char *alias);
bool hold_valid_record(const struct hold_run_record *r);
bool hold_record_json_filename_id(const char *name, char *id, size_t n);
int hold_parse_uid_env(const char *s, uid_t *out);
int hold_parse_gid_env(const char *s, gid_t *out);

/* sha256 */
void hold_sha256_init(struct sha256_ctx *c);
void hold_sha256_update(struct sha256_ctx *c, const void *data, size_t n);
void hold_sha256_final(struct sha256_ctx *c, unsigned char out[32]);
void hold_sha256_update_nul_field(struct sha256_ctx *ctx, const char *s);
void hold_hex_encode(const unsigned char *bytes, size_t n, char *out, size_t out_n);

/* fs */
int hold_mkdir_p_mode(const char *dir, mode_t mode);
int hold_chmod_dir_no_symlink(const char *dir, mode_t mode);
int hold_chown_dir_no_symlink_if_root(const char *dir, uid_t uid, gid_t gid);
int hold_read_owned_file_no_symlink(const char *path, char **out);
void hold_supervisor_lock_log(const char *log_path);
int hold_open_unique_temp(const char *dir, const char *prefix, mode_t mode, char *tmp, size_t tmp_n);
int hold_open_append_no_symlink(const char *path);
bool hold_path_exists(const char *path);

/* json */
void hold_json_escape(FILE *f, const char *s);
int hold_write_json_argv(FILE *f, int argc, char **argv);
int hold_parse_json_string(const char *p, char *out, size_t n, const char **endp);
int hold_json_find_key(const char *j, const char *k, const char **v);
int hold_json_get_i64(const char *j, const char *k, int64_t *out);
int hold_json_get_u64(const char *j, const char *k, uint64_t *out);
int hold_json_get_bool(const char *j, const char *k, bool *out);
int hold_json_get_str(const char *j, const char *k, char *out, size_t n);
int hold_json_get_argv_display(const char *j, char *out, size_t n);
int hold_json_get_argv_alloc(const char *j, char ***argv_out, int *argc_out);
int hold_json_get_env_alloc(const char *j, char ***env_out, int *envc_out);
void hold_free_argv_alloc(char **argv, int argc);

#endif /* HOLD_CORE_H */
