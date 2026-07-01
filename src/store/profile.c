#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "hold/platform.h"

static void free_profiles(struct hold_profile *profiles, size_t count);
static bool profile_equal_recipe(const struct hold_profile *p,
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
                                 const char *log_destination);
static int parse_profile_object(const char *j, const char *hash, struct hold_profile *profile);
static int load_profiles(const struct hold_store *store, struct hold_profile **profiles_out, size_t *count_out);
static int write_profiles_atomic(const struct hold_store *store, const struct hold_profile *profiles, size_t count);

/*
 * This digest is a stable capability key. Do not add versions, environment,
 * cwd, uid, timestamps, or other context: existing aliases, profiles, and
 * sudoers grants are keyed to exactly this binary-path + argv framing.
 */
void hold_profile_hash_for_argv(const char *binary_path, int argc, char **argv, char out[PROFILE_HASH_STR_LEN]) {
    struct sha256_ctx ctx;
    unsigned char digest[32];
    hold_sha256_init(&ctx);
    hold_sha256_update_nul_field(&ctx, "hold-profile");
    hold_sha256_update_nul_field(&ctx, binary_path);
    char count[32];
    snprintf(count, sizeof(count), "%d", argc);
    hold_sha256_update_nul_field(&ctx, count);
    for (int i = 0; i < argc; i++) {
        char idx[32];
        snprintf(idx, sizeof(idx), "%d", i);
        hold_sha256_update_nul_field(&ctx, idx);
        hold_sha256_update_nul_field(&ctx, argv[i]);
    }
    hold_sha256_final(&ctx, digest);
    hold_hex_encode(digest, sizeof(digest), out, PROFILE_HASH_STR_LEN);
}

static void write_json_argv_tail(FILE *f, int argc, char **argv) {
    fputc('[', f);
    for (int i = 1; i < argc; i++) {
        if (i > 1) fputs(", ", f);
        fputc('"', f);
        hold_json_escape(f, argv && argv[i] ? argv[i] : "");
        fputc('"', f);
    }
    fputc(']', f);
}

void hold_free_profile(struct hold_profile *p) {
    if (!p) {
        return;
    }
    hold_free_argv_alloc(p->recipe.argv, p->recipe.argc);
    hold_free_argv_alloc(p->recipe.env, p->recipe.envc);
    hold_free_argv_alloc(p->recipe.ports, p->recipe.portc);
    hold_free_argv_alloc(p->recipe.volumes, p->recipe.volumec);
    hold_free_argv_alloc(p->recipe.cap_add, p->recipe.cap_addc);
    hold_free_argv_alloc(p->recipe.cap_drop, p->recipe.cap_dropc);
    memset(p, 0, sizeof(*p));
}

void hold_write_profile_recipe_json_members(FILE *f,
                                            const struct hold_profile *profile,
                                            const char *indent,
                                            const char *bin_key,
                                            const char *argv_key,
                                            bool mode_object) {
    const char *pre = indent ? indent : "";
    const char *bin = bin_key ? bin_key : "bin";
    const char *argv_name = argv_key ? argv_key : "args";
    fprintf(f, "%s\"%s\": \"", pre, bin);
    hold_json_escape(f, profile && profile->recipe.binary_path[0] ? profile->recipe.binary_path : "");
    fprintf(f, "\", \"%s\": ", argv_name);
    if (strcmp(argv_name, "args") == 0 && profile && profile->recipe.argc > 0) {
        write_json_argv_tail(f, profile->recipe.argc, profile->recipe.argv);
    } else {
        hold_write_json_argv(f, profile ? profile->recipe.argc : 0, profile ? profile->recipe.argv : NULL);
    }
    if (profile && profile->recipe.envc > 0 && profile->recipe.env) {
        fprintf(f, ", \"env\": ");
        hold_write_json_argv(f, profile->recipe.envc, profile->recipe.env);
    }
    if (profile && profile->recipe.portc > 0 && profile->recipe.ports) {
        fprintf(f, ", \"ports\": ");
        hold_write_json_argv(f, profile->recipe.portc, profile->recipe.ports);
    }
    if (profile && profile->recipe.volumec > 0 && profile->recipe.volumes) {
        fprintf(f, ", \"volumes\": ");
        hold_write_json_argv(f, profile->recipe.volumec, profile->recipe.volumes);
    }
    if (profile && profile->recipe.cap_addc > 0 && profile->recipe.cap_add) {
        fprintf(f, ", \"cap_add\": ");
        hold_write_json_argv(f, profile->recipe.cap_addc, profile->recipe.cap_add);
    }
    if (profile && profile->recipe.cap_dropc > 0 && profile->recipe.cap_drop) {
        fprintf(f, ", \"cap_drop\": ");
        hold_write_json_argv(f, profile->recipe.cap_dropc, profile->recipe.cap_drop);
    }
    if (profile && (profile->recipe.mode_interactive || profile->recipe.mode_tty || profile->recipe.mode_detach || profile->recipe.allow_multi)) {
        if (mode_object) {
            fprintf(f, ", \"mode\": {");
            bool wrote_mode = false;
            if (profile->recipe.mode_interactive) { fprintf(f, "\"interactive\": true"); wrote_mode = true; }
            if (profile->recipe.mode_tty) { fprintf(f, "%s\"tty\": true", wrote_mode ? ", " : ""); wrote_mode = true; }
            if (profile->recipe.mode_detach) { fprintf(f, "%s\"detach\": true", wrote_mode ? ", " : ""); wrote_mode = true; }
            if (profile->recipe.allow_multi) { fprintf(f, "%s\"multi\": true", wrote_mode ? ", " : ""); }
            fprintf(f, "}");
        } else {
            if (profile->recipe.mode_interactive) fprintf(f, ", \"interactive\": true");
            if (profile->recipe.mode_tty) fprintf(f, ", \"tty\": true");
            if (profile->recipe.mode_detach) fprintf(f, ", \"detach\": true");
            if (profile->recipe.allow_multi) fprintf(f, ", \"allow_multi\": true");
        }
    }
    if (profile && profile->recipe.has_restart_policy && profile->recipe.restart_policy[0]) {
        fprintf(f, ", \"restart\": \"");
        hold_json_escape(f, profile->recipe.restart_policy);
        fprintf(f, "\"");
    }
    if (profile && profile->recipe.has_restart_delay) {
        fprintf(f, ", \"restart_delay_seconds\": %d", profile->recipe.restart_delay_seconds);
    }
    if (profile && profile->recipe.has_log_destination && profile->recipe.log_destination[0]) {
        fprintf(f, ", \"log_destination\": \"");
        hold_json_escape(f, profile->recipe.log_destination);
        fprintf(f, "\"");
    }
}

static void free_profiles(struct hold_profile *profiles, size_t count) {
    if (!profiles) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        hold_free_profile(&profiles[i]);
    }
    free(profiles);
}

static bool string_array_equal(int ac, char **av, int bc, char **bv) {
    if (ac != bc) return false;
    for (int i = 0; i < ac; i++) {
        if (!av || !bv || !av[i] || !bv[i] || strcmp(av[i], bv[i]) != 0) {
            return false;
        }
    }
    return true;
}

static bool profile_equal_recipe(const struct hold_profile *p,
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
    if (strcmp(p->recipe.binary_path, binary_path) != 0 || !string_array_equal(p->recipe.argc, p->recipe.argv, argc, argv)) {
        return false;
    }
    const char *want_restart = restart_policy && *restart_policy && strcmp(restart_policy, "no") != 0 ? restart_policy : NULL;
    const char *have_restart = p->recipe.has_restart_policy ? p->recipe.restart_policy : NULL;
    const char *want_log = log_destination && *log_destination ? log_destination : NULL;
    const char *have_log = p->recipe.has_log_destination ? p->recipe.log_destination : NULL;
    return p->recipe.mode_interactive == mode_interactive &&
           p->recipe.mode_tty == mode_tty &&
           p->recipe.mode_detach == mode_detach &&
           p->recipe.allow_multi == allow_multi &&
           ((have_restart == NULL && want_restart == NULL) ||
            (have_restart && want_restart && strcmp(have_restart, want_restart) == 0)) &&
           ((want_restart == NULL && !p->recipe.has_restart_delay) ||
            (want_restart != NULL && p->recipe.restart_delay_seconds == restart_delay_seconds)) &&
           ((have_log == NULL && want_log == NULL) ||
            (have_log && want_log && strcmp(have_log, want_log) == 0)) &&
           string_array_equal(p->recipe.envc, p->recipe.env, envc, env) &&
           string_array_equal(p->recipe.portc, p->recipe.ports, portc, ports) &&
           string_array_equal(p->recipe.volumec, p->recipe.volumes, volumec, volumes) &&
           string_array_equal(p->recipe.cap_addc, p->recipe.cap_add, cap_addc, cap_add) &&
           string_array_equal(p->recipe.cap_dropc, p->recipe.cap_drop, cap_dropc, cap_drop);
}

static int prepend_binary_to_argv_if_needed(struct hold_profile *profile) {
    if (!profile || profile->recipe.binary_path[0] != '/' || profile->recipe.argc < 0) {
        errno = EINVAL;
        return -1;
    }
    if (profile->recipe.argc > 0 && profile->recipe.argv && profile->recipe.argv[0] && strcmp(profile->recipe.argv[0], profile->recipe.binary_path) == 0) {
        return 0;
    }
    char **next = calloc((size_t)profile->recipe.argc + 2, sizeof(char *));
    if (!next) return -1;
    next[0] = strdup(profile->recipe.binary_path);
    if (!next[0]) {
        free(next);
        return -1;
    }
    for (int i = 0; i < profile->recipe.argc; i++) {
        next[i + 1] = strdup(profile->recipe.argv && profile->recipe.argv[i] ? profile->recipe.argv[i] : "");
        if (!next[i + 1]) {
            hold_free_argv_alloc(next, i + 1);
            return -1;
        }
    }
    next[profile->recipe.argc + 1] = NULL;
    hold_free_argv_alloc(profile->recipe.argv, profile->recipe.argc);
    profile->recipe.argv = next;
    profile->recipe.argc += 1;
    return 0;
}

static void parse_profile_mode_fields(const char *j, struct hold_profile *profile) {
    (void)hold_json_get_bool(j, "interactive", &profile->recipe.mode_interactive);
    (void)hold_json_get_bool(j, "tty", &profile->recipe.mode_tty);
    (void)hold_json_get_bool(j, "detach", &profile->recipe.mode_detach);
    (void)hold_json_get_bool(j, "multi", &profile->recipe.allow_multi);
    const char *mode = NULL;
    if (hold_json_find_key(j, "mode", &mode) == 0 && mode && *mode == '{') {
        (void)hold_json_get_bool(mode, "interactive", &profile->recipe.mode_interactive);
        (void)hold_json_get_bool(mode, "tty", &profile->recipe.mode_tty);
        (void)hold_json_get_bool(mode, "detach", &profile->recipe.mode_detach);
        (void)hold_json_get_bool(mode, "multi", &profile->recipe.allow_multi);
        (void)hold_json_get_bool(mode, "allow_multi", &profile->recipe.allow_multi);
    }
    const char *config = NULL;
    if (hold_json_find_key(j, "Config", &config) == 0 && config && *config == '{') {
        bool b = false;
        if (hold_json_get_bool(config, "Tty", &b) == 0) profile->recipe.mode_tty = b;
        if (hold_json_get_bool(config, "OpenStdin", &b) == 0) profile->recipe.mode_interactive = b;
    }
}

int hold_parse_profile_recipe_json(const char *j, const char *expected_hash, struct hold_profile *profile) {
    memset(profile, 0, sizeof(*profile));
    if (expected_hash && !hold_valid_profile_hash(expected_hash)) {
        errno = EINVAL;
        return -1;
    }
    if (hold_json_get_str(j, "bin", profile->recipe.binary_path, sizeof(profile->recipe.binary_path)) != 0 &&
        hold_json_get_str(j, "binary_path", profile->recipe.binary_path, sizeof(profile->recipe.binary_path)) != 0 &&
        hold_json_get_str(j, "Path", profile->recipe.binary_path, sizeof(profile->recipe.binary_path)) != 0) {
        return -1;
    }
    if (profile->recipe.binary_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    bool command_args_are_tail = false;
    if (hold_json_get_string_array_key_allow_empty_alloc(j, "args", &profile->recipe.argv, &profile->recipe.argc) == 0 ||
        hold_json_get_string_array_key_allow_empty_alloc(j, "argv", &profile->recipe.argv, &profile->recipe.argc) == 0) {
        command_args_are_tail = true;
    } else if (hold_json_get_path_args_argv_alloc(j, &profile->recipe.argv, &profile->recipe.argc) != 0) {
        hold_free_profile(profile);
        return -1;
    }
    if (command_args_are_tail && prepend_binary_to_argv_if_needed(profile) != 0) {
        hold_free_profile(profile);
        return -1;
    }
    if (hold_json_get_env_alloc(j, &profile->recipe.env, &profile->recipe.envc) != 0) {
        const char *config = NULL;
        if (hold_json_find_key(j, "Config", &config) != 0 || !config || *config != '{' ||
            hold_json_get_string_array_key_allow_empty_alloc(config, "Env", &profile->recipe.env, &profile->recipe.envc) != 0) {
            profile->recipe.env = NULL;
            profile->recipe.envc = 0;
        }
    }
    if (hold_json_get_ports_alloc(j, &profile->recipe.ports, &profile->recipe.portc) != 0) {
        profile->recipe.ports = NULL;
        profile->recipe.portc = 0;
    }
    if (hold_json_get_volumes_alloc(j, &profile->recipe.volumes, &profile->recipe.volumec) != 0) {
        profile->recipe.volumes = NULL;
        profile->recipe.volumec = 0;
    }
    if (hold_json_get_string_array_key_alloc(j, "cap_add", &profile->recipe.cap_add, &profile->recipe.cap_addc) != 0 &&
        hold_json_get_string_array_key_alloc(j, "CapAdd", &profile->recipe.cap_add, &profile->recipe.cap_addc) != 0) {
        const char *config = NULL;
        if (hold_json_find_key(j, "Config", &config) != 0 || !config || *config != '{' ||
            hold_json_get_string_array_key_alloc(config, "CapAdd", &profile->recipe.cap_add, &profile->recipe.cap_addc) != 0) {
        profile->recipe.cap_add = NULL;
        profile->recipe.cap_addc = 0;
        }
    }
    if (hold_json_get_string_array_key_alloc(j, "cap_drop", &profile->recipe.cap_drop, &profile->recipe.cap_dropc) != 0 &&
        hold_json_get_string_array_key_alloc(j, "CapDrop", &profile->recipe.cap_drop, &profile->recipe.cap_dropc) != 0) {
        const char *config = NULL;
        if (hold_json_find_key(j, "Config", &config) != 0 || !config || *config != '{' ||
            hold_json_get_string_array_key_alloc(config, "CapDrop", &profile->recipe.cap_drop, &profile->recipe.cap_dropc) != 0) {
            profile->recipe.cap_drop = NULL;
            profile->recipe.cap_dropc = 0;
        }
    }
    if (hold_json_get_str(j, "log_destination", profile->recipe.log_destination, sizeof(profile->recipe.log_destination)) == 0 &&
        profile->recipe.log_destination[0]) {
        profile->recipe.has_log_destination = true;
    } else {
        const char *config = NULL;
        const char *log_config = NULL;
        if (hold_json_find_key(j, "Config", &config) == 0 && config && *config == '{' &&
            hold_json_find_key(config, "LogConfig", &log_config) == 0 && log_config && *log_config == '{' &&
            hold_json_get_str(log_config, "Type", profile->recipe.log_destination, sizeof(profile->recipe.log_destination)) == 0 &&
            profile->recipe.log_destination[0]) {
            profile->recipe.has_log_destination = true;
        }
    }
    if (profile->recipe.has_log_destination && strcmp(profile->recipe.log_destination, "local") == 0) {
        profile->recipe.log_destination[0] = '\0';
        profile->recipe.has_log_destination = false;
    }
    parse_profile_mode_fields(j, profile);
    if (hold_json_get_str(j, "restart", profile->recipe.restart_policy, sizeof(profile->recipe.restart_policy)) == 0 &&
        profile->recipe.restart_policy[0] && strcmp(profile->recipe.restart_policy, "no") != 0) {
        profile->recipe.has_restart_policy = true;
    }
    int64_t restart_delay_tmp = 0;
    if (hold_json_get_i64(j, "restart_delay_seconds", &restart_delay_tmp) == 0 && restart_delay_tmp >= 0 && restart_delay_tmp <= INT_MAX) {
        profile->recipe.restart_delay_seconds = (int)restart_delay_tmp;
        profile->recipe.has_restart_delay = profile->recipe.has_restart_policy;
    }
    hold_profile_hash_for_argv(profile->recipe.binary_path, profile->recipe.argc, profile->recipe.argv, profile->hash);
    if (expected_hash && strcmp(profile->hash, expected_hash) != 0) {
        hold_free_profile(profile);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int parse_profile_object(const char *j, const char *hash, struct hold_profile *profile) {
    return hold_parse_profile_recipe_json(j, hash, profile);
}

static int load_profiles(const struct hold_store *store, struct hold_profile **profiles_out, size_t *count_out) {
    *profiles_out = NULL;
    *count_out = 0;
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(store->profile_path, &j) != 0) {
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
    struct hold_profile *profiles = NULL;
    while (1) {
        p = hold_skip_ws(p);
        if (*p == '}') {
            break;
        }
        char hash[PROFILE_HASH_STR_LEN];
        if (hold_parse_json_string(p, hash, sizeof(hash), &p) != 0 || !hold_valid_profile_hash(hash)) {
            free(j);
            free_profiles(profiles, count);
            errno = EINVAL;
            return -1;
        }
        p = hold_skip_ws(p);
        if (*p != ':') {
            free(j);
            free_profiles(profiles, count);
            errno = EINVAL;
            return -1;
        }
        p = hold_skip_ws(p + 1);
        if (*p != '{') {
            free(j);
            free_profiles(profiles, count);
            errno = EINVAL;
            return -1;
        }
        if (count == cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            struct hold_profile *next = realloc(profiles, next_cap * sizeof(*profiles));
            if (!next) {
                free(j);
                free_profiles(profiles, count);
                return -1;
            }
            profiles = next;
            cap = next_cap;
        }
        if (parse_profile_object(p, hash, &profiles[count]) != 0) {
            free(j);
            free_profiles(profiles, count);
            return -1;
        }
        count++;
        if (hold_skip_json_value(&p) != 0) {
            free(j);
            free_profiles(profiles, count);
            return -1;
        }
        p = hold_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            break;
        }
        free(j);
        free_profiles(profiles, count);
        errno = EINVAL;
        return -1;
    }
    free(j);
    *profiles_out = profiles;
    *count_out = count;
    return 0;
}

static int write_profiles_atomic(const struct hold_store *store, const struct hold_profile *profiles, size_t count) {
    char dir[HOLD_PATH_MAX], tmp[HOLD_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", store->profile_path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    int fd = hold_open_unique_temp(dir, "profiles", 0600, tmp, sizeof(tmp));
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0600) != 0 ||
        (store->kind == STORE_SYSTEM_MANAGED && geteuid() == 0 && fchown(fd, 0, 0) != 0)) {
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
        hold_json_escape(f, profiles[i].hash);
        fprintf(f, "\": {");
        hold_write_profile_recipe_json_members(f, &profiles[i], "", "bin", "args", true);
        fprintf(f, "}%s\n", i + 1 == count ? "" : ",");
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
    if (rename(tmp, store->profile_path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    (void)hold_fsync_dir_path(dir);
    return 0;
}

int hold_write_profile_atomic(const struct hold_store *store,
                                const char *hash,
                                const char *binary_path,
                                int argc,
                                char **argv) {
    return hold_write_profile_atomic_env(store, hash, binary_path, argc, argv, 0, NULL);
}

int hold_write_profile_atomic_env(const struct hold_store *store,
                                    const char *hash,
                                    const char *binary_path,
                                    int argc,
                                    char **argv,
                                    int envc,
                                    char **env) {
    return hold_write_profile_atomic_full(store, hash, binary_path, argc, argv, envc, env, 0, NULL, 0, NULL, 0, NULL, 0, NULL, false, false, false, false, NULL, 0, NULL);
}

int hold_write_profile_atomic_full(const struct hold_store *store,
                                    const char *hash,
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
    if (!hold_valid_profile_hash(hash) || !binary_path || binary_path[0] != '/' || argc <= 0 || !argv ||
        envc < 0 || (envc > 0 && !env) ||
        portc < 0 || (portc > 0 && !ports) ||
        volumec < 0 || (volumec > 0 && !volumes) ||
        cap_addc < 0 || (cap_addc > 0 && !cap_add) ||
        cap_dropc < 0 || (cap_dropc > 0 && !cap_drop) ||
        restart_delay_seconds < 0) {
        errno = EINVAL;
        return -1;
    }
    char check_hash[PROFILE_HASH_STR_LEN];
    hold_profile_hash_for_argv(binary_path, argc, argv, check_hash);
    if (strcmp(check_hash, hash) != 0) {
        errno = EINVAL;
        return -1;
    }
    struct hold_profile *profiles = NULL;
    size_t count = 0;
    if (load_profiles(store, &profiles, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(profiles[i].hash, hash) == 0) {
            int rc = 0;
            if (!profile_equal_recipe(&profiles[i], binary_path, argc, argv, envc, env, portc, ports, volumec, volumes, cap_addc, cap_add, cap_dropc, cap_drop, mode_interactive, mode_tty, mode_detach, allow_multi, restart_policy, restart_delay_seconds, log_destination)) {
                errno = EEXIST;
                rc = -1;
            }
            free_profiles(profiles, count);
            return rc;
        }
    }
    struct hold_profile *next = realloc(profiles, (count + 1) * sizeof(*profiles));
    if (!next) {
        free_profiles(profiles, count);
        return -1;
    }
    profiles = next;
    memset(&profiles[count], 0, sizeof(profiles[count]));
    snprintf(profiles[count].hash, sizeof(profiles[count].hash), "%s", hash);
    if (hold_checked_snprintf(profiles[count].recipe.binary_path, sizeof(profiles[count].recipe.binary_path), "%s", binary_path) != 0 ||
        hold_copy_argv(&profiles[count].recipe.argv, argc, argv) != 0 ||
        (envc > 0 && hold_copy_argv(&profiles[count].recipe.env, envc, env) != 0) ||
        (portc > 0 && hold_copy_argv(&profiles[count].recipe.ports, portc, ports) != 0) ||
        (volumec > 0 && hold_copy_argv(&profiles[count].recipe.volumes, volumec, volumes) != 0) ||
        (cap_addc > 0 && hold_copy_argv(&profiles[count].recipe.cap_add, cap_addc, cap_add) != 0) ||
        (cap_dropc > 0 && hold_copy_argv(&profiles[count].recipe.cap_drop, cap_dropc, cap_drop) != 0)) {
        free_profiles(profiles, count + 1);
        return -1;
    }
    profiles[count].recipe.argc = argc;
    profiles[count].recipe.envc = envc;
    profiles[count].recipe.portc = portc;
    profiles[count].recipe.volumec = volumec;
    profiles[count].recipe.cap_addc = cap_addc;
    profiles[count].recipe.cap_dropc = cap_dropc;
    profiles[count].recipe.mode_interactive = mode_interactive;
    profiles[count].recipe.mode_tty = mode_tty;
    profiles[count].recipe.mode_detach = mode_detach;
    profiles[count].recipe.allow_multi = allow_multi;
    if (restart_policy && *restart_policy && strcmp(restart_policy, "no") != 0) {
        snprintf(profiles[count].recipe.restart_policy, sizeof(profiles[count].recipe.restart_policy), "%s", restart_policy);
        profiles[count].recipe.has_restart_policy = true;
    }
    profiles[count].recipe.restart_delay_seconds = profiles[count].recipe.has_restart_policy ? restart_delay_seconds : 0;
    profiles[count].recipe.has_restart_delay = profiles[count].recipe.has_restart_policy && restart_delay_seconds > 0;
    if (log_destination && *log_destination) {
        snprintf(profiles[count].recipe.log_destination, sizeof(profiles[count].recipe.log_destination), "%s", log_destination);
        profiles[count].recipe.has_log_destination = true;
    }
    count++;
    int rc = write_profiles_atomic(store, profiles, count);
    free_profiles(profiles, count);
    return rc;
}

int hold_load_profile_by_hash(const struct hold_store *store, const char *hash, struct hold_profile *profile) {
    memset(profile, 0, sizeof(*profile));
    if (!hold_valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(store->profile_path, &j) != 0) {
        return -1;
    }
    const char *v = NULL;
    if (hold_json_find_key(j, hash, &v) != 0 || parse_profile_object(v, hash, profile) != 0) {
        free(j);
        return -1;
    }
    free(j);
    return 0;
}

int hold_profile_exists_in_store(const struct hold_store *store, const char *hash) {
    struct hold_profile p;
    if (hold_load_profile_by_hash(store, hash, &p) != 0) {
        return -1;
    }
    hold_free_profile(&p);
    return 0;
}
