#include "hold/config.h"
#include "hold/types.h"
#include "hold/platform.h"
#include "hold/core.h"

static int realpath_copy(const char *path, char *out, size_t n);

static int realpath_copy(const char *path, char *out, size_t n) {
    char *resolved = realpath(path, NULL);
    if (!resolved) {
        return -1;
    }
    int rc = hold_checked_snprintf(out, n, "%s", resolved);
    free(resolved);
    return rc;
}

int hold_resolve_binary_path(const char *argv0, char *out, size_t n) {
    if (!argv0 || !*argv0) {
        errno = EINVAL;
        return -1;
    }
    if (strchr(argv0, '/')) {
        return realpath_copy(argv0, out, n);
    }
    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    const char *p = path;
    while (1) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        char dir[HOLD_PATH_MAX];
        if (len == 0) {
            if (hold_checked_snprintf(dir, sizeof(dir), ".") != 0) {
                return -1;
            }
        } else {
            if (len >= sizeof(dir)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(dir, p, len);
            dir[len] = '\0';
        }
        char candidate[HOLD_PATH_MAX], resolved[HOLD_PATH_MAX];
        if (hold_checked_snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv0) == 0 &&
            access(candidate, X_OK) == 0 && realpath_copy(candidate, resolved, sizeof(resolved)) == 0) {
            return hold_checked_snprintf(out, n, "%s", resolved);
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
    errno = ENOENT;
    return -1;
}

int hold_resolve_existing_path_from_cwd(const char *token, const char *cwd, char *out, size_t n) {
    if (!token || !*token || !out || n == 0) {
        errno = EINVAL;
        return -1;
    }
    if (token[0] == '/') {
        return realpath_copy(token, out, n);
    }
    if (cwd && *cwd) {
        char candidate[HOLD_PATH_MAX];
        if (hold_checked_snprintf(candidate, sizeof(candidate), "%s/%s", cwd, token) != 0) {
            return -1;
        }
        return realpath_copy(candidate, out, n);
    }
    return realpath_copy(token, out, n);
}

int hold_normalize_existing_argv_paths_from_cwd(char **argv, int argc, int first_arg, const char *cwd) {
    if (argc < 0 || (argc > 0 && !argv) || first_arg < 0) {
        errno = EINVAL;
        return -1;
    }
    for (int i = first_arg; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg || !*arg || arg[0] == '-') {
            continue;
        }
        char resolved[HOLD_PATH_MAX];
        if (hold_resolve_existing_path_from_cwd(arg, cwd, resolved, sizeof(resolved)) != 0) {
            continue;
        }
        if (!strcmp(arg, resolved)) {
            continue;
        }
        char *copy = strdup(resolved);
        if (!copy) {
            return -1;
        }
        free(argv[i]);
        argv[i] = copy;
    }
    return 0;
}

bool hold_path_is_within_dir(const char *path, const char *dir) {
    if (!path || !*path || !dir || !*dir) {
        return false;
    }
    char resolved_dir[HOLD_PATH_MAX];
    if (!realpath(dir, resolved_dir)) {
        return false;
    }
    size_t len = strlen(resolved_dir);
    while (len > 1 && resolved_dir[len - 1] == '/') {
        resolved_dir[--len] = '\0';
    }
    return strcmp(path, resolved_dir) == 0 ||
           (strncmp(path, resolved_dir, len) == 0 && path[len] == '/');
}

int hold_resolve_self_executable_path(const char *argv0, char *out, size_t n) {
    if (!out || n == 0) {
        errno = EINVAL;
        return -1;
    }
    out[0] = '\0';
#if defined(__linux__)
    char proc_path[HOLD_PATH_MAX];
    ssize_t got = readlink("/proc/self/exe", proc_path, sizeof(proc_path) - 1);
    if (got > 0) {
        proc_path[got] = '\0';
        return hold_checked_snprintf(out, n, "%s", proc_path);
    }
#endif
#if defined(__APPLE__)
    char mac_path[HOLD_PATH_MAX];
    uint32_t mac_size = (uint32_t)sizeof(mac_path);
    if (_NSGetExecutablePath(mac_path, &mac_size) == 0) {
        char resolved[HOLD_PATH_MAX];
        if (realpath(mac_path, resolved)) {
            return hold_checked_snprintf(out, n, "%s", resolved);
        }
    }
#endif
    if (argv0 && strchr(argv0, '/')) {
        char resolved[HOLD_PATH_MAX];
        if (realpath(argv0, resolved)) {
            return hold_checked_snprintf(out, n, "%s", resolved);
        }
    }
    errno = ENOENT;
    return -1;
}
