#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "store_internal.h"

/* Record lifecycle: the two-phase creation reserve (hashed run ids),
 * teardown of loaded records, and the exit stamp (mark_run_finished),
 * including its purged-is-final semantics. */

static bool run_id_material_exists(const struct hold_store *store,
                                   const struct hold_store *avoid_public_store,
                                   const char *id) {
    char path[HOLD_PATH_MAX];
    return (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->record_dir, id) == 0 && hold_path_exists(path)) ||
           (hold_checked_snprintf(path, sizeof(path), "%s/%s.log", store->log_dir, id) == 0 && hold_path_exists(path)) ||
           (hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) == 0 && hold_path_exists(path)) ||
           (store->console_dir[0] &&
            hold_checked_snprintf(path, sizeof(path), "%s/%s.sock", store->console_dir, id) == 0 && hold_path_exists(path)) ||
           (store->public_dir[0] &&
            hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0 && hold_path_exists(path)) ||
           (avoid_public_store && avoid_public_store->public_dir[0] &&
            hold_checked_snprintf(path, sizeof(path), "%s/%s.json", avoid_public_store->public_dir, id) == 0 && hold_path_exists(path));
}

static void hash_field(struct sha256_ctx *ctx, const char *key, const char *value) {
    hold_sha256_update_nul_field(ctx, key);
    hold_sha256_update_nul_field(ctx, value ? value : "-");
}

/* The launch and adoption id materials, kept byte-for-byte distinct: the
 * scope tag ("hold-run-v1" vs "hold-run-adopt-v1") guarantees a launched and
 * an adopted id can never collide by construction. */
struct run_hash_material {
    const char *scope;   /* NULL = launch layout, else adoption layout */
    const char *exe;
    const char *cwd;
    int argc;
    char **argv;
    pid_t pid, pgid;     /* adoption: the adopted leader and group */
    int64_t start_unix_ns;
};

static void compute_run_hash(const struct run_hash_material *m, unsigned long counter,
                             char out[ID_STR_LEN]) {
    struct sha256_ctx ctx;
    unsigned char digest[32];
    char buf[64];
    hold_sha256_init(&ctx);
    if (m->scope) hash_field(&ctx, "scope", m->scope);
    else hash_field(&ctx, "version", "hold-run-v1");
    hash_field(&ctx, "exe", m->exe);
    hash_field(&ctx, "cwd", m->cwd && *m->cwd ? m->cwd : "-");
    snprintf(buf, sizeof(buf), "%" PRId64, m->start_unix_ns);
    hash_field(&ctx, m->scope ? "created_ns" : "timestamp_ns", buf);
    snprintf(buf, sizeof(buf), "%ld", (long)(m->scope ? m->pid : getpid()));
    hash_field(&ctx, m->scope ? "pid" : "launcher_pid", buf);
    if (m->scope) {
        snprintf(buf, sizeof(buf), "%ld", (long)m->pgid);
        hash_field(&ctx, "pgid", buf);
    }
    snprintf(buf, sizeof(buf), "%d", m->argc);
    hash_field(&ctx, "argc", buf);
    for (int i = 0; i < m->argc; i++) {
        snprintf(buf, sizeof(buf), "argv[%d]", i);
        hash_field(&ctx, buf, m->argv && m->argv[i] ? m->argv[i] : (m->scope ? NULL : ""));
    }
    if (!m->scope) {
        snprintf(buf, sizeof(buf), "%lu", counter);
        hash_field(&ctx, "counter", buf);
    } else if (counter > 0) {
        snprintf(buf, sizeof(buf), "%lu", counter);
        hash_field(&ctx, "collision", buf);
    }
    hold_sha256_final(&ctx, digest);
    hold_hex_encode(digest, sizeof(digest), out, ID_STR_LEN);
}

static int reserve_hashed_id(const struct hold_store *store,
                             const struct hold_store *avoid_public_store,
                             const struct run_hash_material *m,
                             char out_id[ID_STR_LEN]) {
    if (!store || !out_id) {
        errno = EINVAL;
        return -1;
    }
    char reserve[HOLD_PATH_MAX];
    for (unsigned long counter = 0; counter < 1024; counter++) {
        compute_run_hash(m, counter, out_id);
        if (!hold_valid_id(out_id) || run_id_material_exists(store, avoid_public_store, out_id)) continue;
        if (hold_checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", store->record_dir, out_id) != 0) return -1;
        int fd = open(reserve, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

int hold_reserve_run_id(const struct hold_store *store,
                        const char *resolved_exec_path,
                        int argc, char **argv,
                        const char *cwd,
                        int64_t start_unix_ns,
                        char out_id[ID_STR_LEN]) {
    struct run_hash_material m = {
        .exe = resolved_exec_path, .cwd = cwd, .argc = argc, .argv = argv,
        .start_unix_ns = start_unix_ns,
    };
    return reserve_hashed_id(store, NULL, &m, out_id);
}

int hold_reserve_adopted_run_id(const struct hold_store *store,
                                const struct hold_store *avoid_public_store,
                                const char *observed_exe,
                                int argc, char **argv,
                                const char *cwd,
                                pid_t pid, pid_t pgid,
                                int64_t start_unix_ns,
                                char out_id[ID_STR_LEN]) {
    struct run_hash_material m = {
        .scope = "hold-run-adopt-v1", .exe = observed_exe, .cwd = cwd,
        .argc = argc, .argv = argv, .pid = pid, .pgid = pgid,
        .start_unix_ns = start_unix_ns,
    };
    return reserve_hashed_id(store, avoid_public_store, &m, out_id);
}

void hold_abort_run_reservation(const struct hold_store *store, const char *id) {
    char path[HOLD_PATH_MAX];
    if (!store || !id || !*id) return;
    if (hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) == 0) {
        unlink(path);
    }
}

void hold_free_run_record(struct hold_run_record *r) {
    if (!r) return;
    hold_free_argv_alloc(r->recipe.argv, r->recipe.argc);
    hold_free_argv_alloc(r->recipe.env, r->recipe.envc);
    hold_free_argv_alloc(r->observed_argv, r->observed_argc);
    r->recipe.argv = NULL;
    r->recipe.env = NULL;
    r->observed_argv = NULL;
    r->recipe.argc = 0;
    r->recipe.envc = 0;
    r->observed_argc = 0;
}

static int wait_status_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 255;
}

int hold_mark_run_finished(const struct hold_store *store, const char *id, int status) {
    if (!store || !hold_valid_id(id)) {
        errno = EINVAL;
        return -1;
    }
    char path[HOLD_PATH_MAX];
    struct hold_run_record r;
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) return -1;
    struct stat old_st;
    bool have_old_st = false;
    int old_fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (old_fd >= 0) {
        have_old_st = fstat(old_fd, &old_st) == 0 && S_ISREG(old_st.st_mode);
        close(old_fd);
    }

    snprintf(r.state, sizeof(r.state), "%s", "exited");
    r.has_state = true;
    r.exit_code = wait_status_exit_code(status);
    r.has_exit_code = true;
    r.has_term_signal = WIFSIGNALED(status);
    r.term_signal = r.has_term_signal ? WTERMSIG(status) : 0;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        int64_t ended_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        hold_format_rfc3339_utc_from_ns(ended_ns, r.ended_at, sizeof(r.ended_at));
        r.has_ended_at = true;
    }

    int argc = r.recipe.argc;
    char **argv = r.recipe.argv;
    if (argc <= 0 || !argv) {
        argc = r.observed_argc;
        argv = r.observed_argv;
    }
    /* A purge may have removed the record between our load and this write
     * (force-purging a live call races the reaper's exit stamp); recreating
     * it here would resurrect a call the user just removed. The re-check
     * narrows that window to the stat-to-rename gap. Return 1 — distinct
     * from -1 — so callers can tell "purged, stay gone" (terminal) from a
     * load failure such as "record not written yet" (retryable). */
    struct stat still_there;
    if (stat(path, &still_there) != 0 && errno == ENOENT) {
        hold_free_run_record(&r);
        return 1;
    }
    /* A concurrent `hold save`/rename may have set the saved flag or renamed
     * the call after our load above. The exit stamp owns only state/exit code;
     * re-read the user-owned fields from the current record so this write does
     * not clobber that update (lost update). Best-effort: on a transient read
     * failure keep our loaded values. The residual window is the same
     * re-read-to-rename gap the stat check above already tolerates. */
    struct hold_run_record cur;
    char cur_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &cur, cur_path, sizeof(cur_path)) == 0) {
        r.saved = cur.saved;
        r.has_name = cur.has_name;
        if (cur.has_name) snprintf(r.name, sizeof(r.name), "%s", cur.name);
        hold_free_run_record(&cur);
    }
    char rewritten_path[HOLD_PATH_MAX] = {0};
    int rc = hold_write_record_atomic(store->record_dir, &r, argc, argv, rewritten_path, sizeof(rewritten_path));
    if (rc == 0 && have_old_st && geteuid() == 0 && rewritten_path[0]) {
        /* Root restores the record's prior ownership (a sudo-user store). */
        int rewritten_fd = open(rewritten_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (rewritten_fd < 0 || fchown(rewritten_fd, old_st.st_uid, old_st.st_gid) != 0) rc = -1;
        if (rewritten_fd >= 0) close(rewritten_fd);
    }
    if (rc == 0 && store->kind == STORE_SYSTEM_MANAGED && store->public_dir[0]) {
        /* A finishing call has no live ports; the projection clears them. */
        rc = hold_write_public_index_atomic(store, &r, NULL);
    }
    hold_free_run_record(&r);
    return rc;
}
