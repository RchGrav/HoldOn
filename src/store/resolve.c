#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"

/* THE resolver: the store's one id/prefix resolution, exported so no layer
 * grows its own directory scan again. Exact id wins if its file exists;
 * otherwise the prefix must match exactly one <id>.json. */
int hold_resolve_record_id(const char *dir, const char *token, char *resolved, size_t n) {
    if (!dir || !*dir || !token || !*token || !resolved || n == 0) return -1;
    if (hold_valid_id(token)) {
        char exact[HOLD_PATH_MAX];
        if (hold_checked_snprintf(exact, sizeof(exact), "%s/%s.json", dir, token) == 0 &&
            access(exact, F_OK) == 0)
            return hold_checked_snprintf(resolved, n, "%s", token);
    }
    if (!hold_valid_id_prefix(token)) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char id[ID_STR_LEN];
        if (!hold_record_json_filename_id(e->d_name, id, sizeof(id))) continue;
        if (strncmp(id, token, strlen(token)) != 0) continue;
        matches++;
        if (hold_checked_snprintf(resolved, n, "%s", id) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return matches == 1 ? 0 : -1;
}

int hold_for_each_record(const char *record_dir, hold_record_fn fn, void *ctx) {
    DIR *d = opendir(record_dir);
    if (!d) return 0;
    int rc = 0;
    const struct dirent *e;
    while (rc == 0 && (e = readdir(d))) {
        char id[ID_STR_LEN], path[HOLD_PATH_MAX];
        if (!hold_record_json_filename_id(e->d_name, id, sizeof(id))) continue;
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", record_dir, e->d_name) != 0) continue;
        struct hold_run_record r;
        bool loaded = hold_load_record(path, &r) == 0;
        bool valid = loaded && hold_valid_record(&r) && strcmp(r.id, id) == 0;
        rc = fn(id, path, valid ? &r : NULL, ctx);
        if (loaded) hold_free_run_record(&r);
    }
    closedir(d);
    return rc;
}
