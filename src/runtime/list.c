#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"
#include "hold/observe.h"

struct list_row {
    char id[ID_STR_LEN];
    char name[ALIAS_MAX_LEN + 1];
    char state[16];
    char started[64];
    char created[64];
    char status[96];
    char ports[HOLD_PATH_MAX];
    char cmd[HOLD_PATH_MAX];
    int64_t start_unix_ns;
    bool running;
};

struct list_rows {
    struct list_row *items;
    size_t count;
};

static void free_list_rows(struct list_rows *rows);
static int append_list_row(struct list_rows *rows, const struct list_row *row);
static int compare_list_rows(const void *a, const void *b);
static void print_list_header(bool iso);
static void print_list_row(const struct list_row *row, bool iso);
static int collect_list_private(const struct hold_store *store,
                                const char *alias_filter,
                                bool iso,
                                bool include_all,
                                bool docker_ps,
                                struct list_rows *rows);
static int collect_list_public(const struct hold_store *store,
                               const char *alias_filter,
                               bool iso,
                               bool include_all,
                               bool docker_ps,
                               struct list_rows *rows);
static int print_collected_list(struct list_rows *rows, bool iso);
static int print_collected_ps(struct list_rows *rows);
static void unlink_public_index(const struct hold_store *store, const char *id);
static void unlink_log_index_for_log(const char *log_path);
static int cmd_prune_store_all(const struct hold_store *store, bool include_stale, int *removed_count);

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
    printf("%-12s %-8s %-*s %s\n", "CALL ID", "STATE", iso ? 24 : 8, iso ? "STARTED_AT" : "STARTED", "CMD");
}

static void print_list_row(const struct list_row *row, bool iso) {
    char cmd[80];
    const char *src = row->cmd[0] ? row->cmd : "?";
    snprintf(cmd, sizeof(cmd), "%.72s%s", src, strlen(src) > 72 ? "..." : "");
    printf("%-12s %-8s %-*s %s\n", row->id, row->state, iso ? 24 : 8, row->started, cmd);
}

/* Docker double-quotes COMMAND and ellipsizes it so one long command never
 * blows out the column. Up to PS_CMD_CHARS characters of the command survive;
 * anything longer ends in an ellipsis inside the quotes. */
#define PS_CMD_CHARS 30
#define PS_CMD_CELL (PS_CMD_CHARS + 8) /* quotes + "..." + NUL */

static void quote_command(char out[PS_CMD_CELL], const char *cmd) {
    const char *src = (cmd && *cmd) ? cmd : "?";
    char body[PS_CMD_CHARS + 4];
    size_t len = strlen(src);
    /* ASCII ellipsis keeps byte width equal to display width, so the
     * content-sized columns below never shear on a truncated command. */
    snprintf(body, sizeof(body), "%.*s%s", PS_CMD_CHARS, src, len > PS_CMD_CHARS ? "..." : "");
    snprintf(out, PS_CMD_CELL, "\"%s\"", body);
}

/* Seconds elapsed since a past instant given in unix nanoseconds. */
static int64_t seconds_since(int64_t ref_unix_ns) {
    struct timespec now;
    if (ref_unix_ns <= 0 || clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0;
    }
    int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    return now_ns > ref_unix_ns ? (now_ns - ref_unix_ns) / 1000000000LL : 0;
}

/* The instant a call last stopped running: its recorded end time when we have
 * one, otherwise its start (a good enough anchor for short-lived calls and the
 * behavior Hold shipped before the ended timestamp was parseable). */
static int64_t ended_reference_ns(const struct hold_run_record *r) {
    int64_t ns = 0;
    if (r->has_ended_at && hold_parse_rfc3339_utc_to_ns(r->ended_at, &ns)) {
        return ns;
    }
    return r->start_unix_ns;
}

static void format_status(enum run_state st, const struct hold_run_record *r, char *out, size_t n) {
    char human[48];
    switch (st) {
    case STATE_RUNNING:
        hold_format_duration_human(seconds_since(r->start_unix_ns), human, sizeof(human));
        snprintf(out, n, "Up %s", human);
        break;
    case STATE_EXITED:
        hold_format_duration_human(seconds_since(ended_reference_ns(r)), human, sizeof(human));
        if (r->has_exit_code) {
            snprintf(out, n, "Exited (%d) %s ago", r->exit_code, human);
        } else if (r->has_term_signal) {
            snprintf(out, n, "Exited (%d) %s ago", 128 + r->term_signal, human);
        } else {
            snprintf(out, n, "Exited (?) %s ago", human);
        }
        break;
    case STATE_FAILED:
        hold_format_duration_human(seconds_since(ended_reference_ns(r)), human, sizeof(human));
        snprintf(out, n, "Failed %s ago", human);
        break;
    case STATE_STALE:
        /* Stale is Hold-specific: the call's boot id no longer matches, so it
         * cannot be running. Report the age without a false exit story. */
        hold_format_duration_human(seconds_since(r->start_unix_ns), human, sizeof(human));
        snprintf(out, n, "Stale %s", human);
        break;
    default:
        snprintf(out, n, "Unknown");
        break;
    }
}

static int collect_list_private(const struct hold_store *store,
                                const char *alias_filter,
                                bool iso,
                                bool include_all,
                                bool docker_ps,
                                struct list_rows *rows) {
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct hold_run_record r;
        if (hold_load_record(path, &r) != 0) {
            fprintf(stderr, "hold: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        if (!hold_valid_record(&r) || strcmp(r.id, file_id) != 0) {
            fprintf(stderr, "hold: warning: skipping corrupt record %s\n", e->d_name);
            hold_free_run_record(&r);
            continue;
        }
        if (alias_filter && (!r.has_alias || strcmp(r.alias, alias_filter) != 0)) {
            hold_free_run_record(&r);
            continue;
        }
        enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
        if (!include_all && st != STATE_RUNNING) {
            hold_free_run_record(&r);
            continue;
        }
        struct list_row row;
        memset(&row, 0, sizeof(row));
        char display_id[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(r.id, display_id);
        snprintf(row.id, sizeof(row.id), "%s", display_id);
        if (r.has_name && r.name[0]) snprintf(row.name, sizeof(row.name), "%s", r.name);
        snprintf(row.state, sizeof(row.state), "%s", hold_state_str(st));
        row.start_unix_ns = r.start_unix_ns;
        row.running = st == STATE_RUNNING;
        int64_t created_ns = r.has_created_at && r.created_unix_ns > 0 ? r.created_unix_ns : r.start_unix_ns;
        if (iso) {
            if (r.has_started_at && r.started_at[0]) {
                snprintf(row.started, sizeof(row.started), "%s", r.started_at);
            } else {
                hold_format_rfc3339_utc_from_ns(r.start_unix_ns, row.started, sizeof(row.started));
            }
            if (r.has_created_at && r.created_at[0]) {
                snprintf(row.created, sizeof(row.created), "%s", r.created_at);
            } else {
                hold_format_rfc3339_utc_from_ns(created_ns, row.created, sizeof(row.created));
            }
        } else {
            hold_format_relative_age(r.start_unix_ns, row.started, sizeof(row.started));
            hold_format_relative_age(created_ns, row.created, sizeof(row.created));
        }
        snprintf(row.cmd, sizeof(row.cmd), "%s", r.cmdline[0] ? r.cmdline : "?");
        if (docker_ps) {
            if (!iso) {
                /* CREATED is humanized Docker-style: "About a minute ago",
                 * "2 days ago" - never the "2m" short form the list table uses. */
                char human[48];
                hold_format_duration_human(seconds_since(created_ns), human, sizeof(human));
                snprintf(row.created, sizeof(row.created), "%s ago", human);
            }
            format_status(st, &r, row.status, sizeof(row.status));
            if (r.saved) {
                /* Surface protection where the eye already rests: a suffix on
                 * STATUS, not a new column. */
                size_t used = strlen(row.status);
                snprintf(row.status + used, sizeof(row.status) - used, " (saved)");
            }
            if (st == STATE_RUNNING) {
                hold_observe_run_ports_column(&r, row.ports, sizeof(row.ports));
            }
        }
        if (append_list_row(rows, &row) != 0) {
            hold_free_run_record(&r);
            closedir(d);
            return -1;
        }
        hold_free_run_record(&r);
    }
    closedir(d);
    return 0;
}

static int collect_list_public(const struct hold_store *store,
                               const char *alias_filter,
                               bool iso,
                               bool include_all,
                               bool docker_ps,
                               struct list_rows *rows) {
    if (!include_all) {
        return 0;
    }
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= ID_STR_LEN) {
            continue;
        }
        char file_id[ID_STR_LEN];
        memcpy(file_id, e->d_name, len - 5);
        file_id[len - 5] = '\0';
        if (!hold_valid_id(file_id)) {
            continue;
        }
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        struct hold_public_index pi;
        if (hold_load_public_index(path, &pi) != 0 || strcmp(pi.id, file_id) != 0) {
            continue;
        }
        if (alias_filter && (!pi.has_alias || strcmp(pi.alias, alias_filter) != 0)) {
            continue;
        }
        struct list_row row;
        memset(&row, 0, sizeof(row));
        char display_id[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(pi.id, display_id);
        snprintf(row.id, sizeof(row.id), "%s", display_id);
        if (pi.has_name && pi.name[0]) snprintf(row.name, sizeof(row.name), "%s", pi.name);
        snprintf(row.state, sizeof(row.state), "%s", pi.state_hint[0] ? pi.state_hint : "unknown");
        row.running = !strcmp(row.state, "running");
        row.start_unix_ns = 0;
        if (iso) {
            snprintf(row.started, sizeof(row.started), "%s", pi.started_at[0] ? pi.started_at : "-");
        } else {
            snprintf(row.started, sizeof(row.started), "%s", "-");
        }
        snprintf(row.created, sizeof(row.created), "%s", pi.created_at[0] ? pi.created_at : row.started);
        snprintf(row.cmd, sizeof(row.cmd), "%s", "<root-managed>");
        if (docker_ps) {
            snprintf(row.status, sizeof(row.status), "%s", pi.state_hint[0] ? pi.state_hint : "Unknown");
            if (!iso) {
                /* The table never shows a raw ISO stamp: humanize the public
                 * index's created_at, falling back to "-" when unparseable. */
                int64_t created_at_ns = 0;
                if (hold_parse_rfc3339_utc_to_ns(pi.created_at, &created_at_ns)) {
                    char human[48];
                    hold_format_duration_human(seconds_since(created_at_ns), human, sizeof(human));
                    snprintf(row.created, sizeof(row.created), "%s ago", human);
                } else {
                    snprintf(row.created, sizeof(row.created), "-");
                }
            }
        }
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

/* The Docker-shaped call table, content-sized like `docker ps`: a first pass
 * measures every column from the actual rows (never a fixed printf width that
 * shears a long value), a second pass prints. Columns are separated by a
 * two-space gutter; each column is at least as wide as its header; the final
 * NAMES column is never padded, so no line carries trailing spaces. */
static int print_collected_ps(struct list_rows *rows) {
    if (rows->count > 1) {
        qsort(rows->items, rows->count, sizeof(rows->items[0]), compare_list_rows);
    }

    char (*cmds)[PS_CMD_CELL] = NULL;
    if (rows->count > 0) {
        cmds = malloc(rows->count * sizeof(*cmds));
        if (!cmds) {
            return 3;
        }
    }

    size_t w_id = strlen("CALL ID");
    size_t w_cmd = strlen("COMMAND");
    size_t w_created = strlen("CREATED");
    size_t w_status = strlen("STATUS");
    size_t w_ports = strlen("PORTS");
    for (size_t i = 0; i < rows->count; i++) {
        const struct list_row *r = &rows->items[i];
        quote_command(cmds[i], r->cmd);
        const char *created = r->created[0] ? r->created : "-";
        const char *status = r->status[0] ? r->status : r->state;
        if (strlen(r->id) > w_id) w_id = strlen(r->id);
        if (strlen(cmds[i]) > w_cmd) w_cmd = strlen(cmds[i]);
        if (strlen(created) > w_created) w_created = strlen(created);
        if (strlen(status) > w_status) w_status = strlen(status);
        if (strlen(r->ports) > w_ports) w_ports = strlen(r->ports);
    }

    printf("%-*s  %-*s  %-*s  %-*s  %-*s  %s\n",
           (int)w_id, "CALL ID",
           (int)w_cmd, "COMMAND",
           (int)w_created, "CREATED",
           (int)w_status, "STATUS",
           (int)w_ports, "PORTS",
           "NAMES");
    for (size_t i = 0; i < rows->count; i++) {
        const struct list_row *r = &rows->items[i];
        printf("%-*s  %-*s  %-*s  %-*s  %-*s  %s\n",
               (int)w_id, r->id,
               (int)w_cmd, cmds[i],
               (int)w_created, r->created[0] ? r->created : "-",
               (int)w_status, r->status[0] ? r->status : r->state,
               (int)w_ports, r->ports,
               r->name[0] ? r->name : "-");
    }
    free(cmds);
    return 0;
}

int hold_cmd_list_normal(const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(user_store, alias_filter, iso, true, false, &rows) != 0 ||
        collect_list_public(system_store, alias_filter, iso, true, false, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
}

int hold_cmd_list_system(const struct hold_store *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(system_store, alias_filter, iso, true, false, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
}

int hold_cmd_ps_normal(const struct hold_store *user_store,
                       const struct hold_store *system_store,
                       bool all) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(user_store, NULL, false, all, true, &rows) != 0 ||
        collect_list_public(system_store, NULL, false, all, true, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_ps(&rows);
    }
    free_list_rows(&rows);
    return rc;
}

int hold_cmd_ps_system(const struct hold_store *system_store, bool all) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(system_store, NULL, false, all, true, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_ps(&rows);
    }
    free_list_rows(&rows);
    return rc;
}

static void unlink_public_index(const struct hold_store *store, const char *id) {
    if (store->kind != STORE_SYSTEM_MANAGED || !id || !*id) {
        return;
    }
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0) {
        unlink(path);
    }
}

static void unlink_log_index_for_log(const char *log_path) {
    char idx[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx, sizeof(idx)) == 0) {
        unlink(idx);
    }
}

int hold_prune_one_run(const struct hold_store *store, const char *id, const char *boot, bool allow_stale, bool *removed) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    enum run_state st = hold_eval_state(&r, boot ? boot : NULL);
    bool prunable = (st == STATE_EXITED || st == STATE_FAILED || (allow_stale && st == STATE_STALE));
    if (!prunable) {
        fprintf(stderr, "hold: error: call %s is %s and cannot be purged\n", id, hold_state_str(st));
        hold_free_run_record(&r);
        return 2;
    }
    unlink(path);
    if (r.has_log) {
        unlink_log_index_for_log(r.log_path);
        unlink(r.log_path);
    }
    if (r.has_console) {
        unlink(r.console_sock);
    }
    unlink_public_index(store, id);
    if (removed) {
        *removed = true;
    }
    hold_free_run_record(&r);
    return 0;
}

static int cmd_prune_store_all(const struct hold_store *store, bool include_stale, int *removed_count) {
    if (removed_count) {
        *removed_count = 0;
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    const struct dirent *e;
    DIR *d = opendir(store->record_dir);
    if (!d) {
        goto sweep_logs;
    }
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct hold_run_record r;
        if (hold_load_record(path, &r) != 0) {
            unlink(path);
            continue;
        }
        if (!hold_valid_record(&r) || strcmp(r.id, file_id) != 0) {
            unlink(path);
            hold_free_run_record(&r);
            continue;
        }
        enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
        /* A sweeping purge silently skips saved calls; only a targeted
         * purge --force removes them. */
        if (!r.saved && (st == STATE_EXITED || st == STATE_FAILED || (include_stale && st == STATE_STALE))) {
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
        hold_free_run_record(&r);
    }
    closedir(d);

sweep_logs:
    d = opendir(store->log_dir);
    if (!d) {
        goto sweep_consoles;
    }
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".log")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 4) {
            continue;
        }
        char id[ID_STR_LEN];
        size_t id_len = len - 4;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!hold_valid_id(id)) {
            continue;
        }
        char json_path[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0) {
            continue;
        }
        if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s", store->log_dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            unlink_log_index_for_log(log_path);
            unlink(log_path);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);

sweep_consoles:
    d = opendir(store->console_dir);
    if (!d) {
        goto sweep_public;
    }
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".sock")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5) {
            continue;
        }
        char id[ID_STR_LEN];
        size_t id_len = len - 5;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!hold_valid_id(id)) {
            continue;
        }
        char json_path[HOLD_PATH_MAX], sock_path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0 ||
            hold_checked_snprintf(sock_path, sizeof(sock_path), "%s/%s", store->console_dir, e->d_name) != 0) {
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

sweep_public:
    d = opendir(store->public_dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5) {
            continue;
        }
        char id[ID_STR_LEN];
        size_t id_len = len - 5;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!hold_valid_id(id)) {
            continue;
        }
        char json_path[HOLD_PATH_MAX], public_path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0 ||
            hold_checked_snprintf(public_path, sizeof(public_path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        /* A public index entry whose record is gone is an orphan; ps must not resurrect it. */
        if (access(json_path, F_OK) != 0) {
            unlink(public_path);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);
    return 0;
}

int hold_cmd_purge_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *target_token,
                            bool all,
                            bool force) {
    if (!target_token || strcmp(target_token, "all") == 0) {
        /* A no-target sweep only clears already-ended calls and silently skips
         * saved ones; --force adds nothing here (it never mass-ends live calls). */
        const struct hold_store *store = inv->euid_root ? system_store : user_store;
        int removed = 0;
        bool include_stale = all || force || (target_token && strcmp(target_token, "all") == 0);
        int rc = cmd_prune_store_all(store, include_stale, &removed);
        if (rc == 0) {
            if (removed > 0) {
                hold_sig_note(inv, "hold: purged %d past call%s\n", removed, removed == 1 ? "" : "s");
            } else {
                hold_sig_note(inv, "hold: nothing to purge\n");
            }
        }
        return rc;
    }
    /* Resolve the target regardless of run state (inspect intent) so protection
     * and --force removal apply uniformly to live and ended calls. */
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "inspect", target_token, all, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        return hold_report_not_found(target_token);
    }
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].requires_root) {
            rc = hold_report_requires_root(targets[i].id);
            free(targets);
            return rc;
        }
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    /* Saved calls are protected: a targeted purge without --force refuses and
     * echoes the exact command the user meant, ready to copy. */
    if (!force) {
        for (int i = 0; i < ntargets; i++) {
            struct hold_run_record r;
            char rp[HOLD_PATH_MAX];
            if (hold_load_record_by_id(targets[i].store.record_dir, targets[i].id, &r, rp, sizeof(rp)) != 0) {
                continue;
            }
            bool saved = r.saved;
            char display_id[ID_DISPLAY_HEX_LEN + 1];
            hold_run_id_display(r.id, display_id);
            const char *label = r.has_name ? r.name : display_id;
            if (saved) {
                fprintf(stderr,
                        "hold: '%s' is saved \xe2\x80\x94 purging a saved call requires --force\n"
                        "  hold purge %s --force\n",
                        label, target_token);
                hold_free_run_record(&r);
                free(targets);
                return 2;
            }
            hold_free_run_record(&r);
        }
    }
    int worst = 0;
    int removed_count = 0;
    for (int i = 0; i < ntargets; i++) {
        /* --force ends a still-live call before removal; without it a live call
         * is refused below by hold_prune_one_run. */
        if (force) {
            struct hold_run_record r;
            char rp[HOLD_PATH_MAX];
            if (hold_load_record_by_id(targets[i].store.record_dir, targets[i].id, &r, rp, sizeof(rp)) == 0) {
                enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
                hold_free_run_record(&r);
                if (st == STATE_RUNNING) {
                    char *one[1] = { targets[i].id };
                    int stop_rc = hold_cmd_signal_action(inv, user_store, system_store, "stop", 1, one,
                                                         SIGTERM, true, false, false);
                    if (stop_rc != 0) {
                        if (stop_rc > worst) worst = stop_rc;
                        continue;
                    }
                }
            }
        }
        bool removed = false;
        rc = hold_prune_one_run(&targets[i].store, targets[i].id, have_boot ? boot : NULL, true, &removed);
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
            enum id_token_scope token_scope = hold_parse_id_token(target_token, &atom);
            bool target_looks_like_alias = (token_scope != ID_TOKEN_INVALID && atom &&
                                            hold_valid_alias(atom) && !hold_valid_id_prefix(atom));
            if (target_looks_like_alias) {
                hold_sig_note(inv, "hold: purged %d past call%s for '%s'\n",
                         removed_count, removed_count == 1 ? "" : "s", atom);
            } else {
                hold_sig_note(inv, "hold: purged %d past call%s\n", removed_count, removed_count == 1 ? "" : "s");
            }
        } else {
            hold_sig_note(inv, "hold: nothing to purge\n");
        }
    }
    free(targets);
    return worst;
}
