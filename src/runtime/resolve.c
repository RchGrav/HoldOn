#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"

static int resolve_run_id(const char *dir, const char *input, char *resolved, size_t n);
static int resolve_user_store_id(const struct hold_store *store, const char *id, char *resolved, size_t n);
static int resolve_system_private_id(const struct hold_store *store, const char *id, char *resolved, size_t n);
static int resolve_system_public_id(const struct hold_store *store, const char *id, char *resolved, size_t n);
static void fill_target(struct hold_resolved_target *out,
                        enum resolve_scope scope,
                        const struct hold_store *store,
                        const char *id,
                        bool requires_root);
static int append_alias_match(struct alias_match_list *list,
                              const struct hold_run_record *r,
                              enum run_state st,
                              const char *started_at);
static bool public_alias_visible(const struct hold_store *store, const char *alias);
static void report_alias_ambiguity(const char *command, const char *alias, const struct alias_match_list *list);
static int append_resolved_target(struct hold_resolved_target **targets,
                                  int *count,
                                  enum resolve_scope scope,
                                  const struct hold_store *store,
                                  const char *id,
                                  bool requires_root);
static int append_private_alias_targets(struct hold_resolved_target **targets,
                                        int *count,
                                        enum resolve_scope scope,
                                        const struct hold_store *store,
                                        const char *alias,
                                        const char *command,
                                        bool all);
static int collect_public_alias_matches(const struct hold_store *store,
                                        const char *alias,
                                        struct alias_match_list *list);
static int append_public_alias_denied_target(struct hold_resolved_target **targets,
                                             int *count,
                                             const struct hold_store *system_store,
                                             const char *alias,
                                             const char *command,
                                             bool all);
static int append_private_run_name_target(struct hold_resolved_target **targets,
                                          int *count,
                                          enum resolve_scope scope,
                                          const struct hold_store *store,
                                          const char *name,
                                          const char *command);
static int append_public_run_name_target(struct hold_resolved_target **targets,
                                         int *count,
                                         const struct hold_store *system_store,
                                         const char *name,
                                         const char *command);

static int resolve_run_id(const char *dir, const char *input, char *resolved, size_t n) {
    if (!input || !*input) {
        return -1;
    }
    if (hold_valid_id(input)) {
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", dir, input) == 0 && access(path, F_OK) == 0) {
            return hold_checked_snprintf(resolved, n, "%s", input);
        }
    }
    if (!hold_valid_id_prefix(input)) {
        return -1;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char id[ID_HEX_LEN + 1];
        if (!hold_record_json_filename_id(e->d_name, id, sizeof(id))) {
            continue;
        }
        if (strncmp(id, input, strlen(input)) == 0) {
            matches++;
            if (hold_checked_snprintf(resolved, n, "%s", id) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return (matches == 1) ? 0 : -1;
}

enum id_token_scope hold_parse_id_token(const char *token, const char **id_out) {
    if (!token || !*token) {
        return ID_TOKEN_INVALID;
    }
    if (strncmp(token, "user:", 5) == 0) {
        *id_out = token + 5;
        return **id_out ? ID_TOKEN_USER : ID_TOKEN_INVALID;
    }
    if (strncmp(token, "system:", 7) == 0) {
        *id_out = token + 7;
        return **id_out ? ID_TOKEN_SYSTEM : ID_TOKEN_INVALID;
    }
    *id_out = token;
    return ID_TOKEN_PLAIN;
}

int hold_ensure_run_recorded_under_alias(const struct hold_store *store, const char *id, const char *alias) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return -1;
    }
    if (!r.has_alias || strcmp(r.alias, alias) != 0) {
        hold_free_run_record(&r);
        errno = EPERM;
        return -1;
    }
    hold_free_run_record(&r);
    return 0;
}

static int resolve_user_store_id(const struct hold_store *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_private_id(const struct hold_store *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_public_id(const struct hold_store *store, const char *id, char *resolved, size_t n) {
    if (resolve_run_id(store->public_dir, id, resolved, n) != 0) {
        return -1;
    }
    struct hold_public_index pi;
    if (hold_load_public_index_by_id(store, resolved, &pi) != 0 || !pi.root_managed) {
        return -1;
    }
    return 0;
}

int hold_resolve_public_profile_token(const struct hold_store *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN]) {
    if (hold_valid_profile_hash(token)) {
        snprintf(hash, PROFILE_HASH_STR_LEN, "%s", token);
        return 1;
    }
    if (hold_valid_alias(token) && hold_alias_lookup_hash(store, token, hash) == 0) {
        return 1;
    }
    return 0;
}

static void fill_target(struct hold_resolved_target *out,
                        enum resolve_scope scope,
                        const struct hold_store *store,
                        const char *id,
                        bool requires_root) {
    memset(out, 0, sizeof(*out));
    out->scope = scope;
    out->store = *store;
    out->requires_root = requires_root;
    hold_checked_snprintf(out->id, sizeof(out->id), "%s", id);
}

void hold_free_alias_match_list(struct alias_match_list *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

bool hold_command_all_allowed(const char *command) {
    return command && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                       !strcmp(command, "prune"));
}

bool hold_record_matches_alias_intent(const char *command, const struct hold_run_record *r, enum run_state st) {
    if (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
        !strcmp(command, "tail")) {
        return st == STATE_RUNNING;
    }
    if (!strcmp(command, "console")) {
        return st == STATE_RUNNING && r->has_console;
    }
    if (!strcmp(command, "dump") || !strcmp(command, "view")) {
        return r->has_log;
    }
    if (!strcmp(command, "inspect")) {
        return true;
    }
    if (!strcmp(command, "prune")) {
        return st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE;
    }
    return false;
}

static bool hold_record_matches_run_name_intent(const char *command, const struct hold_run_record *r, enum run_state st) {
    if (!strcmp(command, "start-existing")) {
        return st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE || st == STATE_RUNNING;
    }
    return hold_record_matches_alias_intent(command, r, st);
}

static int append_alias_match(struct alias_match_list *list,
                              const struct hold_run_record *r,
                              enum run_state st,
                              const char *started_at) {
    struct alias_match *next = realloc(list->items, (list->count + 1) * sizeof(*list->items));
    if (!next) {
        return -1;
    }
    list->items = next;
    memset(&list->items[list->count], 0, sizeof(list->items[list->count]));
    if (hold_checked_snprintf(list->items[list->count].id, sizeof(list->items[list->count].id), "%s", r->id) != 0 ||
        hold_checked_snprintf(list->items[list->count].started_at,
                         sizeof(list->items[list->count].started_at),
                         "%s",
                         started_at && *started_at ? started_at : "-") != 0) {
        return -1;
    }
    list->items[list->count].state = st;
    list->count++;
    return 0;
}

int hold_collect_private_alias_matches(const struct hold_store *store,
                                         const char *alias,
                                         const char *command,
                                         struct alias_match_list *list) {
    memset(list, 0, sizeof(*list));
    if (!hold_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    list->alias_known = hold_alias_exists_in_store(store, alias);
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
            closedir(d);
            return -1;
        }
        struct hold_run_record r;
        if (hold_load_record(path, &r) != 0) {
            continue;
        }
        if (!hold_valid_record(&r) ||
            strcmp(r.id, file_id) != 0 || !r.has_alias || strcmp(r.alias, alias) != 0) {
            hold_free_run_record(&r);
            continue;
        }
        list->alias_known = true;
        enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
        if (!hold_record_matches_alias_intent(command, &r, st)) {
            hold_free_run_record(&r);
            continue;
        }
        char started_at[64];
        if (r.has_started_at && r.started_at[0]) {
            snprintf(started_at, sizeof(started_at), "%s", r.started_at);
        } else {
            hold_format_rfc3339_utc_from_ns(r.start_unix_ns, started_at, sizeof(started_at));
        }
        if (append_alias_match(list, &r, st, started_at) != 0) {
            hold_free_run_record(&r);
            closedir(d);
            return -1;
        }
        hold_free_run_record(&r);
    }
    closedir(d);
    return 0;
}

static bool public_alias_visible(const struct hold_store *store, const char *alias) {
    if (hold_alias_exists_in_store(store, alias)) {
        return true;
    }
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return false;
    }
    bool found = false;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= ID_STR_LEN) {
            continue;
        }
        char id[ID_STR_LEN];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!hold_valid_id(id)) {
            continue;
        }
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        struct hold_public_index pi;
        if (hold_load_public_index(path, &pi) == 0 && pi.has_alias && strcmp(pi.alias, alias) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void report_alias_ambiguity(const char *command, const char *alias, const struct alias_match_list *list) {
    fprintf(stderr,
            "hold: error: profile '%s' matches more than one %s candidate\n",
            alias,
            command ? command : "target");
    fprintf(stderr, "hold: candidates:\n");
    for (size_t i = 0; i < list->count; i++) {
        fprintf(stderr,
                "  %s %-8s %s\n",
                list->items[i].id,
                hold_state_str(list->items[i].state),
                list->items[i].started_at);
    }
    if (hold_command_all_allowed(command)) {
        fprintf(stderr, "hold: use --all to apply %s to every listed run\n", command);
    }
}

static int append_resolved_target(struct hold_resolved_target **targets,
                                  int *count,
                                  enum resolve_scope scope,
                                  const struct hold_store *store,
                                  const char *id,
                                  bool requires_root) {
    struct hold_resolved_target *next = realloc(*targets, (size_t)(*count + 1) * sizeof(**targets));
    if (!next) {
        return -1;
    }
    *targets = next;
    fill_target(&(*targets)[*count], scope, store, id, requires_root);
    (*count)++;
    return 0;
}

static int append_private_alias_targets(struct hold_resolved_target **targets,
                                        int *count,
                                        enum resolve_scope scope,
                                        const struct hold_store *store,
                                        const char *alias,
                                        const char *command,
                                        bool all) {
    struct alias_match_list matches;
    if (hold_collect_private_alias_matches(store, alias, command, &matches) != 0) {
        return -1;
    }
    if (!matches.alias_known) {
        hold_free_alias_match_list(&matches);
        return 0;
    }
    if (matches.count > 1 && (!all || !hold_command_all_allowed(command))) {
        report_alias_ambiguity(command, alias, &matches);
        hold_free_alias_match_list(&matches);
        return -2;
    }
    for (size_t i = 0; i < matches.count; i++) {
        if (append_resolved_target(targets, count, scope, store, matches.items[i].id, false) != 0) {
            hold_free_alias_match_list(&matches);
            return -1;
        }
    }
    hold_free_alias_match_list(&matches);
    return 1;
}

static int append_private_run_name_target(struct hold_resolved_target **targets,
                                          int *count,
                                          enum resolve_scope scope,
                                          const struct hold_store *store,
                                          const char *name,
                                          const char *command) {
    if (!hold_valid_alias(name)) return 0;
    DIR *d = opendir(store->record_dir);
    if (!d) return 0;
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    char matched[ID_STR_LEN] = {0};
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_STR_LEN];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) continue;
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) continue;
        struct hold_run_record r;
        if (hold_load_record(path, &r) != 0 || !hold_valid_record(&r) ||
            strcmp(r.id, file_id) != 0 || !r.has_name || strcmp(r.name, name) != 0) {
            hold_free_run_record(&r);
            continue;
        }
        enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
        if (hold_record_matches_run_name_intent(command, &r, st)) {
            matches++;
            snprintf(matched, sizeof(matched), "%s", r.id);
        }
        hold_free_run_record(&r);
    }
    closedir(d);
    if (matches == 0) return 0;
    if (matches > 1) {
        fprintf(stderr, "hold: error: run name '%s' is ambiguous\n", name);
        return -2;
    }
    return append_resolved_target(targets, count, scope, store, matched, false) == 0 ? 1 : -1;
}

static int collect_public_alias_matches(const struct hold_store *store,
                                        const char *alias,
                                        struct alias_match_list *list) {
    memset(list, 0, sizeof(*list));
    if (!hold_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    list->alias_known = hold_alias_exists_in_store(store, alias);
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
        char id[ID_STR_LEN];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!hold_valid_id(id)) {
            continue;
        }
        struct hold_public_index pi;
        if (hold_load_public_index_by_id(store, id, &pi) != 0 ||
            !pi.has_alias || strcmp(pi.alias, alias) != 0) {
            continue;
        }
        list->alias_known = true;
        struct hold_run_record pseudo;
        memset(&pseudo, 0, sizeof(pseudo));
        snprintf(pseudo.id, sizeof(pseudo.id), "%s", id);
        if (append_alias_match(list, &pseudo, STATE_UNKNOWN, pi.started_at[0] ? pi.started_at : "-") != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int append_public_run_name_target(struct hold_resolved_target **targets,
                                         int *count,
                                         const struct hold_store *system_store,
                                         const char *name,
                                         const char *command) {
    (void)command;
    if (!hold_valid_alias(name)) return 0;
    DIR *d = opendir(system_store->public_dir);
    if (!d) return 0;
    char matched[ID_STR_LEN] = {0};
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".json")) continue;
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= ID_STR_LEN) continue;
        char id[ID_STR_LEN];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!hold_valid_id(id)) continue;
        struct hold_public_index pi;
        if (hold_load_public_index_by_id(system_store, id, &pi) == 0 &&
            pi.has_name && strcmp(pi.name, name) == 0) {
            matches++;
            snprintf(matched, sizeof(matched), "%s", id);
        }
    }
    closedir(d);
    if (matches == 0) return 0;
    if (matches > 1) {
        fprintf(stderr, "hold: error: run name '%s' is ambiguous\n", name);
        return -2;
    }
    return append_resolved_target(targets, count, RESOLVE_SYSTEM_MANAGED, system_store, matched, true) == 0 ? 1 : -1;
}

/* Non-root resolution of a root-managed profile name. Delegated execution is
 * gone: a match is appended only to carry the requires_root flag so the command
 * reports permission denied. A known name with nothing running resolves to zero
 * targets (nothing to do); an unknown name falls through to "not found". */
static int append_public_alias_denied_target(struct hold_resolved_target **targets,
                                             int *count,
                                             const struct hold_store *system_store,
                                             const char *alias,
                                             const char *command,
                                             bool all) {
    if (!public_alias_visible(system_store, alias)) {
        return 0;
    }
    struct alias_match_list matches;
    if (collect_public_alias_matches(system_store, alias, &matches) != 0) {
        return -1;
    }
    if (matches.count == 0) {
        hold_free_alias_match_list(&matches);
        return 1;
    }
    if (matches.count > 1 && (!all || !hold_command_all_allowed(command))) {
        report_alias_ambiguity(command, alias, &matches);
        hold_free_alias_match_list(&matches);
        return -2;
    }
    int rc = append_resolved_target(targets, count, RESOLVE_SYSTEM_MANAGED, system_store,
                                    matches.items[0].id, true) == 0 ? 1 : -1;
    hold_free_alias_match_list(&matches);
    return rc;
}

int hold_resolve_target(const struct hold_invocation *inv,
                          const struct hold_store *current_user_store,
                          const struct hold_store *system_store,
                          const char *token,
                          struct hold_resolved_target *out) {
    memset(out, 0, sizeof(*out));
    out->scope = RESOLVE_NOT_FOUND;

    const char *id = NULL;
    enum id_token_scope token_scope = hold_parse_id_token(token, &id);
    if (token_scope == ID_TOKEN_INVALID || !hold_valid_target_atom(id)) {
        fprintf(stderr, "hold: error: invalid target '%s'\n", token ? token : "");
        out->scope = RESOLVE_ERROR;
        return -1;
    }

    if (inv->euid_root) {
        if (token_scope == ID_TOKEN_USER) {
            struct hold_store user_store;
            if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "hold: error: user:%s requires sudo provenance\n", id);
                out->scope = RESOLVE_ERROR;
                return -1;
            }
            char resolved[ID_STR_LEN];
            if (resolve_user_store_id(&user_store, id, resolved, sizeof(resolved)) == 0) {
                fill_target(out, RESOLVE_USER_LOCAL, &user_store, resolved, false);
                return 0;
            }
            return 0;
        }
        if (token_scope == ID_TOKEN_SYSTEM) {
            char resolved[ID_STR_LEN];
            if (resolve_system_private_id(system_store, id, resolved, sizeof(resolved)) == 0) {
                fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false);
                return 0;
            }
            return 0;
        }

        char root_resolved[ID_STR_LEN] = {0};
        char user_resolved[ID_STR_LEN] = {0};
        bool root_match = resolve_system_private_id(system_store, id, root_resolved, sizeof(root_resolved)) == 0;
        bool user_match = false;
        struct hold_store user_store;
        if (inv->have_sudo_user && hold_init_invoking_user_store(inv, &user_store) == 0) {
            user_match = resolve_user_store_id(&user_store, id, user_resolved, sizeof(user_resolved)) == 0;
        }
        if (root_match) {
            (void)user_match;
            fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, root_resolved, false);
            return 0;
        }
        if (user_match) {
            fill_target(out, RESOLVE_USER_LOCAL, &user_store, user_resolved, false);
            return 0;
        }
        return 0;
    }

    if (token_scope == ID_TOKEN_USER) {
        char resolved[ID_STR_LEN];
        if (resolve_user_store_id(current_user_store, id, resolved, sizeof(resolved)) == 0) {
            fill_target(out, RESOLVE_USER_LOCAL, current_user_store, resolved, false);
            return 0;
        }
        return 0;
    }
    if (token_scope == ID_TOKEN_SYSTEM) {
        char resolved[ID_STR_LEN];
        if (resolve_system_public_id(system_store, id, resolved, sizeof(resolved)) == 0) {
            fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, true);
            return 0;
        }
        return 0;
    }

    char user_resolved[ID_STR_LEN];
    if (resolve_user_store_id(current_user_store, id, user_resolved, sizeof(user_resolved)) == 0) {
        fill_target(out, RESOLVE_USER_LOCAL, current_user_store, user_resolved, false);
        return 0;
    }
    char system_resolved[ID_STR_LEN];
    if (resolve_system_public_id(system_store, id, system_resolved, sizeof(system_resolved)) == 0) {
        fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, system_resolved, true);
        return 0;
    }
    return 0;
}

int hold_report_not_found(const char *token) {
    fprintf(stderr, "hold: error: no run matches '%s'\n", token ? token : "");
    return 5;
}

int hold_report_requires_root(const char *token) {
    fprintf(stderr, "hold: error: '%s' is root-managed; operate on it as root\n", token ? token : "");
    return 3;
}

int hold_resolve_action_token(const struct hold_invocation *inv,
                                const struct hold_store *current_user_store,
                                const struct hold_store *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct hold_resolved_target **targets_out,
                                int *count_out) {
    *targets_out = NULL;
    *count_out = 0;

    const char *atom = NULL;
    enum id_token_scope scope = hold_parse_id_token(token, &atom);
    if (scope == ID_TOKEN_INVALID || (!hold_valid_id_prefix(atom) && !hold_valid_alias(atom))) {
        fprintf(stderr, "hold: error: invalid target '%s'\n", token ? token : "");
        return 5;
    }

    if (hold_valid_alias(atom)) {
        int name_rc = 0;
        if (inv->euid_root) {
            if (scope == ID_TOKEN_USER) {
                struct hold_store user_store;
                if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                    fprintf(stderr, "hold: error: user:%s requires sudo provenance\n", atom);
                    return 5;
                }
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command);
            } else if (scope == ID_TOKEN_SYSTEM) {
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command);
            } else {
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command);
                if (name_rc == 0 && inv->have_sudo_user) {
                    struct hold_store user_store;
                    if (hold_init_invoking_user_store(inv, &user_store) == 0) {
                        name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command);
                    }
                }
            }
        } else {
            if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, atom, command);
            }
            if (name_rc == 0 && (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN)) {
                name_rc = append_public_run_name_target(targets_out, count_out, system_store, atom, command);
            }
        }
        if (name_rc == 1) return 0;
        if (name_rc == -2) return 6;
        if (name_rc < 0) return 3;
    }

    if (hold_valid_id_prefix(atom)) {
        char resolved[ID_STR_LEN];
        if (inv->euid_root) {
            if (scope == ID_TOKEN_USER) {
                struct hold_store user_store;
                if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                    fprintf(stderr, "hold: error: user:%s requires sudo provenance\n", atom);
                    return 5;
                }
                if (resolve_user_store_id(&user_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, resolved, false) == 0 ? 0 : 3;
                }
            } else if (scope == ID_TOKEN_SYSTEM) {
                if (resolve_system_private_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false) == 0 ? 0 : 3;
                }
            } else {
                if (resolve_system_private_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false) == 0 ? 0 : 3;
                }
                if (inv->have_sudo_user) {
                    struct hold_store user_store;
                    if (hold_init_invoking_user_store(inv, &user_store) == 0 &&
                        resolve_user_store_id(&user_store, atom, resolved, sizeof(resolved)) == 0) {
                        return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, resolved, false) == 0 ? 0 : 3;
                    }
                }
            }
        } else {
            if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
                if (resolve_user_store_id(current_user_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, resolved, false) == 0 ? 0 : 3;
                }
                if (scope == ID_TOKEN_USER) {
                    return hold_report_not_found(token);
                }
            }
            if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
                if (resolve_system_public_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, true) == 0 ? 0 : 3;
                }
            }
        }
    }

    if (hold_valid_alias(atom)) {
        int name_rc = 0;
        if (inv->euid_root) {
            if (scope == ID_TOKEN_USER) {
                struct hold_store user_store;
                if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                    fprintf(stderr, "hold: error: user:%s requires sudo provenance\n", atom);
                    return 5;
                }
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command);
            } else if (scope == ID_TOKEN_SYSTEM) {
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command);
            } else {
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command);
                if (name_rc == 0 && inv->have_sudo_user) {
                    struct hold_store user_store;
                    if (hold_init_invoking_user_store(inv, &user_store) == 0) {
                        name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command);
                    }
                }
            }
        } else {
            if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, atom, command);
                if (name_rc == 0 && scope == ID_TOKEN_USER) return hold_report_not_found(token);
            }
            if (name_rc == 0 && (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN)) {
                name_rc = append_public_run_name_target(targets_out, count_out, system_store, atom, command);
            }
        }
        if (name_rc == 1) return 0;
        if (name_rc == -2) return 6;
        if (name_rc < 0) return 3;
    }

    if (!hold_valid_alias(atom)) {
        return hold_report_not_found(token);
    }

    int rc = 0;
    if (inv->euid_root) {
        if (scope == ID_TOKEN_USER) {
            struct hold_store user_store;
            if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "hold: error: user:%s requires sudo provenance\n", atom);
                return 5;
            }
            rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command, all);
            if (rc == 1) return 0;
            if (rc == -2) return 6;
            return rc < 0 ? 3 : hold_report_not_found(token);
        }
        if (scope == ID_TOKEN_SYSTEM) {
            rc = append_private_alias_targets(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command, all);
            if (rc == 1) return 0;
            if (rc == -2) return 6;
            return rc < 0 ? 3 : hold_report_not_found(token);
        }

        rc = append_private_alias_targets(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
        if (inv->have_sudo_user) {
            struct hold_store user_store;
            if (hold_init_invoking_user_store(inv, &user_store) == 0) {
                rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command, all);
                if (rc == 1) return 0;
                if (rc == -2) return 6;
                if (rc < 0) return 3;
            }
        }
        return hold_report_not_found(token);
    }

    if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
        rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
        if (scope == ID_TOKEN_USER) {
            return hold_report_not_found(token);
        }
    }

    if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
        rc = append_public_alias_denied_target(targets_out, count_out, system_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
    }
    return hold_report_not_found(token);
}
