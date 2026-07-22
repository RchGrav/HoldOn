#include "hold/config.h"
#include "hold/core.h"

/*
 * HLOGIDX v1 sidecar: an 80-byte little-endian header (magic, version, sizes,
 * flags, base_unix_us) followed by packed 16-byte entries of 44-bit raw-log
 * offset / 20-bit len-1 / 48-bit microsecond delta / 16-bit meta. The writer
 * is append-only — the header is written once, never rewritten — and the
 * entry count derives from st_size, which also floors away any torn tail
 * entry after a crash. Indexing never fails or reorders the raw write: the
 * raw log stays the sole source of truth.
 */
#define HOLD_LOGIDX_MAGIC "HLOGIDX"
#define HOLD_LOGIDX_VERSION 1u
#define HOLD_LOGIDX_HEADER_SIZE 80u
#define HOLD_LOGIDX_ENTRY_SIZE 16u
#define HOLD_LOGIDX_F_LITTLE_ENDIAN 0x00000001u
#define HOLD_LOGIDX_OFFSET_BITS 44
#define HOLD_LOGIDX_TIME_BITS 48
#define HOLD_LOGIDX_META_STREAM_STDERR 0x0001u
#define HOLD_LOGIDX_META_NO_NEWLINE 0x0002u
#define HOLD_LOGIDX_META_TRUNCATED 0x0008u
#define HOLD_LOGIDX_OFFSET_MASK ((1ULL << HOLD_LOGIDX_OFFSET_BITS) - 1ULL)
#define HOLD_LOGIDX_LEN_MASK ((1ULL << 20) - 1ULL)
#define HOLD_LOGIDX_TIME_MASK ((1ULL << HOLD_LOGIDX_TIME_BITS) - 1ULL)

static uint64_t now_unix_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
}

static void put_le(unsigned char *p, int nbytes, uint64_t v) {
    for (int i = 0; i < nbytes; i++) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static uint64_t get_le(const unsigned char *p, int nbytes) {
    uint64_t v = 0;
    for (int i = nbytes - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static int write_full_at(int fd, const void *buf, size_t n, off_t off) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t wr = pwrite(fd, p, n, off);
        if (wr < 0 && errno == EINTR) continue;
        if (wr == 0) errno = EIO;
        if (wr <= 0) return -1;
        p += wr;
        n -= (size_t)wr;
        off += wr;
    }
    return 0;
}

int hold_log_idx_path(const char *log_path, char *out, size_t n) {
    if (!log_path || !*log_path || !out || n == 0) {
        errno = EINVAL;
        return -1;
    }
    return hold_checked_snprintf(out, n, "%s.idx", log_path);
}

/* Validates magic/version/geometry and returns base_unix_us. */
static int read_logidx_header(int fd, uint64_t *base_unix_us) {
    unsigned char buf[HOLD_LOGIDX_HEADER_SIZE];
    ssize_t nr;
    do {
        nr = pread(fd, buf, sizeof(buf), 0);
    } while (nr < 0 && errno == EINTR);
    if (nr != (ssize_t)sizeof(buf) || memcmp(buf, HOLD_LOGIDX_MAGIC, 8) != 0 ||
        get_le(buf + 8, 2) != HOLD_LOGIDX_VERSION || get_le(buf + 10, 2) != HOLD_LOGIDX_HEADER_SIZE ||
        get_le(buf + 12, 2) != HOLD_LOGIDX_ENTRY_SIZE) {
        errno = EINVAL;
        return -1;
    }
    *base_unix_us = get_le(buf + 24, 8);
    return 0;
}

static int write_logidx_header(int fd, uint64_t base_unix_us) {
    unsigned char buf[HOLD_LOGIDX_HEADER_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, HOLD_LOGIDX_MAGIC, 8);
    put_le(buf + 8, 2, HOLD_LOGIDX_VERSION);
    put_le(buf + 10, 2, HOLD_LOGIDX_HEADER_SIZE);
    put_le(buf + 12, 2, HOLD_LOGIDX_ENTRY_SIZE);
    put_le(buf + 16, 4, HOLD_LOGIDX_F_LITTLE_ENDIAN);
    put_le(buf + 24, 8, base_unix_us);
    return write_full_at(fd, buf, sizeof(buf), 0);
}

int hold_open_log_index_fd(const char *log_path, int raw_log_fd) {
    char idx_path[HOLD_PATH_MAX], dir[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) != 0) return -1;
    const char *slash = strrchr(idx_path, '/');
    if (!slash || slash == idx_path || !slash[1]) {
        errno = EINVAL;
        return -1;
    }
    memcpy(dir, idx_path, (size_t)(slash - idx_path));
    dir[slash - idx_path] = '\0';
    /* Open through the directory, refusing symlinks at both components. */
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return -1;
    int fd = openat(dirfd, slash + 1, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    int saved = errno;
    close(dirfd);
    if (fd < 0) {
        errno = saved;
        return -1;
    }
    struct stat st;
    if (fstat(raw_log_fd, &st) != 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return -1;
    }
    if (st.st_size >= (off_t)HOLD_LOGIDX_HEADER_SIZE) {
        uint64_t base = 0;
        if (read_logidx_header(fd, &base) == 0) return fd;
        /* Corrupt header: truncate and re-init, never fatal. */
        if (ftruncate(fd, 0) != 0) {
            close(fd);
            return -1;
        }
    }
    if (write_logidx_header(fd, now_unix_us()) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int append_logidx_entry(int idx_fd, uint64_t raw_offset, uint32_t len, uint64_t ts_us, uint32_t meta) {
    uint64_t base = 0;
    struct stat st;
    if (len == 0) return 0;
    if (read_logidx_header(idx_fd, &base) != 0 || fstat(idx_fd, &st) != 0) return -1;
    if (len > HOLD_LOGIDX_LEN_MASK + 1u) {
        /* Oversize records saturate; len is stored as len-1. */
        len = (uint32_t)(HOLD_LOGIDX_LEN_MASK + 1u);
        meta |= HOLD_LOGIDX_META_TRUNCATED;
    }
    uint64_t delta = ts_us >= base ? ts_us - base : 0;
    if (raw_offset > HOLD_LOGIDX_OFFSET_MASK || delta > HOLD_LOGIDX_TIME_MASK) {
        errno = EOVERFLOW;
        return -1;
    }
    unsigned char entry[HOLD_LOGIDX_ENTRY_SIZE];
    put_le(entry, 8, (raw_offset & HOLD_LOGIDX_OFFSET_MASK) | (((uint64_t)(len - 1u) & HOLD_LOGIDX_LEN_MASK) << HOLD_LOGIDX_OFFSET_BITS));
    put_le(entry + 8, 8, (delta & HOLD_LOGIDX_TIME_MASK) | (((uint64_t)meta & 0xffffu) << HOLD_LOGIDX_TIME_BITS));
    uint64_t entries = st.st_size > (off_t)HOLD_LOGIDX_HEADER_SIZE
        ? ((uint64_t)st.st_size - HOLD_LOGIDX_HEADER_SIZE) / HOLD_LOGIDX_ENTRY_SIZE : 0;
    return write_full_at(idx_fd, entry, sizeof(entry), (off_t)(HOLD_LOGIDX_HEADER_SIZE + entries * HOLD_LOGIDX_ENTRY_SIZE));
}

/* Raw write first, then best-effort index append at the pre-write EOF offset:
 * that offset is the viewer's lookup key. Index failures are ignored. */
static int write_indexed_chunk(int log_fd, int idx_fd, const char *data, size_t len, uint32_t meta) {
    off_t off = lseek(log_fd, 0, SEEK_END);
    if (off < 0) return -1;
    if (hold_write_all(log_fd, data, len) != 0) return -1;
    if (idx_fd >= 0) (void)append_logidx_entry(idx_fd, (uint64_t)off, (uint32_t)len, now_unix_us(), meta);
    return 0;
}

int hold_write_indexed_log_bytes_fd(int log_fd, int idx_fd, const char *stream, const char *data, size_t n) {
    if (n == 0) return 0;
    if (log_fd < 0 || !stream || !*stream || !data) {
        errno = EINVAL;
        return -1;
    }
    uint32_t stream_meta = !strcmp(stream, "stderr") ? HOLD_LOGIDX_META_STREAM_STDERR : 0;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] != '\n') continue;
        if (write_indexed_chunk(log_fd, idx_fd, data + start, i + 1 - start, stream_meta) != 0) return -1;
        start = i + 1;
    }
    if (start < n && write_indexed_chunk(log_fd, idx_fd, data + start, n - start, stream_meta | HOLD_LOGIDX_META_NO_NEWLINE) != 0) {
        return -1;
    }
    return 0;
}

void hold_logidx_map_free(struct hold_logidx_map *m) {
    if (!m) return;
    free(m->records);
    m->records = NULL;
    m->count = 0;
    m->base_unix_us = 0;
}

int hold_logidx_map_load(const char *log_path, struct hold_logidx_map *out) {
    if (!log_path || !out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char idx_path[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) != 0) return -1;
    int fd = open(idx_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    uint64_t base = 0;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < (off_t)HOLD_LOGIDX_HEADER_SIZE ||
        read_logidx_header(fd, &base) != 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    /* The physical entry count, floored so a torn tail entry never loads. */
    uint64_t count = ((uint64_t)st.st_size - HOLD_LOGIDX_HEADER_SIZE) / HOLD_LOGIDX_ENTRY_SIZE;
    struct hold_logidx_record *recs = NULL;
    if (count > 0 && !(recs = calloc((size_t)count, sizeof(*recs)))) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    size_t got = 0;
    for (uint64_t i = 0; i < count; i++) {
        unsigned char buf[HOLD_LOGIDX_ENTRY_SIZE];
        ssize_t nr;
        do {
            nr = pread(fd, buf, sizeof(buf), (off_t)(HOLD_LOGIDX_HEADER_SIZE + i * HOLD_LOGIDX_ENTRY_SIZE));
        } while (nr < 0 && errno == EINTR);
        if (nr != (ssize_t)sizeof(buf)) break;
        uint64_t pos_len = get_le(buf, 8);
        uint64_t time_meta = get_le(buf + 8, 8);
        recs[got].offset = (off_t)(pos_len & HOLD_LOGIDX_OFFSET_MASK);
        recs[got].len = (uint32_t)(((pos_len >> HOLD_LOGIDX_OFFSET_BITS) & HOLD_LOGIDX_LEN_MASK) + 1U);
        recs[got].ts_us = base + (time_meta & HOLD_LOGIDX_TIME_MASK);
        recs[got].meta = (uint16_t)((time_meta >> HOLD_LOGIDX_TIME_BITS) & 0xffffu);
        got++;
    }
    close(fd);
    out->records = recs;
    out->count = got;
    out->base_unix_us = base;
    return 0;
}

const struct hold_logidx_record *hold_logidx_map_find(const struct hold_logidx_map *m, off_t offset) {
    if (!m) return NULL;
    size_t lo = 0, hi = m->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->records[mid].offset == offset) return &m->records[mid];
        if (m->records[mid].offset < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

enum hold_log_stream hold_logidx_record_stream(uint16_t meta) {
    return (meta & HOLD_LOGIDX_META_STREAM_STDERR) ? HOLD_LOG_STREAM_STDERR : HOLD_LOG_STREAM_STDOUT;
}

size_t hold_logidx_format_time(uint64_t ts_us, enum hold_ts_mode mode, bool utc, char *out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = '\0';
    if (mode == HOLD_TS_NONE) return 0;
    time_t secs = (time_t)(ts_us / 1000000ULL);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (utc ? !gmtime_r(&secs, &tm) : !localtime_r(&secs, &tm)) return 0;
    const char *fmt;
    if (mode == HOLD_TS_DATE) {
        fmt = utc ? "%Y-%m-%d %H:%M:%SZ " : "%Y-%m-%d %H:%M:%S ";
    } else {
        fmt = utc ? "%H:%M:%SZ " : "%H:%M:%S ";
    }
    return strftime(out, n, fmt, &tm);
}
