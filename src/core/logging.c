#include "hold/config.h"
#include "hold/core.h"

static int write_json_escaped_fd(int fd, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char buf[8];
        const char *out = NULL;
        size_t out_n = 0;
        switch (c) {
        case '\\': out = "\\\\"; out_n = 2; break;
        case '"': out = "\\\""; out_n = 2; break;
        case '\n': out = "\\n"; out_n = 2; break;
        case '\r': out = "\\r"; out_n = 2; break;
        case '\t': out = "\\t"; out_n = 2; break;
        default:
            if (c < 0x20) {
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out = buf;
                out_n = strlen(buf);
            } else {
                buf[0] = (char)c;
                out = buf;
                out_n = 1;
            }
            break;
        }
        if (hold_write_all(fd, out, out_n) != 0) return -1;
    }
    return 0;
}

static void format_rfc3339_nano_utc(int64_t unix_ns, char *out, size_t n) {
    time_t sec = (time_t)(unix_ns / 1000000000LL);
    long ns = (long)(unix_ns % 1000000000LL);
    if (ns < 0) ns += 1000000000L;
    struct tm tm_utc;
    if (!gmtime_r(&sec, &tm_utc)) {
        snprintf(out, n, "-");
        return;
    }
    char base[32];
    if (strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm_utc) == 0) {
        snprintf(out, n, "-");
        return;
    }
    snprintf(out, n, "%s.%09ldZ", base, ns);
}

int hold_write_json_log_entry_fd(int fd, const char *stream, const char *data, size_t n) {
    if (fd < 0 || !stream || !*stream || (!data && n > 0)) {
        errno = EINVAL;
        return -1;
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    int64_t ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    char tbuf[64];
    format_rfc3339_nano_utc(ns, tbuf, sizeof(tbuf));
    if (hold_write_all(fd, "{\"log\":\"", 8) != 0) return -1;
    if (n > 0 && write_json_escaped_fd(fd, data, n) != 0) return -1;
    if (hold_write_all(fd, "\",\"stream\":\"", 12) != 0) return -1;
    if (write_json_escaped_fd(fd, stream, strlen(stream)) != 0) return -1;
    if (hold_write_all(fd, "\",\"time\":\"", 10) != 0) return -1;
    if (hold_write_all(fd, tbuf, strlen(tbuf)) != 0) return -1;
    return hold_write_all(fd, "\"}\n", 3);
}

int hold_write_json_log_bytes_fd(int fd, const char *stream, const char *data, size_t n) {
    if (n == 0) return 0;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            if (hold_write_json_log_entry_fd(fd, stream, data + start, i + 1 - start) != 0) return -1;
            start = i + 1;
        }
    }
    if (start < n) {
        if (hold_write_json_log_entry_fd(fd, stream, data + start, n - start) != 0) return -1;
    }
    return 0;
}

int hold_decode_json_log_line(const char *line, char **out) {
    if (!line || !out) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (line[0] == '{') {
        const char *v = NULL;
        if (hold_json_find_key(line, "log", &v) == 0) {
            size_t cap = strlen(line) + 1;
            char *decoded = malloc(cap);
            if (!decoded) return -1;
            if (hold_json_get_str(line, "log", decoded, cap) == 0) {
                *out = decoded;
                return 1;
            }
            free(decoded);
        }
    }
    char *copy = strdup(line);
    if (!copy) return -1;
    *out = copy;
    return 0;
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
