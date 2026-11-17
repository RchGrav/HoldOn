#include "hold/config.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/console_internal.h"

/* Client side of the broker's control protocol (see console_internal.h):
 * magic, one be32-length request, one be32-length reply. Blocking; the
 * broker bounds every wait it serves, so a reply always comes or the socket
 * closes. */

static int read_full(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (r == 0) errno = ECONNRESET;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int hold_console_request(const char *sock_path, const char *request, size_t request_len,
                         unsigned char **reply, size_t *reply_len) {
    *reply = NULL;
    *reply_len = 0;
    if (request_len > HOLD_CONSOLE_MAX_REQUEST) {
        /* The broker would refuse it mid-write (a bare EPIPE here); say why. */
        errno = EMSGSIZE;
        return -1;
    }
    int fd = hold_connect_console_socket(sock_path);
    if (fd < 0) return -1;
    struct sigaction ign, old;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, &old);
    unsigned char hdr[CONSOLE_CONTROL_MAGIC_LEN + 4];
    memcpy(hdr, CONSOLE_CONTROL_MAGIC, CONSOLE_CONTROL_MAGIC_LEN);
    console_store_be32(hdr + CONSOLE_CONTROL_MAGIC_LEN, (uint32_t)request_len);
    int rc = -1;
    unsigned char lenbuf[4];
    if (hold_write_all(fd, hdr, sizeof(hdr)) != 0 || hold_write_all(fd, request, request_len) != 0) goto out;
    if (read_full(fd, lenbuf, 4) != 0) goto out;
    uint32_t n = console_load_be32(lenbuf);
    if (n > (64u << 20)) {
        errno = EPROTO;
        goto out;
    }
    *reply = malloc((size_t)n + 1);
    if (!*reply) goto out;
    if (read_full(fd, *reply, n) != 0) {
        free(*reply);
        *reply = NULL;
        goto out;
    }
    (*reply)[n] = '\0';
    *reply_len = n;
    rc = 0;
out:
    {
        int saved = errno;
        sigaction(SIGPIPE, &old, NULL);
        close(fd);
        errno = saved;
    }
    return rc;
}
