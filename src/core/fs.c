#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/core.h"

int sigmund_mkdir_p0700(const char *dir) {
    char path[SIGMUND_PATH_MAX];
    if (sigmund_checked_snprintf(path, sizeof(path), "%s", dir) != 0) {
        return -1;
    }

    size_t len = strlen(path);
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 1; i <= len; i++) {
        if (path[i] != '/' && path[i] != '\0') {
            continue;
        }
        char saved = path[i];
        path[i] = '\0';
        if (path[0] != '\0') {
            struct stat st;
            bool created = false;
            if (stat(path, &st) != 0) {
                if (mkdir(path, 0700) != 0 && errno != EEXIST) {
                    return -1;
                }
                created = true;
            } else if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
            if (created && chmod(path, 0700) != 0) {
                return -1;
            }
        }
        path[i] = saved;
    }
    return 0;
}

int sigmund_read_file_trim(const char *path, char *buf, size_t n) {
    if (n == 0) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t r;
    do {
        r = read(fd, buf, n - 1);
    } while (r < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (r < 0) {
        errno = saved;
        return -1;
    }
    buf[r] = '\0';
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r' || isspace((unsigned char)buf[r - 1]))) {
        buf[r - 1] = '\0';
        r--;
    }
    return 0;
}

bool sigmund_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int sigmund_mkdir_p_mode(const char *dir, mode_t mode) {
    char path[SIGMUND_PATH_MAX];
    if (sigmund_checked_snprintf(path, sizeof(path), "%s", dir) != 0) {
        return -1;
    }

    size_t len = strlen(path);
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 1; i <= len; i++) {
        if (path[i] != '/' && path[i] != '\0') {
            continue;
        }
        char saved = path[i];
        path[i] = '\0';
        if (path[0] != '\0') {
            struct stat st;
            if (stat(path, &st) != 0) {
                if (mkdir(path, mode) != 0 && errno != EEXIST) {
                    return -1;
                }
                if (chmod(path, mode) != 0) {
                    return -1;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
        }
        path[i] = saved;
    }
    return 0;
}

int sigmund_read_owned_file_no_symlink(const char *path, char **out) {
    *out = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > MAX_RECORD_BYTES) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    size_t sz = (size_t)st.st_size;
    char *j = malloc(sz + 1);
    if (!j) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    size_t off = 0;
    while (off < sz) {
        ssize_t nr = read(fd, j + off, sz - off);
        if (nr > 0) {
            off += (size_t)nr;
            continue;
        }
        if (nr == 0) {
            free(j);
            close(fd);
            errno = EIO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        int saved = errno;
        free(j);
        close(fd);
        errno = saved;
        return -1;
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

int sigmund_fsync_dir_path(const char *dir) {
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        return -1;
    }
    int rc = fsync(dfd);
    int saved = errno;
    close(dfd);
    errno = saved;
    return rc;
}

int sigmund_read_small_file(const char *path, char **out) {
    return sigmund_read_owned_file_no_symlink(path, out);
}
