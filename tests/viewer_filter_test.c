#include "hold/config.h"
#include "hold/core.h"
#include "hold/log_viewer.h"
#include "hold/platform.h"

static int failures = 0;

#define EXPECT_TRUE(name, expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, name); \
        failures++; \
    } \
} while (0)

static int temp_log_fd(void) {
    char tmpl[] = "/tmp/hold-viewer-test.XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        perror("mkstemp");
        exit(2);
    }
    unlink(tmpl);
    return fd;
}

static void write_all_or_die(int fd, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            exit(2);
        }
        s += w;
        n -= (size_t)w;
    }
}

static void rewind_or_die(int fd) {
    if (lseek(fd, 0, SEEK_SET) < 0) {
        perror("lseek");
        exit(2);
    }
}

static void test_log_index_rejects_symlink(void) {
    char dir[] = "/tmp/hold-log-index-test.XXXXXX";
    EXPECT_TRUE("mkdtemp for log index symlink test", mkdtemp(dir) != NULL);
    if (!dir[0]) return;

    char log_path[HOLD_PATH_MAX];
    char idx_path[HOLD_PATH_MAX];
    char sentinel_path[HOLD_PATH_MAX];
    EXPECT_TRUE("format log path", hold_checked_snprintf(log_path, sizeof(log_path), "%s/run.log", dir) == 0);
    EXPECT_TRUE("format idx path", hold_checked_snprintf(idx_path, sizeof(idx_path), "%s.idx", log_path) == 0);
    EXPECT_TRUE("format sentinel path", hold_checked_snprintf(sentinel_path, sizeof(sentinel_path), "%s/sentinel", dir) == 0);

    int logfd = open(log_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    EXPECT_TRUE("create raw log", logfd >= 0);
    int sentinel = open(sentinel_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    EXPECT_TRUE("create sentinel", sentinel >= 0);
    if (sentinel >= 0) {
        write_all_or_die(sentinel, "sentinel\n");
        close(sentinel);
    }
    EXPECT_TRUE("plant idx symlink", symlink(sentinel_path, idx_path) == 0);

    int idxfd = logfd >= 0 ? hold_open_log_index_fd(log_path, logfd) : -1;
    EXPECT_TRUE("symlinked log index is rejected", idxfd < 0);
    if (idxfd >= 0) close(idxfd);
    if (logfd >= 0) close(logfd);

    int check = open(sentinel_path, O_RDONLY | O_CLOEXEC);
    EXPECT_TRUE("sentinel still exists", check >= 0);
    if (check >= 0) {
        char buf[32];
        ssize_t n = read(check, buf, sizeof(buf) - 1);
        EXPECT_TRUE("sentinel still readable", n > 0);
        if (n > 0) {
            buf[n] = '\0';
            EXPECT_TRUE("sentinel content untouched", strcmp(buf, "sentinel\n") == 0);
        }
        close(check);
    }
    unlink(idx_path);
    unlink(log_path);
    unlink(sentinel_path);
    rmdir(dir);
}

static void test_json_rejects_raw_control_bytes(void) {
    char out[16];
    EXPECT_TRUE("raw control byte in JSON string is rejected",
                hold_parse_json_string("\"bad\001\"", out, sizeof(out), NULL) != 0);
    EXPECT_TRUE("escaped control byte in JSON string remains accepted",
                hold_parse_json_string("\"bad\\n\"", out, sizeof(out), NULL) == 0 && strcmp(out, "bad\n") == 0);
}

static void test_path_empty_components_do_not_resolve_cwd(void) {
    char original_cwd[HOLD_PATH_MAX];
    EXPECT_TRUE("save cwd", getcwd(original_cwd, sizeof(original_cwd)) != NULL);

    char dir[] = "/tmp/hold-path-test.XXXXXX";
    EXPECT_TRUE("mkdtemp for path test", mkdtemp(dir) != NULL);
    char cwd_helper[HOLD_PATH_MAX];
    char bin_dir[HOLD_PATH_MAX];
    char bin_helper[HOLD_PATH_MAX];
    EXPECT_TRUE("format cwd helper", hold_checked_snprintf(cwd_helper, sizeof(cwd_helper), "%s/fake-helper", dir) == 0);
    EXPECT_TRUE("format bin dir", hold_checked_snprintf(bin_dir, sizeof(bin_dir), "%s/bin", dir) == 0);
    EXPECT_TRUE("format bin helper", hold_checked_snprintf(bin_helper, sizeof(bin_helper), "%s/fake-helper", bin_dir) == 0);
    EXPECT_TRUE("mkdir bin dir", mkdir(bin_dir, 0700) == 0);

    int fd = open(cwd_helper, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    EXPECT_TRUE("create cwd helper", fd >= 0);
    if (fd >= 0) close(fd);
    fd = open(bin_helper, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    EXPECT_TRUE("create PATH helper", fd >= 0);
    if (fd >= 0) close(fd);

    char resolved[HOLD_PATH_MAX];
    EXPECT_TRUE("chdir path temp", chdir(dir) == 0);
    EXPECT_TRUE("empty-only PATH does not resolve cwd helper",
                setenv("PATH", ":", 1) == 0 && hold_resolve_binary_path("fake-helper", resolved, sizeof(resolved)) != 0);
    char path_value[HOLD_PATH_MAX];
    EXPECT_TRUE("format PATH with empty components", hold_checked_snprintf(path_value, sizeof(path_value), ":%s:", bin_dir) == 0);
    char *expected = realpath(bin_helper, NULL);
    EXPECT_TRUE("realpath PATH helper", expected != NULL);
    EXPECT_TRUE("PATH non-empty component still resolves",
                expected && setenv("PATH", path_value, 1) == 0 && hold_resolve_binary_path("fake-helper", resolved, sizeof(resolved)) == 0 &&
                    strcmp(resolved, expected) == 0);
    free(expected);

    EXPECT_TRUE("restore cwd", chdir(original_cwd) == 0);
    unlink(cwd_helper);
    unlink(bin_helper);
    rmdir(bin_dir);
    rmdir(dir);
}

static void test_literal_filter(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd, "alpha\nneedle one\nbeta\nneedle two\n");
    rewind_or_die(fd);

    struct hold_log_filter_options opts;
    hold_log_filter_options_init(&opts);
    opts.literal = "needle";
    opts.max_results = 2;
    opts.visible_capacity = 2;
    struct hold_log_filter_result result;
    EXPECT_TRUE("literal filter succeeds", hold_log_filter_fd(fd, &opts, &result) == 0);
    EXPECT_TRUE("literal returns two lines", result.line_count == 2);
    EXPECT_TRUE("first literal line", result.line_count > 0 && strstr(result.lines[0], "needle one"));
    EXPECT_TRUE("second literal line", result.line_count > 1 && strstr(result.lines[1], "needle two"));
    EXPECT_TRUE("visible offsets are recorded", result.line_count > 1 && result.line_offsets[0] < result.line_offsets[1]);
    EXPECT_TRUE("next offset advances", result.next_offset > result.line_offsets[1]);
    hold_log_filter_result_free(&result);
    close(fd);
}

static void test_similarity_filter(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd,
                     "info request completed normally\n"
                     "warn database connection timeout retrying\n"
                     "debug cache warmed successfully\n");
    rewind_or_die(fd);

    struct hold_log_filter_options opts;
    hold_log_filter_options_init(&opts);
    opts.similar_examples[0] = "error database connection timeout";
    opts.similar_example_count = 1;
    opts.similar_threshold = 0.45;
    opts.max_results = 1;
    opts.visible_capacity = 1;
    struct hold_log_filter_result result;
    EXPECT_TRUE("similar filter succeeds", hold_log_filter_fd(fd, &opts, &result) == 0);
    EXPECT_TRUE("similar returns one line", result.line_count == 1);
    EXPECT_TRUE("similar line selected", result.line_count > 0 && strstr(result.lines[0], "database connection timeout"));
    hold_log_filter_result_free(&result);
    close(fd);
}

static void test_exclude_similarity_filter(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd,
                     "info request completed normally\n"
                     "warn database connection timeout retrying\n"
                     "debug cache warmed successfully\n");
    rewind_or_die(fd);

    struct hold_log_filter_options opts;
    hold_log_filter_options_init(&opts);
    opts.exclude_examples[0] = "error database connection timeout";
    opts.exclude_example_count = 1;
    opts.similar_threshold = 0.45;
    opts.max_results = 3;
    opts.visible_capacity = 3;
    struct hold_log_filter_result result;
    EXPECT_TRUE("exclude filter succeeds", hold_log_filter_fd(fd, &opts, &result) == 0);
    EXPECT_TRUE("exclude removes one similar line", result.line_count == 2);
    EXPECT_TRUE("exclude keeps normal info", result.line_count > 0 && strstr(result.lines[0], "request completed"));
    EXPECT_TRUE("exclude keeps debug", result.line_count > 1 && strstr(result.lines[1], "cache warmed"));
    hold_log_filter_result_free(&result);
    close(fd);
}

static void test_match_ring_wraps(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd, "hit-0\nhit-1\nhit-2\nhit-3\n");
    rewind_or_die(fd);

    struct hold_log_filter_options opts;
    hold_log_filter_options_init(&opts);
    opts.literal = "hit";
    opts.max_results = 4;
    opts.visible_capacity = 4;
    opts.match_ring_capacity = 2;
    struct hold_log_filter_result result;
    EXPECT_TRUE("ring filter succeeds", hold_log_filter_fd(fd, &opts, &result) == 0);
    EXPECT_TRUE("all matches counted", result.match_count == 4);
    EXPECT_TRUE("ring capped", result.match_ring_count == 2);
    EXPECT_TRUE("ring start advanced", result.match_ring_start == 0 || result.match_ring_start == 1);
    hold_log_filter_result_free(&result);
    close(fd);
}

static void test_lazy_large_file_stops_after_first_screen(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd, "match early 1\nmatch early 2\n");
    for (int i = 0; i < 20000; i++) {
        write_all_or_die(fd, "boring filler line that must not be scanned before first screen\n");
    }
    off_t size = lseek(fd, 0, SEEK_END);
    rewind_or_die(fd);

    struct hold_log_filter_options opts;
    hold_log_filter_options_init(&opts);
    opts.literal = "match early";
    opts.max_results = 2;
    opts.visible_capacity = 2;
    struct hold_log_filter_result result;
    EXPECT_TRUE("lazy large filter succeeds", hold_log_filter_fd(fd, &opts, &result) == 0);
    EXPECT_TRUE("first screen has two matches", result.line_count == 2);
    EXPECT_TRUE("did not scan whole file", result.bytes_read < (size_t)size / 4);
    EXPECT_TRUE("did not reach eof", !result.reached_eof);
    hold_log_filter_result_free(&result);
    close(fd);
}

static void test_next_offset_resumes_next_screen(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd, "hit one\nhit two\nhit three\nhit four\n");
    rewind_or_die(fd);

    struct hold_log_filter_options opts;
    hold_log_filter_options_init(&opts);
    opts.literal = "hit";
    opts.max_results = 2;
    opts.visible_capacity = 2;
    struct hold_log_filter_result first;
    EXPECT_TRUE("first page succeeds", hold_log_filter_fd(fd, &opts, &first) == 0);
    EXPECT_TRUE("first page has first hit", first.line_count > 0 && strstr(first.lines[0], "hit one"));
    EXPECT_TRUE("first page has second hit", first.line_count > 1 && strstr(first.lines[1], "hit two"));

    EXPECT_TRUE("seek to next offset", lseek(fd, first.next_offset, SEEK_SET) == first.next_offset);
    struct hold_log_filter_result second;
    EXPECT_TRUE("second page succeeds", hold_log_filter_fd(fd, &opts, &second) == 0);
    EXPECT_TRUE("second page has third hit", second.line_count > 0 && strstr(second.lines[0], "hit three"));
    EXPECT_TRUE("second page has fourth hit", second.line_count > 1 && strstr(second.lines[1], "hit four"));
    hold_log_filter_result_free(&second);
    hold_log_filter_result_free(&first);
    close(fd);
}

static void test_backward_tail_window_finds_recent_matches_without_full_scan(void) {
    int fd = temp_log_fd();
    for (int i = 0; i < 20000; i++) {
        write_all_or_die(fd, "boring filler line that should not be scanned from the live edge\n");
    }
    write_all_or_die(fd, "recent needle one\nrecent needle two\nrecent needle three\n");
    off_t end = lseek(fd, 0, SEEK_END);

    struct hold_log_filter_options opts;
    hold_log_filter_options_init(&opts);
    opts.literal = "recent needle";
    opts.max_results = 2;
    opts.visible_capacity = 2;
    struct hold_log_filter_result result;
    EXPECT_TRUE("backward tail filter succeeds", hold_log_filter_backward_fd(fd, &opts, end, 65536, &result) == 0);
    EXPECT_TRUE("backward tail returns requested visible lines", result.line_count == 2);
    EXPECT_TRUE("backward tail keeps chronological order", result.line_count > 1 &&
                    strstr(result.lines[0], "recent needle two") &&
                    strstr(result.lines[1], "recent needle three"));
    EXPECT_TRUE("backward tail did not scan whole file", result.bytes_read < (size_t)end / 4);
    hold_log_filter_result_free(&result);
    close(fd);
}

static void test_backward_sparse_window_reports_partial(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd, "ancient needle\n");
    for (int i = 0; i < 20000; i++) {
        write_all_or_die(fd, "boring filler line after old match\n");
    }
    off_t end = lseek(fd, 0, SEEK_END);

    struct hold_log_filter_options opts;
    hold_log_filter_options_init(&opts);
    opts.literal = "ancient needle";
    opts.max_results = 1;
    opts.visible_capacity = 1;
    struct hold_log_filter_result result;
    EXPECT_TRUE("backward sparse filter succeeds", hold_log_filter_backward_fd(fd, &opts, end, 32768, &result) == 0);
    EXPECT_TRUE("sparse backward window does not scan to ancient match", result.line_count == 0);
    EXPECT_TRUE("sparse backward window reports limited scan", result.scan_limited);
    EXPECT_TRUE("sparse backward window bounded bytes", result.bytes_read < (size_t)end / 4);
    hold_log_filter_result_free(&result);
    close(fd);
}

/* Writes a real raw log plus HLOGIDX sidecar in a temp dir and returns the
 * open raw-log fd (seeked to start); *dir_out holds the directory to clean up. */
static int build_indexed_log(char *log_path_out, size_t log_path_cap, char *dir_out) {
    strcpy(dir_out, "/tmp/hold-idx-map-test.XXXXXX");
    if (!mkdtemp(dir_out)) {
        perror("mkdtemp");
        exit(2);
    }
    if (hold_checked_snprintf(log_path_out, log_path_cap, "%s/run.log", dir_out) != 0) exit(2);
    int logfd = open(log_path_out, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (logfd < 0) {
        perror("open log");
        exit(2);
    }
    int idxfd = hold_open_log_index_fd(log_path_out, logfd);
    if (idxfd < 0) {
        perror("open idx");
        exit(2);
    }
    /* offset 0: stdout "out one\n"; offset 8: stderr "err two\n". */
    (void)hold_write_indexed_log_bytes_fd(logfd, idxfd, "stdout", "out one\n", 8);
    (void)hold_write_indexed_log_bytes_fd(logfd, idxfd, "stderr", "err two\n", 8);
    close(idxfd);
    rewind_or_die(logfd);
    return logfd;
}

static void remove_dir(const char *dir) {
    char cmd[HOLD_PATH_MAX + 16];
    if (hold_checked_snprintf(cmd, sizeof(cmd), "rm -rf %s", dir) == 0) {
        if (system(cmd) != 0) { /* best effort cleanup */ }
    }
}

static void test_logidx_map_reads_timestamps_and_streams(void) {
    char dir[HOLD_PATH_MAX];
    char log_path[HOLD_PATH_MAX];
    int logfd = build_indexed_log(log_path, sizeof(log_path), dir);

    struct hold_logidx_map map;
    EXPECT_TRUE("sidecar map loads", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("sidecar map has two records", map.count == 2);
    EXPECT_TRUE("sidecar base timestamp is set", map.base_unix_us > 0);

    const struct hold_logidx_record *r0 = hold_logidx_map_find(&map, 0);
    const struct hold_logidx_record *r1 = hold_logidx_map_find(&map, 8);
    EXPECT_TRUE("record at offset 0 found", r0 != NULL);
    EXPECT_TRUE("record at offset 8 found", r1 != NULL);
    EXPECT_TRUE("missing offset returns NULL", hold_logidx_map_find(&map, 4) == NULL);
    if (r0) EXPECT_TRUE("offset 0 is stdout", hold_logidx_record_stream(r0->meta) == HOLD_LOG_STREAM_STDOUT);
    if (r1) EXPECT_TRUE("offset 8 is stderr", hold_logidx_record_stream(r1->meta) == HOLD_LOG_STREAM_STDERR);
    if (r0) EXPECT_TRUE("record length is 8", r0->len == 8);

    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

static void test_logidx_time_formatting(void) {
    /* 2021-01-01 00:00:00 UTC */
    uint64_t ts_us = 1609459200ULL * 1000000ULL;
    char buf[64];
    EXPECT_TRUE("ts none writes nothing",
                hold_logidx_format_time(ts_us, HOLD_TS_NONE, false, buf, sizeof(buf)) == 0 && buf[0] == '\0');
    hold_logidx_format_time(ts_us, HOLD_TS_TIME, true, buf, sizeof(buf));
    EXPECT_TRUE("time UTC prefix", strcmp(buf, "00:00:00Z ") == 0);
    hold_logidx_format_time(ts_us, HOLD_TS_DATE, true, buf, sizeof(buf));
    EXPECT_TRUE("date UTC prefix", strcmp(buf, "2021-01-01 00:00:00Z ") == 0);
    hold_logidx_format_time(ts_us, HOLD_TS_TIME, false, buf, sizeof(buf));
    EXPECT_TRUE("time local has no Z and trailing space",
                strlen(buf) == 9 && buf[8] == ' ' && strchr(buf, 'Z') == NULL);
}

static void test_source_mask_filters_by_stream(void) {
    char dir[HOLD_PATH_MAX];
    char log_path[HOLD_PATH_MAX];
    int logfd = build_indexed_log(log_path, sizeof(log_path), dir);

    struct hold_logidx_map map;
    EXPECT_TRUE("mask test loads map", hold_logidx_map_load(log_path, &map) == 0);

    struct hold_log_filter_options opts;
    struct hold_log_filter_result result;

    hold_log_filter_options_init(&opts);
    opts.idx_map = &map;
    opts.source_mask = HOLD_LOG_SRC_STDOUT;
    rewind_or_die(logfd);
    EXPECT_TRUE("stdout-only filter runs", hold_log_filter_fd(logfd, &opts, &result) == 0);
    EXPECT_TRUE("stdout-only keeps one line", result.line_count == 1);
    EXPECT_TRUE("stdout-only keeps the stdout line", result.line_count == 1 && strstr(result.lines[0], "out one"));
    hold_log_filter_result_free(&result);

    hold_log_filter_options_init(&opts);
    opts.idx_map = &map;
    opts.source_mask = HOLD_LOG_SRC_STDERR;
    rewind_or_die(logfd);
    EXPECT_TRUE("stderr-only filter runs", hold_log_filter_fd(logfd, &opts, &result) == 0);
    EXPECT_TRUE("stderr-only keeps the stderr line", result.line_count == 1 && strstr(result.lines[0], "err two"));
    hold_log_filter_result_free(&result);

    hold_log_filter_options_init(&opts);
    opts.idx_map = &map;
    opts.source_mask = HOLD_LOG_SRC_ALL;
    rewind_or_die(logfd);
    EXPECT_TRUE("mask-all filter runs", hold_log_filter_fd(logfd, &opts, &result) == 0);
    EXPECT_TRUE("mask-all keeps both lines", result.line_count == 2);
    hold_log_filter_result_free(&result);

    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

int main(void) {
    test_log_index_rejects_symlink();
    test_logidx_map_reads_timestamps_and_streams();
    test_logidx_time_formatting();
    test_source_mask_filters_by_stream();
    test_json_rejects_raw_control_bytes();
    test_path_empty_components_do_not_resolve_cwd();
    test_literal_filter();
    test_similarity_filter();
    test_exclude_similarity_filter();
    test_match_ring_wraps();
    test_lazy_large_file_stops_after_first_screen();
    test_next_offset_resumes_next_screen();
    test_backward_tail_window_finds_recent_matches_without_full_scan();
    test_backward_sparse_window_reports_partial();
    if (failures) {
        fprintf(stderr, "viewer_filter_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: viewer filter engine");
    return 0;
}
