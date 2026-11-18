#include "hold/config.h"
#include "hold/core.h"
#include "hold/console_internal.h"

/* The listener only receives seven-byte introductions: a channel kind and
 * the suffix of a private mkdtemp directory beside the listener. Each client
 * has its own data pipes, so large payloads never share a writer stream. */
static int pipe_end(const char *path, int mode, uid_t owner) {
    int fd = open(path, mode | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) && st.st_uid == owner &&
        (st.st_mode & 0077) == 0) return fd;
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
    if (hold_checked_snprintf(x->input, sizeof(x->input), "%s/in", x->dir) < 0) return -1;
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

int hold_open_channel(const char *path, char kind, int *output) {
    struct stat st;
    if (lstat(path, &st) < 0 || !S_ISFIFO(st.st_mode)) {
        *output = hold_connect_console_socket(path);
        return *output;
    }
    int listener = pipe_end(path, O_WRONLY, geteuid());
    if (listener < 0) return -1;
    struct exchange x = {0};
    int request = -1, response = -1, guard = -1, result = -1;
    bool created = false;
    if (hold_checked_snprintf(x.dir, sizeof(x.dir), "%s.XXXXXX", path) < 0 ||
        !mkdtemp(x.dir)) goto done;
    created = true;
    if (exchange_paths(&x) < 0 || mkfifo(x.input, 0600) < 0 ||
        mkfifo(x.output, 0600) < 0) goto done;
    /* Open both client ends before introduction. The temporary reader keeps
     * the request writer open until the broker has its own reader. */
    guard = pipe_end(x.input, O_RDONLY, geteuid());
    if (guard < 0) goto done;
    request = pipe_end(x.input, O_WRONLY, geteuid());
    response = pipe_end(x.output, O_RDONLY, geteuid());
    if (request < 0 || response < 0) goto done;
    char intro[7] = {kind};
    memcpy(intro + 1, x.dir + strlen(x.dir) - 6, 6);
    /* The broker may exit between open and write. Surface EPIPE to
     * the caller just like the framed request writer does. */
    struct sigaction old, ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &old) < 0) goto done;
    ssize_t written;
    do { written = write(listener, intro, sizeof(intro)); }
    while (written < 0 && errno == EINTR);
    int write_error = errno;
    sigaction(SIGPIPE, &old, NULL);
    if (written != sizeof(intro)) {
        errno = write_error;
        goto done;
    }
    struct pollfd p = {.fd = response, .events = POLLIN};
    int ready;
    do { ready = poll(&p, 1, 5000); } while (ready < 0 && errno == EINTR);
    if (ready == 0) errno = ETIMEDOUT;
    if (ready <= 0) goto done;
    char ack = 0;
    if (read(response, &ack, 1) != 1 || ack != 'R') {
        errno = ack == 'B' ? EBUSY : ECONNREFUSED;
        goto done;
    }
    if (blocking(request) < 0 || blocking(response) < 0) goto done;
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

int hold_fifo_accept(int listener, const char *path, char *kind, int *output) {
    char intro[7];
    if (read(listener, intro, sizeof(intro)) != sizeof(intro)) return -1;
    if (intro[0] != 'C' && intro[0] != 'A') return -1;
    for (size_t i = 1; i < sizeof(intro); i++)
        if (!isalnum((unsigned char)intro[i])) return -1;
    struct exchange x;
    if (hold_checked_snprintf(x.dir, sizeof(x.dir), "%s.%.*s", path, 6, intro + 1) < 0 ||
        exchange_paths(&x) < 0) return -1;
    int input = pipe_end(x.input, O_RDONLY, geteuid());
    if (input < 0) return -1;
    *output = pipe_end(x.output, O_WRONLY, geteuid());
    if (*output < 0) {
        close(input);
        return -1;
    }
    remove_exchange(&x);
    *kind = intro[0];
    return input;
}
