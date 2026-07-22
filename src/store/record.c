#include <stddef.h>

#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "store_internal.h"

/* The record schema, as one field table. The private writer walks it below,
 * the strict reader walks it in record_read.c; a field added here is written,
 * read, and presence-tracked without further code. Native snake_case keys are
 * the record format (the Docker-shaped parallel projection died in 0.7). */

#define F(key, type, flags, memb) \
    { key, type, flags, 0, offsetof(struct hold_run_record, memb), RF_ALWAYS }
#define FH(key, type, flags, memb, has) \
    { key, type, flags, 0, offsetof(struct hold_run_record, memb), offsetof(struct hold_run_record, has) }
#define S(key, flags, memb) \
    { key, RF_STR, flags, sizeof(((struct hold_run_record *)0)->memb), offsetof(struct hold_run_record, memb), RF_ALWAYS }
#define SH(key, flags, memb, has) \
    { key, RF_STR, flags, sizeof(((struct hold_run_record *)0)->memb), offsetof(struct hold_run_record, memb), offsetof(struct hold_run_record, has) }

const struct hold_record_field hold_record_fields[] = {
    F("saved", RF_BOOL, RF_WRONLY, saved),        /* reader shims legacy "Saved" (D-4) */
    F("version", RF_INT, RF_REQ, version),
    S("id", RF_REQ, id),
    S("run_id", RF_WRONLY, run_id),               /* reader defaults run_id <- id */
    F("pid", RF_PID, RF_REQ, pid),
    F("pgid", RF_PID, RF_REQ, pgid),
    F("sid", RF_PID, RF_REQ, sid),
    F("start_unix_ns", RF_I64, 0, start_unix_ns),
    F("created_unix_ns", RF_I64, RF_WRONLY, created_unix_ns), /* reader defaults <- start */
    SH("name", RF_ALIAS, name, has_name),
    SH("started_at", 0, started_at, has_started_at),
    SH("created_at", 0, created_at, has_created_at), /* reader defaults <- started_at */
    SH("ended_at", 0, ended_at, has_ended_at),
    SH("state", 0, state, has_state),
    FH("exit_code", RF_INT, 0, exit_code, has_exit_code),
    FH("term_signal", RF_INT, 0, term_signal, has_term_signal),
    SH("launch_error", 0, launch_error, has_launch_error),
    SH("console_sock", RF_ABS, console_sock, has_console),
    SH("restart", RF_NONEMPTY, recipe.restart_policy, recipe.has_restart_policy),
    FH("restart_delay_seconds", RF_INT, 0, recipe.restart_delay_seconds, recipe.has_restart_delay),
    FH("invoked_by_uid", RF_UID, 0, invoked_by_uid, has_invocation),
    FH("invoked_by_gid", RF_GID, 0, invoked_by_gid, has_invocation),
    SH("invoked_by_user", 0, invoked_by_user, has_invocation),
    FH("invoked_via_sudo", RF_BOOL, 0, invoked_via_sudo, has_invocation),
    F("uid", RF_UID, RF_REQ, uid),
    F("gid", RF_GID, RF_REQ, gid),
    SH("log_path", 0, log_path, has_log),         /* writer derives log_idx_path beside it */
    SH("boot_id", 0, boot_id, has_boot),
    F("proc_starttime_ticks", RF_U64, RF_REQ, proc_starttime_ticks),
    F("exe_dev", RF_U64, RF_REQ, exe_dev),
    F("exe_ino", RF_U64, RF_REQ, exe_ino),
};
const size_t hold_record_field_count = sizeof(hold_record_fields) / sizeof(hold_record_fields[0]);

/* The one running-record base: everything the launch and adoption paths would
 * otherwise stamp field-by-field in parallel. The table above stays the
 * schema authority; this only fills values. */
void hold_record_init_running(struct hold_run_record *r,
                              const char *id,
                              const char *log_path,
                              pid_t pid, pid_t pgid, pid_t sid,
                              int64_t start_unix_ns,
                              int64_t created_unix_ns) {
    memset(r, 0, sizeof(*r));
    r->version = 1;
    snprintf(r->id, sizeof(r->id), "%s", id);
    snprintf(r->run_id, sizeof(r->run_id), "%s", id);
    r->pid = pid;
    r->pgid = pgid;
    r->sid = sid;
    r->start_unix_ns = start_unix_ns;
    r->created_unix_ns = created_unix_ns;
    hold_format_rfc3339_utc_from_ns(start_unix_ns, r->started_at, sizeof(r->started_at));
    r->has_started_at = true;
    hold_format_rfc3339_utc_from_ns(created_unix_ns, r->created_at, sizeof(r->created_at));
    r->has_created_at = true;
    snprintf(r->state, sizeof(r->state), "running");
    r->has_state = true;
    r->uid = geteuid();
    r->gid = getegid();
    r->has_log = true;
    snprintf(r->log_path, sizeof(r->log_path), "%s", log_path);
}

void hold_emit_kv(FILE *f, bool *first, const char *key) {
    fprintf(f, "%s  \"%s\": ", *first ? "" : ",\n", key);
    *first = false;
}

void hold_emit_kv_str(FILE *f, bool *first, const char *key, const char *val) {
    hold_emit_kv(f, first, key);
    fputc('"', f);
    hold_json_escape(f, val);
    fputc('"', f);
}

void hold_record_emit_fields(FILE *f, bool *first, const struct hold_run_record *r) {
    for (size_t i = 0; i < hold_record_field_count; i++) {
        const struct hold_record_field *fs = &hold_record_fields[i];
        const char *p = (const char *)r + fs->off;
        if (fs->has_off != RF_ALWAYS && !*(const bool *)((const char *)r + fs->has_off)) {
            continue;
        }
        switch (fs->type) {
        case RF_STR: hold_emit_kv_str(f, first, fs->key, p); break;
        case RF_BOOL: hold_emit_kv(f, first, fs->key); fputs(*(const bool *)p ? "true" : "false", f); break;
        case RF_INT: hold_emit_kv(f, first, fs->key); fprintf(f, "%d", *(const int *)p); break;
        case RF_I64: hold_emit_kv(f, first, fs->key); fprintf(f, "%" PRId64, *(const int64_t *)p); break;
        case RF_U64: hold_emit_kv(f, first, fs->key); fprintf(f, "%" PRIu64, *(const uint64_t *)p); break;
        case RF_PID: hold_emit_kv(f, first, fs->key); fprintf(f, "%ld", (long)*(const pid_t *)p); break;
        case RF_UID: hold_emit_kv(f, first, fs->key); fprintf(f, "%u", (unsigned)*(const uid_t *)p); break;
        case RF_GID: hold_emit_kv(f, first, fs->key); fprintf(f, "%u", (unsigned)*(const gid_t *)p); break;
        }
    }
}

struct record_emit_ctx {
    const struct hold_run_record *r;
    int argc;
    char **argv;
};

static int emit_record_json(FILE *f, void *vctx) {
    const struct record_emit_ctx *c = vctx;
    const struct hold_run_record *r = c->r;
    bool first = true;
    fprintf(f, "{\n");
    hold_record_emit_fields(f, &first, r);
    hold_emit_kv(f, &first, "argv");
    hold_write_json_argv(f, c->argc, c->argv);
    hold_emit_kv(f, &first, "normalized");
    fprintf(f, "{ \"argv\": ");
    hold_write_json_argv(f, c->argc, c->argv);
    fprintf(f, " }");
    if (r->has_observed) {
        hold_emit_kv(f, &first, "observed");
        fprintf(f, "{ \"exe\": \"");
        hold_json_escape(f, r->observed_exe);
        fprintf(f, "\", \"argv\": ");
        hold_write_json_argv(f, r->observed_argc, r->observed_argv);
        fprintf(f, ", \"cwd\": \"");
        hold_json_escape(f, r->observed_cwd);
        fprintf(f, "\" }");
    }
    if (r->recipe.envc > 0 && r->recipe.env) {
        hold_emit_kv(f, &first, "env");
        hold_write_json_argv(f, r->recipe.envc, r->recipe.env);
    }
    if (r->recipe.mode_interactive || r->recipe.mode_tty || r->recipe.mode_detach || r->recipe.allow_multi) {
        hold_emit_kv(f, &first, "mode");
        const char *sep = "{";
        if (r->recipe.mode_interactive) { fprintf(f, "%s\"interactive\": true", sep); sep = ", "; }
        if (r->recipe.mode_tty) { fprintf(f, "%s\"tty\": true", sep); sep = ", "; }
        if (r->recipe.mode_detach) { fprintf(f, "%s\"detach\": true", sep); sep = ", "; }
        if (r->recipe.allow_multi) { fprintf(f, "%s\"allow_multi\": true", sep); sep = ", "; }
        fprintf(f, "}");
    }
    if (r->has_log) {
        char idx[HOLD_PATH_MAX];
        if (hold_log_idx_path(r->log_path, idx, sizeof(idx)) == 0) {
            hold_emit_kv_str(f, &first, "log_idx_path", idx);
        }
    }
    fprintf(f, "\n}\n");
    return 0;
}

int hold_write_record_atomic(const char *dir, const struct hold_run_record *r, int argc, char **argv, char *out_json_path, size_t out_n) {
    struct hold_run_record rec = *r;
    char name[ID_STR_LEN + 8], reserve[HOLD_PATH_MAX];
    /* Writer-side defaults, so the table can write every field verbatim. */
    if (!rec.run_id[0]) {
        snprintf(rec.run_id, sizeof(rec.run_id), "%s", rec.id);
    }
    if (rec.created_unix_ns == 0) {
        rec.created_unix_ns = rec.start_unix_ns;
    }
    struct record_emit_ctx ctx = { &rec, argc, argv };
    if (hold_checked_snprintf(name, sizeof(name), "%s.json", rec.id) != 0) {
        return -1;
    }
    if (hold_atomic_write_json(dir, name, 0600, false, emit_record_json, &ctx, out_json_path, out_n) != 0) {
        return -1;
    }
    if (hold_checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", dir, rec.id) == 0) {
        unlink(reserve);
    }
    return 0;
}
