#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"

/* Closes fd without clobbering the errno of the failure being reported.
 * Always returns -1 so error paths can `return close_keep_errno(fd);`. */
static int close_keep_errno(int fd) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

/* Opens dir refusing symlinks and re-verifies directoryness on the fd. */
static int open_dir_no_symlink(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) return close_keep_errno(fd);
    if (!S_ISDIR(st.st_mode)) {
        close(fd);
        errno = ENOTDIR;
        return -1;
    }
    return fd;
}

int hold_chmod_dir_no_symlink(const char *dir, mode_t mode) {
    int fd = open_dir_no_symlink(dir);
    if (fd < 0) return -1;
    if (fchmod(fd, mode) != 0) return close_keep_errno(fd);
    return close(fd);
}

int hold_chown_dir_no_symlink_if_root(const char *dir, uid_t uid, gid_t gid) {
    if (geteuid() != 0) return 0;
    int fd = open_dir_no_symlink(dir);
    if (fd < 0) return -1;
    if (fchown(fd, uid, gid) != 0) return close_keep_errno(fd);
    return close(fd);
}

bool hold_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int hold_mkdir_p_mode(const char *dir, mode_t mode) {
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s", dir) != 0) return -1;
    size_t len = strlen(path);
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 1; i <= len; i++) {
        if (path[i] != '/' && path[i] != '\0') continue;
        char saved = path[i];
        path[i] = '\0';
        if (path[0] != '\0') {
            struct stat st;
            bool created = false;
            if (lstat(path, &st) != 0) {
                if (mkdir(path, mode) != 0 && errno != EEXIST) return -1;
                if (lstat(path, &st) != 0) return -1;
                created = true;
            }
            if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
            /* chmod only dirs this call created; umask must not weaken them. */
            if (created && hold_chmod_dir_no_symlink(path, mode) != 0) return -1;
        }
        path[i] = saved;
    }
    return 0;
}

/* THE hardened reader: refuses symlinks, foreign owners (unless root-owned or
 * running as root) and oversize files, and caps before allocating. */
int hold_read_owned_file_no_symlink(const char *path, char **out) {
    *out = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) return close_keep_errno(fd);
    if (geteuid() != 0 && st.st_uid != 0 && st.st_uid != geteuid()) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > MAX_RECORD_BYTES) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    size_t sz = (size_t)st.st_size;
    char *j = malloc(sz + 1);
    if (!j) return close_keep_errno(fd);
    size_t off = 0;
    while (off < sz) {
        ssize_t nr = read(fd, j + off, sz - off);
        if (nr > 0) {
            off += (size_t)nr;
            continue;
        }
        if (nr < 0 && errno == EINTR) continue;
        if (nr == 0) errno = EIO;
        free(j);
        return close_keep_errno(fd);
    }
    j[sz] = '\0';
    if (close(fd) != 0) {
        int saved = errno;
        free(j);
        errno = saved;
        return -1;
    }
    *out = j;
    return 0;
}

/* Temp naming is an on-disk contract: the purge sweep recognizes orphans by
 * the `.<prefix>.<pid>.<nonce>.tmp` shape. O_EXCL carries uniqueness; the
 * clock+counter nonce only keeps the retry loop short. */
int hold_open_unique_temp(const char *dir, const char *prefix, mode_t mode, char *tmp, size_t tmp_n) {
    if (!dir || !*dir || !prefix || !*prefix || !tmp || tmp_n == 0 || strchr(prefix, '/')) {
        errno = EINVAL;
        return -1;
    }
    static unsigned counter;
    for (int i = 0; i < 64; i++) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
        unsigned long long nonce = (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
        if (hold_checked_snprintf(tmp, tmp_n, "%s/.%s.%ld.%llx%x.tmp", dir, prefix, (long)getpid(), nonce, counter++) != 0) {
            return -1;
        }
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

/* Append-open a log path refusing symlinks on the directory and the file,
 * then verify the result is a regular file. Creates 0600 when absent. */
int hold_open_append_no_symlink(const char *path) {
    const char *slash = path && *path ? strrchr(path, '/') : NULL;
    if (!slash || slash == path || !slash[1]) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)(slash - path) >= HOLD_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char dir[HOLD_PATH_MAX];
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = '\0';
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return -1;
    int fd = openat(dirfd, slash + 1, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    int saved = errno;
    close(dirfd);
    if (fd < 0) {
        errno = saved;
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
