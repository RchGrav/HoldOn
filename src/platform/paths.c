#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/platform.h"
#include "sigmund/core.h"

static int realpath_copy(const char *path, char *out, size_t n);

static int realpath_copy(const char *path, char *out, size_t n) {
    char *resolved = realpath(path, NULL);
    if (!resolved) {
        return -1;
    }
    int rc = sigmund_checked_snprintf(out, n, "%s", resolved);
    free(resolved);
    return rc;
}

int sigmund_resolve_binary_path(const char *argv0, char *out, size_t n) {
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
        char dir[SIGMUND_PATH_MAX];
        if (len == 0) {
            if (sigmund_checked_snprintf(dir, sizeof(dir), ".") != 0) {
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
        char candidate[SIGMUND_PATH_MAX], resolved[SIGMUND_PATH_MAX];
        if (sigmund_checked_snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv0) == 0 &&
            access(candidate, X_OK) == 0 && realpath_copy(candidate, resolved, sizeof(resolved)) == 0) {
            return sigmund_checked_snprintf(out, n, "%s", resolved);
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
    errno = ENOENT;
    return -1;
}

bool sigmund_path_is_within_dir(const char *path, const char *dir) {
    if (!path || !*path || !dir || !*dir) {
        return false;
    }
    char resolved_dir[SIGMUND_PATH_MAX];
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

int sigmund_resolve_self_executable_path(const char *argv0, char *out, size_t n) {
    if (!out || n == 0) {
        errno = EINVAL;
        return -1;
    }
    out[0] = '\0';
#if defined(__linux__)
    char proc_path[SIGMUND_PATH_MAX];
    ssize_t got = readlink("/proc/self/exe", proc_path, sizeof(proc_path) - 1);
    if (got > 0) {
        proc_path[got] = '\0';
        return sigmund_checked_snprintf(out, n, "%s", proc_path);
    }
#endif
#if defined(__APPLE__)
    char mac_path[SIGMUND_PATH_MAX];
    uint32_t mac_size = (uint32_t)sizeof(mac_path);
    if (_NSGetExecutablePath(mac_path, &mac_size) == 0) {
        char resolved[SIGMUND_PATH_MAX];
        if (realpath(mac_path, resolved)) {
            return sigmund_checked_snprintf(out, n, "%s", resolved);
        }
    }
#endif
    if (argv0 && strchr(argv0, '/')) {
        char resolved[SIGMUND_PATH_MAX];
        if (realpath(argv0, resolved)) {
            return sigmund_checked_snprintf(out, n, "%s", resolved);
        }
    }
    errno = ENOENT;
    return -1;
}
