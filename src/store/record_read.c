#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "store_internal.h"

/* The strict record reader: one table-driven pass over the schema in
 * record.c (required fields hard-fail, every id-like integer narrows with
 * ERANGE), then the few reader-owned defaults, shims, and nested objects. */

int hold_record_checked_i64_to_pid(int64_t v, pid_t *out) {
    pid_t narrowed = (pid_t)v;
    if ((int64_t)narrowed != v) {
        errno = ERANGE;
        return -1;
    }
    *out = narrowed;
    return 0;
}

int hold_record_read_fields(const char *j, struct hold_run_record *r) {
    for (size_t i = 0; i < hold_record_field_count; i++) {
        const struct hold_record_field *fs = &hold_record_fields[i];
        if (fs->flags & RF_WRONLY) continue;
        char *p = (char *)r + fs->off;
        bool ok = false;
        int64_t v = 0;
        switch (fs->type) {
        case RF_STR:
            ok = hold_json_get_str(j, fs->key, p, fs->size) == 0 &&
                 !((fs->flags & RF_ALIAS) && !hold_valid_alias(p)) &&
                 !((fs->flags & RF_ABS) && p[0] != '/') &&
                 !((fs->flags & RF_NONEMPTY) && !p[0]);
            break;
        case RF_BOOL:
            ok = hold_json_get_bool(j, fs->key, (bool *)p) == 0;
            break;
        case RF_U64:
            ok = hold_json_get_u64(j, fs->key, (uint64_t *)p) == 0;
            break;
        default:
            if (hold_json_get_i64(j, fs->key, &v) != 0) break;
            if (fs->type == RF_I64) {
                *(int64_t *)p = v;
            } else if (fs->type == RF_INT) {
                *(int *)p = (int)v;
            } else if (fs->type == RF_PID) {
                if (hold_record_checked_i64_to_pid(v, (pid_t *)p) != 0) return -1;
            } else if (fs->type == RF_UID) {
                if (v < 0 || (uintmax_t)(uid_t)v != (uintmax_t)v) { errno = ERANGE; return -1; }
                *(uid_t *)p = (uid_t)v;
            } else { /* RF_GID */
                if (v < 0 || (uintmax_t)(gid_t)v != (uintmax_t)v) { errno = ERANGE; return -1; }
                *(gid_t *)p = (gid_t)v;
            }
            ok = true;
            break;
        }
        if (!ok) {
            if (fs->flags & RF_REQ) return -1;
            continue;
        }
        if (fs->has_off != RF_ALWAYS) *(bool *)((char *)r + fs->has_off) = true;
    }
    return 0;
}

int hold_load_record(const char *path, struct hold_run_record *r) {
    memset(r, 0, sizeof(*r));
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(path, &j) != 0) return -1;
    if (hold_record_read_fields(j, r) != 0) {
        free(j);
        return -1;
    }
    /* Reader-side defaults and shims for the table's RF_WRONLY fields. */
    if (!r->run_id[0]) snprintf(r->run_id, sizeof(r->run_id), "%s", r->id);
    if (r->created_unix_ns == 0) r->created_unix_ns = r->start_unix_ns;
    if (!r->has_created_at && r->has_started_at) {
        snprintf(r->created_at, sizeof(r->created_at), "%s", r->started_at);
        r->has_created_at = true;
    }
    if (hold_json_get_bool(j, "saved", &r->saved) != 0) {
        /* Legacy shim (D-4): pre-0.7 records spell the flag "Saved". */
        (void)hold_json_get_bool(j, "Saved", &r->saved);
    }
    const char *obj = NULL;
    if (hold_json_find_key(j, "mode", &obj) == 0 && obj && *obj == '{') {
        bool b = false;
        if (hold_json_get_bool(obj, "interactive", &b) == 0) r->recipe.mode_interactive = b;
        if (hold_json_get_bool(obj, "tty", &b) == 0) r->recipe.mode_tty = b;
        if (hold_json_get_bool(obj, "detach", &b) == 0) r->recipe.mode_detach = b;
        if (hold_json_get_bool(obj, "allow_multi", &b) == 0) r->recipe.allow_multi = b;
    }
    if (hold_json_find_key(j, "observed", &obj) == 0 && obj && *obj == '{') {
        r->has_observed |= hold_json_get_str(obj, "exe", r->observed_exe, sizeof(r->observed_exe)) == 0;
        r->has_observed |= hold_json_get_str(obj, "cwd", r->observed_cwd, sizeof(r->observed_cwd)) == 0;
        r->has_observed |= hold_json_get_argv_alloc(obj, &r->observed_argv, &r->observed_argc) == 0;
    }
    if (hold_json_get_argv_alloc(j, &r->recipe.argv, &r->recipe.argc) != 0) {
        r->recipe.argv = NULL;
        r->recipe.argc = 0;
    }
    if (hold_json_get_env_alloc(j, &r->recipe.env, &r->recipe.envc) != 0) {
        r->recipe.env = NULL;
        r->recipe.envc = 0;
    }
    /* cmdline fallback chain: argv render <- cmdline_display <- "?". */
    if (hold_json_get_argv_display(j, r->cmdline, sizeof(r->cmdline)) != 0 &&
        hold_json_get_str(j, "cmdline_display", r->cmdline, sizeof(r->cmdline)) != 0) {
        if (r->recipe.argc <= 0 || !r->recipe.argv ||
            hold_format_argv_human(r->cmdline, sizeof(r->cmdline), r->recipe.argc, r->recipe.argv) != 0)
            snprintf(r->cmdline, sizeof(r->cmdline), "?");
    }
    free(j);
    return 0;
}

int hold_load_record_by_id(const char *dir, const char *id, struct hold_run_record *r, char *path, size_t n) {
    char resolved[ID_STR_LEN];
    if (hold_resolve_record_id(dir, id, resolved, sizeof(resolved)) != 0) return -1;
    if (hold_checked_snprintf(path, n, "%s/%s.json", dir, resolved) != 0) return -1;
    if (hold_load_record(path, r) != 0) return -1;
    if (!hold_valid_record(r) || strcmp(r->id, resolved) != 0) {
        hold_free_run_record(r);
        return -1;
    }
    return 0;
}
