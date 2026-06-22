#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/runtime.h"
#include "sigmund/runtime_internal.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"
#include "sigmund/store.h"
#include "sigmund/console.h"
#include "sigmund/access.h"

struct list_row {
    char id[16];
    char state[16];
    char started[64];
    char result[64];
    char cmd[SIGMUND_PATH_MAX];
    int64_t start_unix_ns;
    bool running;
};

struct list_rows {
    struct list_row *items;
    size_t count;
};

static void format_result(const struct sigmund_run_record *r, enum run_state st, char *out, size_t n);
static void free_list_rows(struct list_rows *rows);
static int append_list_row(struct list_rows *rows, const struct list_row *row);
static int compare_list_rows(const void *a, const void *b);
static void print_list_header(bool iso);
static void print_list_row(const struct list_row *row, bool iso);
static int collect_list_private(const struct sigmund_store *store,
                                const char *alias_filter,
                                bool iso,
                                struct list_rows *rows);
static int collect_list_public(const struct sigmund_store *store,
                               const char *alias_filter,
                               bool iso,
                               struct list_rows *rows);
static int print_collected_list(struct list_rows *rows, bool iso);
static void unlink_public_index(const struct sigmund_store *store, const char *id);
static int cmd_prune_store_all(const struct sigmund_store *store, bool include_stale, int *removed_count);

static void format_result(const struct sigmund_run_record *r, enum run_state st, char *out, size_t n) {
    if (st == STATE_RUNNING) {
        snprintf(out, n, "%s", r->has_console ? "console" : "-");
        return;
    }
    if (r->has_launch_error && r->launch_error[0]) {
        snprintf(out, n, "launch=%.48s", r->launch_error);
        return;
    }
    if (r->has_term_signal) {
        snprintf(out, n, "signal=%d", r->term_signal);
        return;
    }
    if (r->has_exit_code) {
        snprintf(out, n, "exit=%d", r->exit_code);
        return;
    }
    if (st == STATE_FAILED) {
        snprintf(out, n, "launch=unknown");
    } else {
        snprintf(out, n, "-");
    }
}

static void free_list_rows(struct list_rows *rows) {
    free(rows->items);
    rows->items = NULL;
    rows->count = 0;
}

static int append_list_row(struct list_rows *rows, const struct list_row *row) {
    struct list_row *next = realloc(rows->items, (rows->count + 1) * sizeof(*rows->items));
    if (!next) {
        return -1;
    }
    rows->items = next;
    rows->items[rows->count++] = *row;
    return 0;
}

static int compare_list_rows(const void *a, const void *b) {
    const struct list_row *ra = (const struct list_row *)a;
    const struct list_row *rb = (const struct list_row *)b;
    if (ra->running != rb->running) {
        return ra->running ? -1 : 1;
    }
    if (ra->start_unix_ns > rb->start_unix_ns) {
        return -1;
    }
    if (ra->start_unix_ns < rb->start_unix_ns) {
        return 1;
    }
    return strcmp(ra->id, rb->id);
}

static void print_list_header(bool iso) {
    printf("%-10s %-8s %-*s %-10s %s\n", "RUNID", "STATE", iso ? 24 : 8, iso ? "STARTED_AT" : "STARTED", "RESULT", "CMD");
}

static void print_list_row(const struct list_row *row, bool iso) {
    char cmd[80];
    const char *src = row->cmd[0] ? row->cmd : "?";
    snprintf(cmd, sizeof(cmd), "%.72s%s", src, strlen(src) > 72 ? "..." : "");
    printf("%-10s %-8s %-*s %-10s %s\n", row->id, row->state, iso ? 24 : 8, row->started, row->result, cmd);
}

static int collect_list_private(const struct sigmund_store *store,
                                const char *alias_filter,
                                bool iso,
                                struct list_rows *rows) {
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!sigmund_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (sigmund_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct sigmund_run_record r;
        if (sigmund_load_record(path, &r) != 0) {
            fprintf(stderr, "sigmund: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        if (!sigmund_valid_record(&r) || strcmp(r.id, file_id) != 0) {
            fprintf(stderr, "sigmund: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        if (alias_filter && (!r.has_alias || strcmp(r.alias, alias_filter) != 0)) {
            continue;
        }
        enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
        struct list_row row;
        memset(&row, 0, sizeof(row));
        snprintf(row.id, sizeof(row.id), "%s", r.id);
        snprintf(row.state, sizeof(row.state), "%s", sigmund_state_str(st));
        row.start_unix_ns = r.start_unix_ns;
        row.running = st == STATE_RUNNING;
        if (iso) {
            if (r.has_started_at && r.started_at[0]) {
                snprintf(row.started, sizeof(row.started), "%s", r.started_at);
            } else {
                sigmund_format_rfc3339_utc_from_ns(r.start_unix_ns, row.started, sizeof(row.started));
            }
        } else {
            sigmund_format_relative_age(r.start_unix_ns, row.started, sizeof(row.started));
        }
        format_result(&r, st, row.result, sizeof(row.result));
        snprintf(row.cmd, sizeof(row.cmd), "%s", r.cmdline[0] ? r.cmdline : "?");
        if (append_list_row(rows, &row) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int collect_list_public(const struct sigmund_store *store,
                               const char *alias_filter,
                               bool iso,
                               struct list_rows *rows) {
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!sigmund_has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= 16) {
            continue;
        }
        char file_id[16];
        memcpy(file_id, e->d_name, len - 5);
        file_id[len - 5] = '\0';
        if (!sigmund_valid_id(file_id)) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (sigmund_checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        struct sigmund_public_index pi;
        if (sigmund_load_public_index(path, &pi) != 0 || strcmp(pi.id, file_id) != 0) {
            continue;
        }
        if (alias_filter && (!pi.has_alias || strcmp(pi.alias, alias_filter) != 0)) {
            continue;
        }
        struct list_row row;
        memset(&row, 0, sizeof(row));
        snprintf(row.id, sizeof(row.id), "%s", pi.id);
        snprintf(row.state, sizeof(row.state), "%s", "unknown");
        row.running = false;
        row.start_unix_ns = 0;
        if (iso) {
            snprintf(row.started, sizeof(row.started), "%s", pi.started_at[0] ? pi.started_at : "-");
        } else {
            snprintf(row.started, sizeof(row.started), "%s", "-");
        }
        snprintf(row.result, sizeof(row.result), "%s", "-");
        snprintf(row.cmd, sizeof(row.cmd), "%s", "<root-managed>");
        if (append_list_row(rows, &row) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int print_collected_list(struct list_rows *rows, bool iso) {
    if (rows->count > 1) {
        qsort(rows->items, rows->count, sizeof(rows->items[0]), compare_list_rows);
    }
    print_list_header(iso);
    for (size_t i = 0; i < rows->count; i++) {
        print_list_row(&rows->items[i], iso);
    }
    return 0;
}

int sigmund_cmd_list_normal(const struct sigmund_store *user_store,
                           const struct sigmund_store *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(user_store, alias_filter, iso, &rows) != 0 ||
        collect_list_public(system_store, alias_filter, iso, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
}

int sigmund_cmd_list_system(const struct sigmund_store *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(system_store, alias_filter, iso, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
}

static void unlink_public_index(const struct sigmund_store *store, const char *id) {
    if (store->kind != STORE_SYSTEM_MANAGED || !id || !*id) {
        return;
    }
    char path[SIGMUND_PATH_MAX];
    if (sigmund_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0) {
        unlink(path);
    }
}

int sigmund_prune_one_run(const struct sigmund_store *store, const char *id, const char *boot, bool allow_stale, bool *removed) {
    struct sigmund_run_record r;
    char path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    enum run_state st = sigmund_eval_state(&r, boot ? boot : NULL);
    bool prunable = (st == STATE_EXITED || st == STATE_FAILED || (allow_stale && st == STATE_STALE));
    if (!prunable) {
        fprintf(stderr, "sigmund: error: run %s is %s and cannot be pruned\n", id, sigmund_state_str(st));
        return 2;
    }
    unlink(path);
    if (r.has_log) {
        unlink(r.log_path);
    }
    if (r.has_console) {
        unlink(r.console_sock);
    }
    unlink_public_index(store, id);
    if (removed) {
        *removed = true;
    }
    return 0;
}

static int cmd_prune_store_all(const struct sigmund_store *store, bool include_stale, int *removed_count) {
    if (removed_count) {
        *removed_count = 0;
    }
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));

    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!sigmund_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (sigmund_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct sigmund_run_record r;
        if (sigmund_load_record(path, &r) != 0) {
            unlink(path);
            continue;
        }
        if (!sigmund_valid_record(&r) || strcmp(r.id, file_id) != 0) {
            unlink(path);
            continue;
        }
        enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
        if (st == STATE_EXITED || st == STATE_FAILED || (include_stale && st == STATE_STALE)) {
            unlink(path);
            if (r.has_log) {
                unlink(r.log_path);
            }
            if (r.has_console) {
                unlink(r.console_sock);
            }
            unlink_public_index(store, r.id);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);

    d = opendir(store->log_dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d))) {
        if (!sigmund_has_suffix(e->d_name, ".log")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 4) {
            continue;
        }
        char id[32];
        size_t id_len = len - 4;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!sigmund_valid_id(id)) {
            continue;
        }
        char json_path[SIGMUND_PATH_MAX], log_path[SIGMUND_PATH_MAX];
        if (sigmund_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0) {
            continue;
        }
        if (sigmund_checked_snprintf(log_path, sizeof(log_path), "%s/%s", store->log_dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            unlink(log_path);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);
    d = opendir(store->console_dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d))) {
        if (!sigmund_has_suffix(e->d_name, ".sock")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5) {
            continue;
        }
        char id[32];
        size_t id_len = len - 5;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!sigmund_valid_id(id)) {
            continue;
        }
        char json_path[SIGMUND_PATH_MAX], sock_path[SIGMUND_PATH_MAX];
        if (sigmund_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0 ||
            sigmund_checked_snprintf(sock_path, sizeof(sock_path), "%s/%s", store->console_dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            unlink(sock_path);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);
    return 0;
}

int sigmund_cmd_prune_action(const struct sigmund_invocation *inv,
                            const struct sigmund_store *user_store,
                            const struct sigmund_store *system_store,
                            const char *program,
                            const char *target_token,
                            bool all) {
    if (!target_token || strcmp(target_token, "all") == 0) {
        const struct sigmund_store *store = inv->euid_root ? system_store : user_store;
        int removed = 0;
        int rc = cmd_prune_store_all(store, target_token && strcmp(target_token, "all") == 0, &removed);
        if (rc == 0) {
            if (removed > 0) {
                sigmund_sig_note(inv, "sigmund: pruned %d past run%s\n", removed, removed == 1 ? "" : "s");
            } else {
                sigmund_sig_note(inv, "sigmund: nothing to prune\n");
            }
        }
        return rc;
    }
    struct sigmund_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = sigmund_resolve_action_token(inv, user_store, system_store, "prune", target_token, all, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sigmund_sig_note(inv, "sigmund: nothing to prune\n");
        return 0;
    }
    bool need_elevation = false;
    for (int i = 0; i < ntargets; i++) {
        need_elevation = need_elevation || targets[i].needs_elevation;
    }
    if (need_elevation) {
        rc = sigmund_elevate_with_sudo_targets(program, "prune", NULL, targets, ntargets, all, false);
        free(targets);
        return rc;
    }
    char boot[128] = {0};
    bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
    int worst = 0;
    int removed_count = 0;
    for (int i = 0; i < ntargets; i++) {
        bool removed = false;
        rc = sigmund_prune_one_run(&targets[i].store, targets[i].id, have_boot ? boot : NULL, true, &removed);
        if (removed) {
            removed_count++;
        }
        if (rc > worst) {
            worst = rc;
        }
    }
    if (worst == 0) {
        if (removed_count > 0) {
            const char *atom = NULL;
            enum id_token_scope token_scope = sigmund_parse_id_token(target_token, &atom);
            bool target_looks_like_alias = (token_scope != ID_TOKEN_INVALID && atom &&
                                            sigmund_valid_alias(atom) && !sigmund_valid_id_prefix(atom));
            if (target_looks_like_alias) {
                sigmund_sig_note(inv, "sigmund: pruned %d past run%s for '%s'\n",
                         removed_count, removed_count == 1 ? "" : "s", atom);
            } else {
                sigmund_sig_note(inv, "sigmund: pruned %d past run%s\n", removed_count, removed_count == 1 ? "" : "s");
            }
        } else {
            sigmund_sig_note(inv, "sigmund: nothing to prune\n");
        }
    }
    free(targets);
    return worst;
}
