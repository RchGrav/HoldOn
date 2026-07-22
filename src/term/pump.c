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

/* ---- THE detach-key FSM, shared by console attach and the hold-on shell ----
 *
 * pending is always a strict prefix of keys, so feeding a byte either extends
 * the prefix (arming the flush deadline), completes the chord, or unwinds:
 * the byte that broke the match is popped, the surviving prefix is released
 * through the sink, and the byte is re-fed against the empty state. */

static int64_t term_now_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
}

void hold_term_detach_init(struct hold_term_detach *d, const unsigned char *keys, size_t nkeys) {
    memset(d, 0, sizeof(*d));
    if (!keys || nkeys == 0 || nkeys > HOLD_TERM_DETACH_MAX_KEYS) return;
    memcpy(d->keys, keys, nkeys);
    d->nkeys = nkeys;
}

int hold_term_detach_flush(struct hold_term_detach *d, hold_term_detach_sink sink, void *ctx) {
    if (d->pending_len == 0) return 0;
    size_t n = d->pending_len;
    d->pending_len = 0;
    return sink(ctx, d->pending, n) != 0 ? -1 : 0;
}

int hold_term_detach_feed(struct hold_term_detach *d, unsigned char c,
                          hold_term_detach_sink sink, void *ctx, bool *detached) {
    *detached = false;
    if (d->nkeys == 0) return sink(ctx, &c, 1) != 0 ? -1 : 0;
    for (;;) {
        if (d->pending_len == 0 && c != d->keys[0]) return sink(ctx, &c, 1) != 0 ? -1 : 0;
        if (d->pending_len >= HOLD_TERM_DETACH_MAX_KEYS) {
            if (hold_term_detach_flush(d, sink, ctx) != 0) return -1;
            continue;
        }
        d->pending[d->pending_len++] = c;
        if (d->pending_len == d->nkeys && memcmp(d->pending, d->keys, d->nkeys) == 0) {
            d->pending_len = 0;
            *detached = true;
            return 0;
        }
        if (d->pending_len < d->nkeys && memcmp(d->pending, d->keys, d->pending_len) == 0) {
            d->deadline_usec = term_now_usec() + HOLD_TERM_DETACH_FLUSH_USEC;
            return 0;
        }
        d->pending_len--;
        if (hold_term_detach_flush(d, sink, ctx) != 0) return -1;
        /* re-feed c against the now-empty pending state */
    }
}

int hold_term_detach_timeout_ms(const struct hold_term_detach *d) {
    if (d->pending_len == 0) return -1;
    int64_t remaining = d->deadline_usec - term_now_usec();
    if (remaining <= 0) return 0;
    return (int)((remaining + 999) / 1000);
}
