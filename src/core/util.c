#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"

void hold_sig_note(const struct hold_invocation *inv, const char *fmt, ...) {
    if (inv && inv->quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

_Noreturn void hold_die_errno(const char *msg) {
    int e = errno;
    fprintf(stderr, "%s: %s\n", msg, strerror(e));
    exit(1);
}

/* Truncation is ENAMETOOLONG, never a silently shortened path. */
int hold_checked_snprintf(char *dst, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

bool hold_has_suffix(const char *s, const char *suffix) {
    size_t sl = strlen(s), sufl = strlen(suffix);
    return sl >= sufl && strcmp(s + (sl - sufl), suffix) == 0;
}

int hold_write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) {
            errno = EIO;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int put_ch(char *dst, size_t n, size_t *off, char c) {
    if (*off + 1 >= n) return -1;
    dst[(*off)++] = c;
    dst[*off] = '\0';
    return 0;
}

static bool cmd_arg_needs_quotes(const char *arg) {
    if (!*arg) return true;
    for (const unsigned char *p = (const unsigned char *)arg; *p; p++) {
        if (isspace(*p) || strchr("'\"\\$`!*?[]{}()<>|&;#", (int)*p)) return true;
    }
    return false;
}

/* Appends arg shell-quoted (single quotes, '\'' for embedded quotes) when it
 * needs it, verbatim when it does not: the COMMAND column formatter. */
static int append_cmd_human(char *dst, size_t n, size_t *off, const char *arg) {
    if (!arg) arg = "";
    if (!cmd_arg_needs_quotes(arg)) {
        for (; *arg; arg++) {
            if (put_ch(dst, n, off, *arg) != 0) return -1;
        }
        return 0;
    }
    if (put_ch(dst, n, off, '\'') != 0) return -1;
    for (; *arg; arg++) {
        if (*arg == '\'') {
            for (const char *sq = "'\\''"; *sq; sq++) {
                if (put_ch(dst, n, off, *sq) != 0) return -1;
            }
        } else if (put_ch(dst, n, off, *arg) != 0) {
            return -1;
        }
    }
    return put_ch(dst, n, off, '\'');
}

int hold_format_argv_human(char *dst, size_t n, int argc, char **argv) {
    if (!dst || n == 0 || argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    size_t off = 0;
    dst[0] = '\0';
    for (int i = 0; i < argc; i++) {
        if (i > 0 && put_ch(dst, n, &off, ' ') != 0) return -1;
        if (append_cmd_human(dst, n, &off, argv[i]) != 0) return -1;
    }
    return 0;
}

/* Reads the exec errno handshake: EOF = exec succeeded (returns 0), a full
 * int = child's errno (returns 1), anything else is a transport failure. */
int hold_read_exec_handshake(int fd, int *child_errno) {
    unsigned char *p = (unsigned char *)child_errno;
    size_t got = 0;
    *child_errno = 0;
    while (got < sizeof(*child_errno)) {
        ssize_t n = read(fd, p + got, sizeof(*child_errno) - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) {
            if (got == 0) return 0;
            errno = EPROTO;
            return -1;
        }
        if (errno == EINTR) continue;
        return -1;
    }
    return 1;
}

void hold_format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n) {
    time_t sec = (time_t)(unix_ns / 1000000000LL);
    struct tm tm_utc;
    if (!gmtime_r(&sec, &tm_utc) || strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        snprintf(out, n, "-");
    }
}

/* Docker's go-units HumanDuration, character for character: the phrasing the
 * call table borrows for CREATED ("2 minutes") and STATUS ("Up 2 minutes",
 * "Exited (0) 2 days ago"). The caller appends " ago" where the reference is
 * a past instant. */
void hold_format_duration_human(int64_t seconds, char *out, size_t n) {
    if (seconds < 0) seconds = 0;
    if (seconds < 1) {
        snprintf(out, n, "Less than a second");
        return;
    }
    if (seconds < 60) {
        snprintf(out, n, "%" PRId64 " second%s", seconds, seconds == 1 ? "" : "s");
        return;
    }
    int64_t minutes = seconds / 60;
    if (minutes == 1) {
        snprintf(out, n, "About a minute");
        return;
    }
    if (minutes < 46) {
        snprintf(out, n, "%" PRId64 " minutes", minutes);
        return;
    }
    /* Docker rounds hours to the nearest whole: int(d.Hours() + 0.5). */
    int64_t hours = (seconds + 1800) / 3600;
    if (hours == 1) {
        snprintf(out, n, "About an hour");
    } else if (hours < 48) {
        snprintf(out, n, "%" PRId64 " hours", hours);
    } else if (hours < 24 * 7 * 2) {
        snprintf(out, n, "%" PRId64 " days", hours / 24);
    } else if (hours < 24 * 30 * 2) {
        snprintf(out, n, "%" PRId64 " weeks", hours / 24 / 7);
    } else if (hours < 24 * 365 * 2) {
        snprintf(out, n, "%" PRId64 " months", hours / 24 / 30);
    } else {
        snprintf(out, n, "%" PRId64 " years", hours / 24 / 365);
    }
}

/* Parse an RFC3339 UTC instant ("2026-07-03T01:23:45Z", fractional seconds and
 * the trailing Z ignored) into unix nanoseconds. Returns false on any garbage
 * or the zero-value placeholder Hold writes for records without a timestamp. */
bool hold_parse_rfc3339_utc_to_ns(const char *s, int64_t *out_ns) {
    if (!s || !*s) return false;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!strptime(s, "%Y-%m-%dT%H:%M:%S", &tm)) return false;
    time_t t = timegm(&tm);
    if (t == (time_t)-1 || t <= 0) return false;
    if (out_ns) *out_ns = (int64_t)t * 1000000000LL;
    return true;
}

const char *hold_run_id_display(const char *id, char out[ID_DISPLAY_HEX_LEN + 1]) {
    if (!out) return "";
    if (!id) {
        out[0] = '\0';
        return out;
    }
    size_t n = strlen(id);
    if (n > ID_DISPLAY_HEX_LEN) n = ID_DISPLAY_HEX_LEN;
    memcpy(out, id, n);
    out[n] = '\0';
    return out;
}

int hold_copy_argv(char ***out, int argc, char **argv) {
    *out = NULL;
    if (argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    char **copy = calloc((size_t)argc + 1, sizeof(char *));
    if (!copy) return -1;
    for (int i = 0; i < argc; i++) {
        copy[i] = strdup(argv[i]);
        if (!copy[i]) {
            hold_free_argv_alloc(copy, i);
            return -1;
        }
    }
    copy[argc] = NULL;
    *out = copy;
    return 0;
}
