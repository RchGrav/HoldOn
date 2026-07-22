#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"

/* Target resolution: the privilege/scope/intent matrix. The store owns raw
 * id/prefix scanning (hold_resolve_record_id); this file owns WHICH stores a
 * token may reach — root-vs-nonroot × plain/user:/system: — with alias
 * intent, sudo provenance for user:, and ambiguity rc 6. The candidate list
 * is built once and shared by the alias and id-prefix passes (alias wins). */

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

/* A public-projection id resolves only when the projection is root-managed. */
static int resolve_system_public_id(const struct hold_store *store, const char *id, char *resolved, size_t n) {
    if (hold_resolve_record_id(store->public_dir, id, resolved, n) != 0) {
        return -1;
    }
    struct hold_public_index pi;
    if (hold_load_public_index_by_id(store, resolved, &pi) != 0 || !pi.root_managed) {
        return -1;
    }
    return 0;
}

bool hold_record_matches_alias_intent(const char *command, const struct hold_run_record *r, enum run_state st) {
    if (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
        !strcmp(command, "tail")) {
        return st == STATE_RUNNING;
    }
    if (!strcmp(command, "console")) {
        return st == STATE_RUNNING && r->has_console;
    }
    if (!strcmp(command, "view")) {
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
    struct hold_resolved_target *out = &(*targets)[*count];
    memset(out, 0, sizeof(*out));
    out->scope = scope;
    out->store = *store;
    out->requires_root = requires_root;
    hold_checked_snprintf(out->id, sizeof(out->id), "%s", id);
    (*count)++;
    return 0;
}

struct name_walk {
    const char *name;
    const char *command;
    const char *boot_id;
    char matched[ID_STR_LEN];
    int matches;
};

static int name_match_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)id;
    (void)path;
    struct name_walk *w = vctx;
    if (!r || !r->has_name || strcmp(r->name, w->name) != 0) return 0;
    if (hold_record_matches_run_name_intent(w->command, r, hold_eval_state(r, w->boot_id))) {
        w->matches++;
        snprintf(w->matched, sizeof(w->matched), "%s", r->id);
    }
    return 0;
}

static int append_private_run_name_target(struct hold_resolved_target **targets,
                                          int *count,
                                          enum resolve_scope scope,
                                          const struct hold_store *store,
                                          const char *name,
                                          const char *command) {
    char boot[128];
    struct name_walk w = { name, command, hold_boot_id_or_null(boot), {0}, 0 };
    hold_for_each_record(store->record_dir, name_match_cb, &w);
    if (w.matches == 0) return 0;
    if (w.matches > 1) {
        fprintf(stderr, "hold: error: call name '%s' is ambiguous\n", name);
        return -2;
    }
    return append_resolved_target(targets, count, scope, store, w.matched, false) == 0 ? 1 : -1;
}

static int append_public_run_name_target(struct hold_resolved_target **targets,
                                         int *count,
                                         const struct hold_store *system_store,
                                         const char *name) {
    DIR *d = opendir(system_store->public_dir);
    if (!d) return 0;
    char matched[ID_STR_LEN] = {0};
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char id[ID_STR_LEN];
        if (!hold_record_json_filename_id(e->d_name, id, sizeof(id))) continue;
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
        fprintf(stderr, "hold: error: call name '%s' is ambiguous\n", name);
        return -2;
    }
    return append_resolved_target(targets, count, RESOLVE_SYSTEM_MANAGED, system_store, matched, true) == 0 ? 1 : -1;
}

int hold_report_not_found(const char *token) {
    fprintf(stderr, "hold: error: no call matches '%s'\n", token ? token : "");
    return 5;
}

int hold_report_requires_root(const char *token) {
    fprintf(stderr, "hold: error: '%s' is root-managed; operate on it as root\n", token ? token : "");
    return 3;
}

/* One candidate store the token's scope may reach, in resolution order. */
struct resolve_candidate {
    enum resolve_scope scope;
    struct hold_store store;
    bool public_scan; /* match via the public projection; hits require root */
};

/* The matrix. Root reads private stores only (system, plus the invoking
 * user's under sudo provenance — user: DEMANDS that provenance); non-root
 * reads its own store, then falls back to the redacted public projection.
 * Returns the candidate count, or -1 with the provenance error printed. */
static int build_scope_candidates(const struct hold_invocation *inv,
                                  const struct hold_store *current_user_store,
                                  const struct hold_store *system_store,
                                  enum id_token_scope scope,
                                  const char *atom,
                                  struct resolve_candidate cands[2]) {
    int n = 0;
    if (inv->euid_root) {
        if (scope == ID_TOKEN_USER) {
            struct hold_store user_store;
            if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "hold: error: user:%s requires sudo provenance\n", atom);
                return -1;
            }
            cands[n++] = (struct resolve_candidate){RESOLVE_USER_LOCAL, user_store, false};
            return n;
        }
        cands[n++] = (struct resolve_candidate){RESOLVE_SYSTEM_MANAGED, *system_store, false};
        if (scope == ID_TOKEN_PLAIN && inv->have_sudo_user) {
            struct hold_store user_store;
            if (hold_init_invoking_user_store(inv, &user_store) == 0) {
                cands[n++] = (struct resolve_candidate){RESOLVE_USER_LOCAL, user_store, false};
            }
        }
        return n;
    }
    if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
        cands[n++] = (struct resolve_candidate){RESOLVE_USER_LOCAL, *current_user_store, false};
    }
    if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
        cands[n++] = (struct resolve_candidate){RESOLVE_SYSTEM_MANAGED, *system_store, true};
    }
    return n;
}

int hold_resolve_action_token(const struct hold_invocation *inv,
                                const struct hold_store *current_user_store,
                                const struct hold_store *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct hold_resolved_target **targets_out,
                                int *count_out) {
    (void)all;
    *targets_out = NULL;
    *count_out = 0;

    const char *atom = NULL;
    enum id_token_scope scope = hold_parse_id_token(token, &atom);
    if (scope == ID_TOKEN_INVALID || (!hold_valid_id_prefix(atom) && !hold_valid_alias(atom))) {
        fprintf(stderr, "hold: error: invalid target '%s'\n", token ? token : "");
        return 5;
    }

    struct resolve_candidate cands[2];
    int ncands = build_scope_candidates(inv, current_user_store, system_store, scope, atom, cands);
    if (ncands < 0) return 5;

    if (hold_valid_alias(atom)) {
        for (int i = 0; i < ncands; i++) {
            int name_rc = cands[i].public_scan
                ? append_public_run_name_target(targets_out, count_out, system_store, atom)
                : append_private_run_name_target(targets_out, count_out, cands[i].scope, &cands[i].store, atom, command);
            if (name_rc == 1) return 0;
            if (name_rc == -2) return 6;
            if (name_rc < 0) return 3;
        }
    }

    if (hold_valid_id_prefix(atom)) {
        for (int i = 0; i < ncands; i++) {
            char resolved[ID_STR_LEN];
            bool hit = cands[i].public_scan
                ? resolve_system_public_id(system_store, atom, resolved, sizeof(resolved)) == 0
                : hold_resolve_record_id(cands[i].store.record_dir, atom, resolved, sizeof(resolved)) == 0;
            if (hit) {
                return append_resolved_target(targets_out, count_out, cands[i].scope, &cands[i].store,
                                              resolved, cands[i].public_scan) == 0 ? 0 : 3;
            }
        }
    }

    return hold_report_not_found(token);
}
