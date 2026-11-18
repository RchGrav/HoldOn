#include "hold/config.h"
#include "hold/core.h"
#include "hold/console.h"
#include "hold/platform.h"
#include "hold/console_internal.h"

/* The console over FIFOs, for environments that deny Unix-domain sockets
 * (some sandboxes refuse socket(AF_UNIX) with EPERM). The broker's endpoint
 * becomes a FIFO at the usual <id>.sock path; it only ever carries fixed-size
 * introductions, which are atomic (far below PIPE_BUF), so concurrent clients
 * never interleave. Each client makes a private directory beside it,
 * "<endpoint>.XXXXXX", holding an in/out FIFO pair, and introduces it:
 *
 *   kind (1: 'C' control, 'A' attach) | suffix (6) | client pid (10 digits)
 *
 * The broker opens the pair, removes the directory, and answers one byte on
 * "out": 'R' ready, 'B' busy (try again), 'D' denied. From then on the pair is
 * the connection: the same framed protocol as the socket, minus the control
 * magic (the kind already said it). The claimed pid is believed only if that
 * process holds the "in" FIFO open — the peer identity hold msg verifies. */

#define FIFO_INTRO_LEN 17

/* Open one end of a FIFO without blocking; it must be a private FIFO (no
 * group or other access). Its owner is reported for the caller to judge. */
static int pipe_end(const char *path, int mode, uid_t *owner) {
    int fd = open(path, mode | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) && (st.st_mode & 0077) == 0) {
        if (owner) *owner = st.st_uid;
        return fd;
    }
    close(fd);
    errno = EACCES;
    return -1;
}

struct exchange {
    char dir[HOLD_PATH_MAX];
    char input[HOLD_PATH_MAX];
    char output[HOLD_PATH_MAX];
};

static int exchange_paths(struct exchange *x) {
    if (hold_checked_snprintf(x->input, sizeof(x->input), "%s/in", x->dir) != 0) return -1;
    return hold_checked_snprintf(x->output, sizeof(x->output), "%s/out", x->dir);
}

static void remove_exchange(const struct exchange *x) {
    unlink(x->input);
    unlink(x->output);
    rmdir(x->dir);
}

static int blocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

int hold_make_fifo_listener(const char *path, int *writer) {
    unlink(path);
    if (mkfifo(path, 0600) != 0) return -1;
    int flags = O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    int fd = open(path, flags | O_RDONLY);
    if (fd >= 0) {
        /* Our own writer keeps the FIFO from reporting EOF between clients. */
        *writer = open(path, flags | O_WRONLY);
        if (*writer >= 0) return fd;
        close(fd);
    }
    int saved = errno;
    unlink(path);
    errno = saved;
    return -1;
}

int hold_open_channel(const char *path, char kind, int *output) {
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISFIFO(st.st_mode)) {
        *output = hold_connect_console_socket(path);
        return *output;
    }
    /* Mutual authentication, as with the socket: the endpoint must belong to
     * us — or, for root, to the call's owner, whoever that is. */
    uid_t me = geteuid(), owner = (uid_t)-1;
    int listener = pipe_end(path, O_WRONLY, &owner);
    if (listener < 0) return -1;
    if (owner != me && me != 0) {
        close(listener);
        errno = EACCES;
        return -1;
    }
    struct exchange x;
    memset(&x, 0, sizeof(x));
    int request = -1, response = -1, guard = -1, result = -1;
    bool created = false;
    if (hold_checked_snprintf(x.dir, sizeof(x.dir), "%s.XXXXXX", path) != 0 || !mkdtemp(x.dir)) goto done;
    created = true;
    if (exchange_paths(&x) != 0 || mkfifo(x.input, 0600) != 0 || mkfifo(x.output, 0600) != 0) goto done;
    /* Open both of our ends before the introduction. The temporary reader
     * keeps the request writer valid until the broker has its own reader. */
    /* Root driving another user's call: hand the channel to that user, or
     * the broker (running as them) could not open it. */
    if (me == 0 && owner != 0 &&
        (chown(x.dir, owner, (gid_t)-1) != 0 || chown(x.input, owner, (gid_t)-1) != 0 ||
         chown(x.output, owner, (gid_t)-1) != 0))
        goto done;
    guard = pipe_end(x.input, O_RDONLY, NULL);
    if (guard < 0) goto done;
    request = pipe_end(x.input, O_WRONLY, NULL);
    response = pipe_end(x.output, O_RDONLY, NULL);
    if (request < 0 || response < 0) goto done;
    char intro[48];
    if (snprintf(intro, sizeof(intro), "%c%.6s%010ld", kind, x.dir + strlen(x.dir) - 6, (long)getpid()) !=
        FIFO_INTRO_LEN) {
        errno = EOVERFLOW; /* a pid beyond 10 digits: not on any platform we build for */
        goto done;
    }
    /* The broker may exit between our open and write: surface EPIPE as the
     * framed writers do, not a signal. */
    struct sigaction old, ignore;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &old) != 0) goto done;
    ssize_t written;
    do {
        written = write(listener, intro, FIFO_INTRO_LEN);
    } while (written < 0 && errno == EINTR);
    int write_error = errno;
    sigaction(SIGPIPE, &old, NULL);
    if (written != FIFO_INTRO_LEN) {
        errno = write_error;
        goto done;
    }
    struct pollfd p = {.fd = response, .events = POLLIN};
    int ready;
    do {
        ready = poll(&p, 1, 5000);
    } while (ready < 0 && errno == EINTR);
    if (ready == 0) errno = ETIMEDOUT;
    if (ready <= 0) goto done;
    char ack = 0;
    if (read(response, &ack, 1) != 1 || ack != 'R') {
        errno = ack == 'B' ? EBUSY : ack == 'D' ? EACCES : ECONNREFUSED;
        goto done;
    }
    if (blocking(request) != 0 || blocking(response) != 0) goto done;
    *output = request;
    result = response;
done: {
        int saved = errno;
        if (guard >= 0) close(guard);
        if (result < 0 && request >= 0) close(request);
        if (result < 0 && response >= 0) close(response);
        if (created) remove_exchange(&x);
        close(listener);
        errno = saved;
    }
    return result;
}

int hold_fifo_accept(int listener, const char *path, char *kind, int *output, uid_t *peer_uid, pid_t *peer_pid) {
    char intro[FIFO_INTRO_LEN];
    if (read(listener, intro, sizeof(intro)) != (ssize_t)sizeof(intro)) return -1;
    if (intro[0] != 'C' && intro[0] != 'A') return -1;
    for (size_t i = 1; i < 7; i++)
        if (!isalnum((unsigned char)intro[i])) return -1;
    long pid = 0;
    for (size_t i = 7; i < sizeof(intro); i++) {
        if (!isdigit((unsigned char)intro[i])) return -1;
        pid = pid * 10 + (intro[i] - '0');
    }
    struct exchange x;
    if (hold_checked_snprintf(x.dir, sizeof(x.dir), "%s.%.6s", path, intro + 1) != 0 || exchange_paths(&x) != 0)
        return -1;
    uid_t in_owner = (uid_t)-1, out_owner = (uid_t)-2;
    int input = pipe_end(x.input, O_RDONLY, &in_owner);
    if (input < 0) return -1;
    *output = pipe_end(x.output, O_WRONLY, &out_owner);
    struct stat st;
    if (*output < 0 || in_owner != out_owner || fstat(input, &st) != 0) {
        if (*output >= 0) close(*output);
        close(input);
        return -1;
    }
    remove_exchange(&x);
    *kind = intro[0];
    *peer_uid = in_owner;
    /* Believe the claimed pid only if it holds our "in" FIFO open. */
    *peer_pid = hold_pid_has_file_open((pid_t)pid, (uint64_t)st.st_dev, (uint64_t)st.st_ino) ? (pid_t)pid : 0;
    return input;
}

void hold_remove_fifo_exchanges(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    if (!slash) return;
    char dir[HOLD_PATH_MAX];
    size_t dlen = (size_t)(slash - path), blen = strlen(slash + 1);
    if (dlen == 0 || dlen >= sizeof(dir)) return;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';
    DIR *d = opendir(dir);
    if (!d) return;
    const struct dirent *e;
    while ((e = readdir(d))) {
        /* "<endpoint>.XXXXXX": exactly the mkdtemp shape, nothing else. */
        if (strncmp(e->d_name, slash + 1, blen) != 0 || e->d_name[blen] != '.' || strlen(e->d_name) != blen + 7)
            continue;
        struct exchange x;
        if (hold_checked_snprintf(x.dir, sizeof(x.dir), "%s/%s", dir, e->d_name) == 0 && exchange_paths(&x) == 0)
            remove_exchange(&x);
    }
    closedir(d);
}
