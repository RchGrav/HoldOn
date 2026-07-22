#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "store_internal.h"

/* The public projection: the deliberately narrower, world-readable view of a
 * root-managed call. Never carries argv, environment, or the owning user —
 * that split is a frozen invariant. Writer and reader live together so the
 * projection schema has one home. */

struct pub_emit_ctx {
    const struct hold_run_record *r;
    const char *ports;
};

static void emit_kv_long(FILE *f, bool *first, const char *key, long v) {
    hold_emit_kv(f, first, key);
    fprintf(f, "%ld", v);
}

static int emit_public_json(FILE *f, void *vctx) {
    const struct pub_emit_ctx *c = vctx;
    const struct hold_run_record *r = c->r;
    const char *started = r->has_started_at && r->started_at[0] ? r->started_at : "-";
    bool first = true;
    fprintf(f, "{\n");
    hold_emit_kv_str(f, &first, "id", r->id);
    hold_emit_kv(f, &first, "root_managed");
    fputs("true", f);
    if (r->has_name) hold_emit_kv_str(f, &first, "name", r->name);
    hold_emit_kv_str(f, &first, "state_hint", r->has_state && r->state[0] ? r->state : "unknown");
    hold_emit_kv_str(f, &first, "started_at", started);
    hold_emit_kv_str(f, &first, "created_at", r->has_created_at && r->created_at[0] ? r->created_at : started);
    /* Observed ports are root's projection of the live process group; an
     * absent field means "not observed yet", never "no ports". */
    if (c->ports && c->ports[0]) hold_emit_kv_str(f, &first, "observed_ports", c->ports);
    if (r->has_ended_at && r->ended_at[0]) hold_emit_kv_str(f, &first, "ended_at", r->ended_at);
    if (r->has_exit_code) {
        hold_emit_kv(f, &first, "exit_code");
        fprintf(f, "%d", r->exit_code);
    }
    emit_kv_long(f, &first, "pid", (long)r->pid);
    emit_kv_long(f, &first, "pgid", (long)r->pgid);
    emit_kv_long(f, &first, "sid", (long)r->sid);
    hold_emit_kv(f, &first, "running");
    fputs(r->has_state && strcmp(r->state, "running") == 0 ? "true" : "false", f);
    fprintf(f, "\n}\n");
    return 0;
}

int hold_write_public_index_atomic(const struct hold_store *store, const struct hold_run_record *r,
                                     const char *observed_ports_csv) {
    char name[ID_STR_LEN + 8];
    struct pub_emit_ctx ctx = { r, observed_ports_csv };
    if (hold_checked_snprintf(name, sizeof(name), "%s.json", r->id) != 0) return -1;
    return hold_atomic_write_json(store->public_dir, name, 0644, true, emit_public_json, &ctx, NULL, 0);
}

/* Tolerant reader: a public entry with holes still lists, per-field defaults
 * standing in for anything absent. */
int hold_load_public_index(const char *path, struct hold_public_index *pi) {
    memset(pi, 0, sizeof(*pi));
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(path, &j) != 0) return -1;
    if (hold_json_get_str(j, "id", pi->id, sizeof(pi->id)) != 0 || !hold_valid_id(pi->id)) {
        free(j);
        return -1;
    }
    if (hold_json_get_bool(j, "root_managed", &pi->root_managed) != 0) pi->root_managed = true;
    if (hold_json_get_str(j, "name", pi->name, sizeof(pi->name)) == 0 && hold_valid_alias(pi->name))
        pi->has_name = true;
    if (hold_json_get_str(j, "state_hint", pi->state_hint, sizeof(pi->state_hint)) != 0)
        snprintf(pi->state_hint, sizeof(pi->state_hint), "%s", "unknown");
    if (hold_json_get_str(j, "started_at", pi->started_at, sizeof(pi->started_at)) != 0)
        snprintf(pi->started_at, sizeof(pi->started_at), "%s", "-");
    if (hold_json_get_str(j, "created_at", pi->created_at, sizeof(pi->created_at)) != 0)
        snprintf(pi->created_at, sizeof(pi->created_at), "%s", pi->started_at[0] ? pi->started_at : "-");
    if (hold_json_get_str(j, "observed_ports", pi->observed_ports, sizeof(pi->observed_ports)) != 0)
        pi->observed_ports[0] = '\0';
    if (hold_json_get_str(j, "ended_at", pi->finished_at, sizeof(pi->finished_at)) == 0)
        pi->has_state = true;
    int64_t v = 0;
    if (hold_json_get_i64(j, "exit_code", &v) == 0) {
        pi->exit_code = (int)v;
        pi->has_exit_code = true;
        pi->has_state = true;
    }
    struct { const char *key; pid_t *out; } ids[] = {
        { "pid", &pi->pid }, { "pgid", &pi->pgid }, { "sid", &pi->sid }
    };
    for (size_t i = 0; i < 3; i++)
        if (hold_json_get_i64(j, ids[i].key, &v) == 0 && hold_record_checked_i64_to_pid(v, ids[i].out) == 0)
            pi->has_state = true;
    bool running = false;
    if (hold_json_get_bool(j, "running", &running) == 0) {
        pi->running = running;
        pi->has_state = true;
    } else {
        pi->running = strcmp(pi->state_hint, "running") == 0;
    }
    free(j);
    return 0;
}

int hold_load_public_index_by_id(const struct hold_store *store, const char *id, struct hold_public_index *pi) {
    if (!hold_valid_id(id)) return -1;
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) != 0) return -1;
    if (hold_load_public_index(path, pi) != 0 || strcmp(pi->id, id) != 0) return -1;
    return 0;
}
