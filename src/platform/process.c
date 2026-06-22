#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/platform.h"
#include "sigmund/core.h"

#if defined(__APPLE__)
static int mac_kinfo_pid(pid_t pid, struct kinfo_proc *kp);
#endif
#if !defined(__APPLE__)
static int parse_pid_token(const char *tok, pid_t *out);
#endif
#if !defined(__APPLE__)
static int read_process_ids_state(pid_t pid, pid_t *pgid_out, pid_t *sid_out, char *state_out);
#endif

#if defined(__APPLE__)
static int mac_kinfo_pid(pid_t pid, struct kinfo_proc *kp) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t len = sizeof(*kp);
    memset(kp, 0, sizeof(*kp));
    if (sysctl(mib, 4, kp, &len, NULL, 0) != 0 || len == 0) {
        return -1;
    }
    return 0;
}
#endif

#if !defined(__APPLE__)
static int parse_pid_token(const char *tok, pid_t *out) {
    char *end = NULL;
    errno = 0;
    long x = strtol(tok, &end, 10);
    if (end == tok || *end != '\0' || errno != 0 || x <= 0) {
        return -1;
    }
    *out = (pid_t)x;
    return 0;
}
#endif

#if !defined(__APPLE__)
static int read_process_ids_state(pid_t pid, pid_t *pgid_out, pid_t *sid_out, char *state_out) {
    char path[128], buf[4096];
    if (sigmund_checked_snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) != 0) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t nr;
    do {
        nr = read(fd, buf, sizeof(buf) - 1);
    } while (nr < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (nr <= 0) {
        if (nr < 0) {
            errno = saved;
        } else {
            errno = EIO;
        }
        return -1;
    }
    buf[nr] = '\0';
    char *rp = strrchr(buf, ')');
    if (!rp) {
        errno = EINVAL;
        return -1;
    }
    char *fields = rp + 2;
    char *save = NULL;
    bool got_state = false, got_pgid = false, got_sid = false;
    pid_t pgid = 0, sid = 0;
    char state = 0;
    int idx = 0;
    for (char *tok = strtok_r(fields, " ", &save); tok; tok = strtok_r(NULL, " ", &save), idx++) {
        if (idx == 0) {
            state = tok[0];
            got_state = true;
        } else if (idx == 2) {
            if (parse_pid_token(tok, &pgid) != 0) {
                return -1;
            }
            got_pgid = true;
        } else if (idx == 3) {
            if (parse_pid_token(tok, &sid) != 0) {
                return -1;
            }
            got_sid = true;
            break;
        }
    }
    if ((state_out && !got_state) || (pgid_out && !got_pgid) || (sid_out && !got_sid)) {
        errno = EINVAL;
        return -1;
    }
    if (state_out) {
        *state_out = state;
    }
    if (pgid_out) {
        *pgid_out = pgid;
    }
    if (sid_out) {
        *sid_out = sid;
    }
    return 0;
}
#endif

enum group_liveness sigmund_group_session_liveness(pid_t pgid, pid_t sid) {
    if (pgid <= 1 || sid <= 0) {
        return GROUP_SCAN_ERROR;
    }
#if defined(__APPLE__)
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) {
        return GROUP_SCAN_ERROR;
    }
    struct kinfo_proc *procs = malloc(len ? len : sizeof(*procs));
    if (!procs) {
        return GROUP_SCAN_ERROR;
    }
    if (sysctl(mib, 3, procs, &len, NULL, 0) != 0) {
        free(procs);
        return GROUP_SCAN_ERROR;
    }
    bool any = false;
    bool live = false;
    size_t nprocs = len / sizeof(procs[0]);
    for (size_t i = 0; i < nprocs; i++) {
        pid_t pid = procs[i].kp_proc.p_pid;
        if (pid <= 0 || procs[i].kp_eproc.e_pgid != pgid) {
            continue;
        }
        pid_t proc_sid = getsid(pid);
        if (proc_sid != sid) {
            continue;
        }
        any = true;
        if (procs[i].kp_proc.p_stat != SZOMB) {
            live = true;
            break;
        }
    }
    free(procs);
    if (live) {
        return GROUP_LIVE;
    }
    return any ? GROUP_ZOMBIE_ONLY : GROUP_EMPTY;
#else
    DIR *d = opendir("/proc");
    if (!d) {
        return GROUP_SCAN_ERROR;
    }
    bool any = false;
    bool live = false;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) {
            continue;
        }
        char *pid_end = NULL;
        errno = 0;
        long pid_long = strtol(e->d_name, &pid_end, 10);
        if (pid_end == e->d_name || *pid_end != '\0' || errno != 0 || pid_long <= 0) {
            continue;
        }
        pid_t proc_pgid = 0, proc_sid = 0;
        char state = 0;
        if (read_process_ids_state((pid_t)pid_long, &proc_pgid, &proc_sid, &state) != 0) {
            continue;
        }
        if (proc_pgid != pgid || proc_sid != sid) {
            continue;
        }
        any = true;
        if (state != 'Z') {
            live = true;
            break;
        }
    }
    closedir(d);
    if (live) {
        return GROUP_LIVE;
    }
    return any ? GROUP_ZOMBIE_ONLY : GROUP_EMPTY;
#endif
}

int sigmund_count_session_escapees(pid_t sid, pid_t expected_pgid) {
#if defined(__APPLE__)
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) {
        return -1;
    }
    struct kinfo_proc *procs = malloc(len ? len : sizeof(*procs));
    if (!procs) {
        return -1;
    }
    if (sysctl(mib, 3, procs, &len, NULL, 0) != 0) {
        free(procs);
        return -1;
    }
    int count = 0;
    size_t nprocs = len / sizeof(procs[0]);
    for (size_t i = 0; i < nprocs; i++) {
        pid_t pid = procs[i].kp_proc.p_pid;
        if (pid <= 0) {
            continue;
        }
        pid_t proc_sid = getsid(pid);
        if (proc_sid == sid && procs[i].kp_eproc.e_pgid != expected_pgid) {
            count++;
        }
    }
    free(procs);
    return count;
#else
    DIR *d = opendir("/proc");
    if (!d) {
        return -1;
    }
    int count = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) {
            continue;
        }
        char *pid_end = NULL;
        errno = 0;
        long pid_long = strtol(e->d_name, &pid_end, 10);
        if (pid_end == e->d_name || *pid_end != '\0' || errno != 0 || pid_long <= 0) {
            continue;
        }
        pid_t proc_pgid = 0, proc_sid = 0;
        if (read_process_ids_state((pid_t)pid_long, &proc_pgid, &proc_sid, NULL) != 0) {
            continue;
        }
        if (proc_sid == sid && proc_pgid != expected_pgid) {
            count++;
        }
    }
    closedir(d);
    return count;
#endif
}

int sigmund_read_proc_stat_tokens(pid_t pid, char *state_out, uint64_t *starttime_out) {
#if defined(__APPLE__)
    struct kinfo_proc kp;
    if (mac_kinfo_pid(pid, &kp) != 0) {
        return -1;
    }
    if (state_out) {
        *state_out = kp.kp_proc.p_stat == SZOMB ? 'Z' : '?';
    }
    if (starttime_out) {
        struct timeval tv = kp.kp_proc.p_starttime;
        if (tv.tv_sec == 0 && tv.tv_usec == 0) {
            return -1;
        }
        *starttime_out = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
    }
    return 0;
#else
    char path[128], buf[4096];
    if (sigmund_checked_snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) != 0) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n;
    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (n <= 0) {
        if (n < 0) {
            errno = saved;
        } else {
            errno = EIO;
        }
        return -1;
    }
    buf[n] = '\0';
    char *rp = strrchr(buf, ')');
    if (!rp) {
        return -1;
    }
    char *p = rp + 2;
    int idx = 0;
    char *save = NULL;
    bool got_state = false;
    for (char *tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save), idx++) {
        if (idx == 0 && state_out) {
            *state_out = tok[0];
            got_state = true;
        }
        /* /proc/<pid>/stat starttime is field 22 (1-indexed overall),
         * which is index 19 after the trailing ')' where idx 0 starts at state. */
        if (idx == 19 && starttime_out) {
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(tok, &end, 10);
            if (end == tok || errno != 0) {
                return -1;
            }
            *starttime_out = parsed;
            return 0;
        }
    }
    return (state_out && got_state && !starttime_out) ? 0 : -1;
#endif
}

int sigmund_read_proc_exe(pid_t pid, uint64_t *dev, uint64_t *ino) {
    struct stat st;
#if defined(__APPLE__)
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
        return -1;
    }
#else
    char path[128];
    if (sigmund_checked_snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid) != 0) {
        return -1;
    }
#endif
    if (stat(path, &st) != 0) {
        return -1;
    }
    *dev = (uint64_t)st.st_dev;
    *ino = (uint64_t)st.st_ino;
    return 0;
}

bool sigmund_leader_present(pid_t pid) {
#if defined(__APPLE__)
    struct kinfo_proc kp;
    if (mac_kinfo_pid(pid, &kp) == 0) {
        return kp.kp_proc.p_stat != SZOMB;
    }
#else
    char path[128];
    struct stat st;
    if (sigmund_checked_snprintf(path, sizeof(path), "/proc/%ld", (long)pid) != 0) {
        return false;
    }
    if (stat(path, &st) == 0) {
        char stc = 0;
        if (sigmund_read_proc_stat_tokens(pid, &stc, NULL) == 0 && stc == 'Z') {
            return false;
        }
        return true;
    }
#endif
    if (kill(pid, 0) == 0 || errno == EPERM) {
        return true;
    }
    return false;
}

int sigmund_group_exists(pid_t pgid) {
    if (kill(-pgid, 0) == 0 || errno == EPERM) {
        return 1;
    }
    if (errno == ESRCH) {
        return 0;
    }
    return -1;
}
