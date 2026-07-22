#include "hold/config.h"
#include "hold/core.h"
#include "hold/term.h"

/* THE PTY-master pump. Every reader of a hold-owned PTY master drains it
 * through this one step so the capture path (indexed log first, then caller
 * fan-out) and the end-of-target semantics live in exactly one place.
 *
 * End-of-target: a PTY master reports the last slave closing either as EOF
 * or as read() failing with EIO (Linux). EIO is checked only when the read
 * actually failed — never against a stale errno from a successful read. */
ssize_t hold_term_pump_master(int master, int logfd, int logidxfd,
                              char *buf, size_t n) {
    ssize_t r = read(master, buf, n);
    if (r > 0) {
        (void)hold_write_indexed_log_bytes_fd(logfd, logidxfd, "stdout", buf, (size_t)r);
        return r;
    }
    if (r == 0 || (r < 0 && errno == EIO)) {
        return 0;
    }
    return -1;
}
