#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "hold/platform.h"

static int write_aliases_atomic(const struct hold_store *store, const struct hold_alias *entries, size_t count);

void hold_free_aliases(struct hold_alias *entries, size_t count) {
    if (!entries) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        hold_free_argv_alloc(entries[i].recipe.argv, entries[i].recipe.argc);
        hold_free_argv_alloc(entries[i].recipe.env, entries[i].recipe.envc);
        hold_free_argv_alloc(entries[i].recipe.ports, entries[i].recipe.portc);
        hold_free_argv_alloc(entries[i].recipe.volumes, entries[i].recipe.volumec);
        hold_free_argv_alloc(entries[i].recipe.cap_add, entries[i].recipe.cap_addc);
        hold_free_argv_alloc(entries[i].recipe.cap_drop, entries[i].recipe.cap_dropc);
    }
    free(entries);
}

static int parse_alias_recipe_object(const char *j, struct hold_alias *entry) {
    struct hold_profile recipe;
    if (hold_parse_profile_recipe_json(j, NULL, &recipe) != 0) {
        return -1;
    }
    snprintf(entry->recipe.binary_path, sizeof(entry->recipe.binary_path), "%s", recipe.recipe.binary_path);
    entry->recipe.argc = recipe.recipe.argc;
    entry->recipe.argv = recipe.recipe.argv;
    entry->recipe.envc = recipe.recipe.envc;
    entry->recipe.env = recipe.recipe.env;
    entry->recipe.portc = recipe.recipe.portc;
    entry->recipe.ports = recipe.recipe.ports;
    entry->recipe.volumec = recipe.recipe.volumec;
    entry->recipe.volumes = recipe.recipe.volumes;
    entry->recipe.cap_addc = recipe.recipe.cap_addc;
    entry->recipe.cap_add = recipe.recipe.cap_add;
    entry->recipe.cap_dropc = recipe.recipe.cap_dropc;
    entry->recipe.cap_drop = recipe.recipe.cap_drop;
    entry->recipe.mode_interactive = recipe.recipe.mode_interactive;
    entry->recipe.mode_tty = recipe.recipe.mode_tty;
    entry->recipe.mode_detach = recipe.recipe.mode_detach;
    entry->recipe.allow_multi = recipe.recipe.allow_multi;
    snprintf(entry->recipe.restart_policy, sizeof(entry->recipe.restart_policy), "%s", recipe.recipe.restart_policy);
    entry->recipe.restart_delay_seconds = recipe.recipe.restart_delay_seconds;
    entry->recipe.has_restart_policy = recipe.recipe.has_restart_policy;
    entry->recipe.has_restart_delay = recipe.recipe.has_restart_delay;
    snprintf(entry->recipe.log_destination, sizeof(entry->recipe.log_destination), "%s", recipe.recipe.log_destination);
    entry->recipe.has_log_destination = recipe.recipe.has_log_destination;
    recipe.recipe.argv = NULL;
    recipe.recipe.argc = 0;
    recipe.recipe.env = NULL;
    recipe.recipe.envc = 0;
    recipe.recipe.ports = NULL;
    recipe.recipe.portc = 0;
    recipe.recipe.volumes = NULL;
    recipe.recipe.volumec = 0;
    recipe.recipe.cap_add = NULL;
    recipe.recipe.cap_addc = 0;
    recipe.recipe.cap_drop = NULL;
    recipe.recipe.cap_dropc = 0;
    entry->has_recipe = true;
    return 0;
}

int hold_load_aliases(const struct hold_store *store, struct hold_alias **entries_out, size_t *count_out) {
    *entries_out = NULL;
    *count_out = 0;
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(store->alias_path, &j) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    const char *p = hold_skip_ws(j);
    if (*p != '{') {
        free(j);
        errno = EINVAL;
        return -1;
    }
    p++;
    size_t cap = 0, count = 0;
    struct hold_alias *entries = NULL;
    while (1) {
        p = hold_skip_ws(p);
        if (*p == '}') {
            break;
        }
        char name[ALIAS_MAX_LEN + 1], hash[PROFILE_HASH_STR_LEN];
        if (hold_parse_json_string(p, name, sizeof(name), &p) != 0 || !hold_valid_alias(name)) {
            free(j);
            hold_free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = hold_skip_ws(p);
        if (*p != ':') {
            free(j);
            hold_free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = hold_skip_ws(p + 1);
        if (count == cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            struct hold_alias *next = realloc(entries, next_cap * sizeof(*entries));
            if (!next) {
                free(j);
                hold_free_aliases(entries, count);
                return -1;
            }
            entries = next;
            cap = next_cap;
        }
        memset(&entries[count], 0, sizeof(entries[count]));
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
        const char *value = p;
        if (*value == '"') {
            if (hold_parse_json_string(value, hash, sizeof(hash), NULL) != 0 || !hold_valid_profile_hash(hash)) {
                free(j);
                hold_free_aliases(entries, count);
                errno = EINVAL;
                return -1;
            }
            snprintf(entries[count].hash, sizeof(entries[count].hash), "%s", hash);
            entries[count].has_hash = true;
        } else if (*value == '{') {
            if (parse_alias_recipe_object(value, &entries[count]) != 0) {
                free(j);
                hold_free_aliases(entries, count + 1);
                return -1;
            }
        } else {
            free(j);
            hold_free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = value;
        if (hold_skip_json_value(&p) != 0) {
            free(j);
            hold_free_aliases(entries, count + 1);
            return -1;
        }
        count++;
        p = hold_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            break;
        }
        free(j);
        hold_free_aliases(entries, count);
        errno = EINVAL;
        return -1;
    }
    free(j);
    *entries_out = entries;
    *count_out = count;
    return 0;
}

static int write_aliases_atomic(const struct hold_store *store, const struct hold_alias *entries, size_t count) {
    const char *dir = store->kind == STORE_SYSTEM_MANAGED ? store->public_dir : store->base;
    char tmp[HOLD_PATH_MAX];
    mode_t mode = store->kind == STORE_SYSTEM_MANAGED ? 0644 : 0600;
    int fd = hold_open_unique_temp(dir, "aliases", mode, tmp, sizeof(tmp));
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, mode) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (store->kind == STORE_SYSTEM_MANAGED && geteuid() == 0 && fchown(fd, 0, 0) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    fprintf(f, "{\n");
    for (size_t i = 0; i < count; i++) {
        fprintf(f, "  \"");
        hold_json_escape(f, entries[i].name);
        if (store->kind == STORE_SYSTEM_MANAGED) {
            if (!entries[i].has_hash || !hold_valid_profile_hash(entries[i].hash)) {
                fclose(f);
                unlink(tmp);
                errno = EINVAL;
                return -1;
            }
            fprintf(f, "\": \"");
            hold_json_escape(f, entries[i].hash);
            fprintf(f, "\"%s\n", i + 1 == count ? "" : ",");
        } else {
            if (!entries[i].has_recipe || entries[i].recipe.binary_path[0] != '/' || entries[i].recipe.argc <= 0 || !entries[i].recipe.argv) {
                fclose(f);
                unlink(tmp);
                errno = EINVAL;
                return -1;
            }
            fprintf(f, "\": {");
            struct hold_profile view;
            memset(&view, 0, sizeof(view));
            snprintf(view.recipe.binary_path, sizeof(view.recipe.binary_path), "%s", entries[i].recipe.binary_path);
            view.recipe.argc = entries[i].recipe.argc;
            view.recipe.argv = entries[i].recipe.argv;
            view.recipe.envc = entries[i].recipe.envc;
            view.recipe.env = entries[i].recipe.env;
            view.recipe.portc = entries[i].recipe.portc;
            view.recipe.ports = entries[i].recipe.ports;
            view.recipe.volumec = entries[i].recipe.volumec;
            view.recipe.volumes = entries[i].recipe.volumes;
            view.recipe.cap_addc = entries[i].recipe.cap_addc;
            view.recipe.cap_add = entries[i].recipe.cap_add;
            view.recipe.cap_dropc = entries[i].recipe.cap_dropc;
            view.recipe.cap_drop = entries[i].recipe.cap_drop;
            view.recipe.mode_interactive = entries[i].recipe.mode_interactive;
            view.recipe.mode_tty = entries[i].recipe.mode_tty;
            view.recipe.mode_detach = entries[i].recipe.mode_detach;
            view.recipe.allow_multi = entries[i].recipe.allow_multi;
            snprintf(view.recipe.restart_policy, sizeof(view.recipe.restart_policy), "%s", entries[i].recipe.restart_policy);
            view.recipe.restart_delay_seconds = entries[i].recipe.restart_delay_seconds;
            view.recipe.has_restart_policy = entries[i].recipe.has_restart_policy;
            view.recipe.has_restart_delay = entries[i].recipe.has_restart_delay;
            snprintf(view.recipe.log_destination, sizeof(view.recipe.log_destination), "%s", entries[i].recipe.log_destination);
            view.recipe.has_log_destination = entries[i].recipe.has_log_destination;
            hold_write_profile_recipe_json_members(f, &view, "", "bin", "args", true);
            fprintf(f, "}%s\n", i + 1 == count ? "" : ",");
        }
    }
    fprintf(f, "}\n");
    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, store->alias_path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    (void)hold_fsync_dir_path(dir);
    return 0;
}

int hold_alias_lookup_hash(const struct hold_store *store, const char *alias, char hash[PROFILE_HASH_STR_LEN]) {
    if (!hold_valid_alias(alias)) {
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    int rc = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0 && entries[i].has_hash) {
            snprintf(hash, PROFILE_HASH_STR_LEN, "%s", entries[i].hash);
            rc = 0;
            break;
        }
    }
    hold_free_aliases(entries, count);
    return rc;
}

int hold_alias_upsert_hash(const struct hold_store *store, const char *alias, const char *hash) {
    if (!hold_valid_alias(alias) || !hold_valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            snprintf(entries[i].hash, sizeof(entries[i].hash), "%s", hash);
            entries[i].has_hash = true;
            int rc = write_aliases_atomic(store, entries, count);
            hold_free_aliases(entries, count);
            return rc;
        }
    }
    struct hold_alias *next = realloc(entries, (count + 1) * sizeof(*entries));
    if (!next) {
        hold_free_aliases(entries, count);
        return -1;
    }
    entries = next;
    memset(&entries[count], 0, sizeof(entries[count]));
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", alias);
    snprintf(entries[count].hash, sizeof(entries[count].hash), "%s", hash);
    entries[count].has_hash = true;
    count++;
    int rc = write_aliases_atomic(store, entries, count);
    hold_free_aliases(entries, count);
    return rc;
}

int hold_alias_lookup_recipe(const struct hold_store *store, const char *alias, struct hold_profile *recipe) {
    memset(recipe, 0, sizeof(*recipe));
    if (!hold_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    int rc = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) != 0) {
            continue;
        }
        if (entries[i].has_recipe) {
            if (hold_checked_snprintf(recipe->recipe.binary_path, sizeof(recipe->recipe.binary_path), "%s", entries[i].recipe.binary_path) == 0 &&
                hold_copy_argv(&recipe->recipe.argv, entries[i].recipe.argc, entries[i].recipe.argv) == 0 &&
                (entries[i].recipe.envc == 0 || hold_copy_argv(&recipe->recipe.env, entries[i].recipe.envc, entries[i].recipe.env) == 0) &&
                (entries[i].recipe.portc == 0 || hold_copy_argv(&recipe->recipe.ports, entries[i].recipe.portc, entries[i].recipe.ports) == 0) &&
                (entries[i].recipe.volumec == 0 || hold_copy_argv(&recipe->recipe.volumes, entries[i].recipe.volumec, entries[i].recipe.volumes) == 0) &&
                (entries[i].recipe.cap_addc == 0 || hold_copy_argv(&recipe->recipe.cap_add, entries[i].recipe.cap_addc, entries[i].recipe.cap_add) == 0) &&
                (entries[i].recipe.cap_dropc == 0 || hold_copy_argv(&recipe->recipe.cap_drop, entries[i].recipe.cap_dropc, entries[i].recipe.cap_drop) == 0)) {
                recipe->recipe.argc = entries[i].recipe.argc;
                recipe->recipe.envc = entries[i].recipe.envc;
                recipe->recipe.portc = entries[i].recipe.portc;
                recipe->recipe.volumec = entries[i].recipe.volumec;
                recipe->recipe.cap_addc = entries[i].recipe.cap_addc;
                recipe->recipe.cap_dropc = entries[i].recipe.cap_dropc;
                recipe->recipe.mode_interactive = entries[i].recipe.mode_interactive;
                recipe->recipe.mode_tty = entries[i].recipe.mode_tty;
                recipe->recipe.mode_detach = entries[i].recipe.mode_detach;
                recipe->recipe.allow_multi = entries[i].recipe.allow_multi;
                if (entries[i].recipe.has_restart_policy) {
                    snprintf(recipe->recipe.restart_policy, sizeof(recipe->recipe.restart_policy), "%s", entries[i].recipe.restart_policy);
                    recipe->recipe.has_restart_policy = true;
                }
                recipe->recipe.restart_delay_seconds = entries[i].recipe.restart_delay_seconds;
                recipe->recipe.has_restart_delay = entries[i].recipe.has_restart_delay;
                if (entries[i].recipe.has_log_destination && entries[i].recipe.log_destination[0]) {
                    snprintf(recipe->recipe.log_destination, sizeof(recipe->recipe.log_destination), "%s", entries[i].recipe.log_destination);
                    recipe->recipe.has_log_destination = true;
                }
                rc = 0;
            }
            break;
        }
        if (entries[i].has_hash && hold_load_profile_by_hash(store, entries[i].hash, recipe) == 0) {
            rc = 0;
            break;
        }
    }
    hold_free_aliases(entries, count);
    return rc;
}

int hold_alias_upsert_recipe(const struct hold_store *store,
                               const char *alias,
                               const char *binary_path,
                               int argc,
                               char **argv) {
    return hold_alias_upsert_recipe_env(store, alias, binary_path, argc, argv, 0, NULL);
}

int hold_alias_upsert_recipe_env(const struct hold_store *store,
                                   const char *alias,
                                   const char *binary_path,
                                   int argc,
                                   char **argv,
                                   int envc,
                                   char **env) {
    return hold_alias_upsert_recipe_full(store, alias, binary_path, argc, argv, envc, env, 0, NULL, 0, NULL, 0, NULL, 0, NULL, false, false, false, false, NULL, 0, NULL);
}

int hold_alias_upsert_recipe_full(const struct hold_store *store,
                                   const char *alias,
                                   const char *binary_path,
                                   int argc,
                                   char **argv,
                                   int envc,
                                   char **env,
                                   int portc,
                                   char **ports,
                                   int volumec,
                                   char **volumes,
                                   int cap_addc,
                                   char **cap_add,
                                   int cap_dropc,
                                   char **cap_drop,
                                   bool mode_interactive,
                                   bool mode_tty,
                                   bool mode_detach,
                                   bool allow_multi,
                                   const char *restart_policy,
                                   int restart_delay_seconds,
                                   const char *log_destination) {
    if (!hold_valid_alias(alias) || !binary_path || binary_path[0] != '/' || argc <= 0 || !argv ||
        envc < 0 || (envc > 0 && !env) ||
        portc < 0 || (portc > 0 && !ports) ||
        volumec < 0 || (volumec > 0 && !volumes) ||
        cap_addc < 0 || (cap_addc > 0 && !cap_add) ||
        cap_dropc < 0 || (cap_dropc > 0 && !cap_drop) ||
        restart_delay_seconds < 0 ||
        (log_destination && strcmp(log_destination, "syslog") != 0)) {
        errno = EINVAL;
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            hold_free_argv_alloc(entries[i].recipe.argv, entries[i].recipe.argc);
            hold_free_argv_alloc(entries[i].recipe.env, entries[i].recipe.envc);
            hold_free_argv_alloc(entries[i].recipe.ports, entries[i].recipe.portc);
            hold_free_argv_alloc(entries[i].recipe.volumes, entries[i].recipe.volumec);
            hold_free_argv_alloc(entries[i].recipe.cap_add, entries[i].recipe.cap_addc);
            hold_free_argv_alloc(entries[i].recipe.cap_drop, entries[i].recipe.cap_dropc);
            entries[i].recipe.argv = NULL;
            entries[i].recipe.argc = 0;
            entries[i].recipe.env = NULL;
            entries[i].recipe.envc = 0;
            entries[i].recipe.ports = NULL;
            entries[i].recipe.portc = 0;
            entries[i].recipe.volumes = NULL;
            entries[i].recipe.volumec = 0;
            entries[i].recipe.cap_add = NULL;
            entries[i].recipe.cap_addc = 0;
            entries[i].recipe.cap_drop = NULL;
            entries[i].recipe.cap_dropc = 0;
            if (hold_checked_snprintf(entries[i].recipe.binary_path, sizeof(entries[i].recipe.binary_path), "%s", binary_path) != 0 ||
                hold_copy_argv(&entries[i].recipe.argv, argc, argv) != 0 ||
                (envc > 0 && hold_copy_argv(&entries[i].recipe.env, envc, env) != 0) ||
                (portc > 0 && hold_copy_argv(&entries[i].recipe.ports, portc, ports) != 0) ||
                (volumec > 0 && hold_copy_argv(&entries[i].recipe.volumes, volumec, volumes) != 0) ||
                (cap_addc > 0 && hold_copy_argv(&entries[i].recipe.cap_add, cap_addc, cap_add) != 0) ||
                (cap_dropc > 0 && hold_copy_argv(&entries[i].recipe.cap_drop, cap_dropc, cap_drop) != 0)) {
                hold_free_aliases(entries, count);
                return -1;
            }
            entries[i].recipe.argc = argc;
            entries[i].recipe.envc = envc;
            entries[i].recipe.portc = portc;
            entries[i].recipe.volumec = volumec;
            entries[i].recipe.cap_addc = cap_addc;
            entries[i].recipe.cap_dropc = cap_dropc;
            entries[i].recipe.mode_interactive = mode_interactive;
            entries[i].recipe.mode_tty = mode_tty;
            entries[i].recipe.mode_detach = mode_detach;
            entries[i].recipe.allow_multi = allow_multi;
            entries[i].recipe.restart_policy[0] = '\0';
            entries[i].recipe.has_restart_policy = false;
            if (restart_policy && *restart_policy && strcmp(restart_policy, "no") != 0) {
                snprintf(entries[i].recipe.restart_policy, sizeof(entries[i].recipe.restart_policy), "%s", restart_policy);
                entries[i].recipe.has_restart_policy = true;
            }
            entries[i].recipe.restart_delay_seconds = entries[i].recipe.has_restart_policy ? restart_delay_seconds : 0;
            entries[i].recipe.has_restart_delay = entries[i].recipe.has_restart_policy && restart_delay_seconds > 0;
            entries[i].recipe.log_destination[0] = '\0';
            entries[i].recipe.has_log_destination = false;
            if (log_destination && *log_destination) {
                snprintf(entries[i].recipe.log_destination, sizeof(entries[i].recipe.log_destination), "%s", log_destination);
                entries[i].recipe.has_log_destination = true;
            }
            entries[i].has_recipe = true;
            entries[i].has_hash = false;
            entries[i].hash[0] = '\0';
            int rc = write_aliases_atomic(store, entries, count);
            hold_free_aliases(entries, count);
            return rc;
        }
    }
    struct hold_alias *next = realloc(entries, (count + 1) * sizeof(*entries));
    if (!next) {
        hold_free_aliases(entries, count);
        return -1;
    }
    entries = next;
    memset(&entries[count], 0, sizeof(entries[count]));
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", alias);
    if (hold_checked_snprintf(entries[count].recipe.binary_path, sizeof(entries[count].recipe.binary_path), "%s", binary_path) != 0 ||
        hold_copy_argv(&entries[count].recipe.argv, argc, argv) != 0 ||
        (envc > 0 && hold_copy_argv(&entries[count].recipe.env, envc, env) != 0) ||
        (portc > 0 && hold_copy_argv(&entries[count].recipe.ports, portc, ports) != 0) ||
        (volumec > 0 && hold_copy_argv(&entries[count].recipe.volumes, volumec, volumes) != 0) ||
        (cap_addc > 0 && hold_copy_argv(&entries[count].recipe.cap_add, cap_addc, cap_add) != 0) ||
        (cap_dropc > 0 && hold_copy_argv(&entries[count].recipe.cap_drop, cap_dropc, cap_drop) != 0)) {
        hold_free_aliases(entries, count + 1);
        return -1;
    }
    entries[count].recipe.argc = argc;
    entries[count].recipe.envc = envc;
    entries[count].recipe.portc = portc;
    entries[count].recipe.volumec = volumec;
    entries[count].recipe.cap_addc = cap_addc;
    entries[count].recipe.cap_dropc = cap_dropc;
    entries[count].recipe.mode_interactive = mode_interactive;
    entries[count].recipe.mode_tty = mode_tty;
    entries[count].recipe.mode_detach = mode_detach;
    entries[count].recipe.allow_multi = allow_multi;
    if (restart_policy && *restart_policy && strcmp(restart_policy, "no") != 0) {
        snprintf(entries[count].recipe.restart_policy, sizeof(entries[count].recipe.restart_policy), "%s", restart_policy);
        entries[count].recipe.has_restart_policy = true;
    }
    entries[count].recipe.restart_delay_seconds = entries[count].recipe.has_restart_policy ? restart_delay_seconds : 0;
    entries[count].recipe.has_restart_delay = entries[count].recipe.has_restart_policy && restart_delay_seconds > 0;
    if (log_destination && *log_destination) {
        snprintf(entries[count].recipe.log_destination, sizeof(entries[count].recipe.log_destination), "%s", log_destination);
        entries[count].recipe.has_log_destination = true;
    }
    entries[count].has_recipe = true;
    count++;
    int rc = write_aliases_atomic(store, entries, count);
    hold_free_aliases(entries, count);
    return rc;
}

int hold_alias_delete(const struct hold_store *store, const char *alias, bool *deleted) {
    if (deleted) {
        *deleted = false;
    }
    if (!hold_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) != 0) {
            continue;
        }
        hold_free_argv_alloc(entries[i].recipe.argv, entries[i].recipe.argc);
        hold_free_argv_alloc(entries[i].recipe.env, entries[i].recipe.envc);
        hold_free_argv_alloc(entries[i].recipe.ports, entries[i].recipe.portc);
        hold_free_argv_alloc(entries[i].recipe.volumes, entries[i].recipe.volumec);
        hold_free_argv_alloc(entries[i].recipe.cap_add, entries[i].recipe.cap_addc);
        hold_free_argv_alloc(entries[i].recipe.cap_drop, entries[i].recipe.cap_dropc);
        for (size_t j = i + 1; j < count; j++) {
            entries[j - 1] = entries[j];
        }
        memset(&entries[count - 1], 0, sizeof(entries[count - 1]));
        count--;
        int rc = write_aliases_atomic(store, entries, count);
        if (deleted && rc == 0) {
            *deleted = true;
        }
        hold_free_aliases(entries, count);
        return rc;
    }
    hold_free_aliases(entries, count);
    return 0;
}

int hold_alias_rename(const struct hold_store *store, const char *old_alias, const char *new_alias) {
    if (!hold_valid_alias(old_alias) || !hold_valid_alias(new_alias)) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(old_alias, new_alias) == 0) {
        return 0;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    ssize_t old_idx = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, new_alias) == 0) {
            hold_free_aliases(entries, count);
            errno = EEXIST;
            return -1;
        }
        if (strcmp(entries[i].name, old_alias) == 0) {
            old_idx = (ssize_t)i;
        }
    }
    if (old_idx < 0) {
        hold_free_aliases(entries, count);
        errno = ENOENT;
        return -1;
    }
    if (hold_checked_snprintf(entries[old_idx].name, sizeof(entries[old_idx].name), "%s", new_alias) != 0) {
        hold_free_aliases(entries, count);
        return -1;
    }
    int rc = write_aliases_atomic(store, entries, count);
    hold_free_aliases(entries, count);
    return rc;
}

int hold_parse_alias_cap_atom(const char *atom,
                                char alias[ALIAS_MAX_LEN + 1],
                                char hash[PROFILE_HASH_STR_LEN]) {
    const char *sep = atom ? strchr(atom, '@') : NULL;
    if (!sep || sep == atom) {
        return -1;
    }
    size_t alias_len = (size_t)(sep - atom);
    if (alias_len > ALIAS_MAX_LEN) {
        return -1;
    }
    char alias_tmp[ALIAS_MAX_LEN + 1];
    memcpy(alias_tmp, atom, alias_len);
    alias_tmp[alias_len] = '\0';
    if (!hold_valid_alias(alias_tmp) || !hold_valid_profile_hash(sep + 1)) {
        return -1;
    }
    snprintf(alias, ALIAS_MAX_LEN + 1, "%s", alias_tmp);
    snprintf(hash, PROFILE_HASH_STR_LEN, "%s", sep + 1);
    return 0;
}

int hold_verify_system_alias_cap(const struct hold_store *system_store,
                                   const char *alias,
                                   const char *hash) {
    char current[PROFILE_HASH_STR_LEN];
    struct hold_profile p;
    if (!hold_valid_alias(alias) || !hold_valid_profile_hash(hash) ||
        hold_alias_lookup_hash(system_store, alias, current) != 0 ||
        strcmp(current, hash) != 0 ||
        hold_load_profile_by_hash(system_store, hash, &p) != 0) {
        return -1;
    }
    hold_free_profile(&p);
    return 0;
}

bool hold_alias_exists_in_store(const struct hold_store *store, const char *alias) {
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (!hold_valid_alias(alias) || hold_load_aliases(store, &entries, &count) != 0) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            found = true;
            break;
        }
    }
    hold_free_aliases(entries, count);
    return found;
}
