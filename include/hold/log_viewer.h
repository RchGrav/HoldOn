#pragma once
#ifndef HOLD_LOG_VIEWER_H
#define HOLD_LOG_VIEWER_H

#include "hold/config.h"

#define HOLD_LOG_VIEWER_MAX_EXAMPLES 8

struct hold_log_filter_options {
    const char *literal;
    const char *similar_examples[HOLD_LOG_VIEWER_MAX_EXAMPLES];
    size_t similar_example_count;
    const char *exclude_examples[HOLD_LOG_VIEWER_MAX_EXAMPLES];
    size_t exclude_example_count;
    double similar_threshold;
    size_t visible_capacity;
    size_t match_ring_capacity;
    size_t max_results;
    size_t scan_byte_budget;
};

struct hold_log_filter_result {
    char **lines;
    off_t *line_offsets;
    off_t *match_offsets;
    size_t line_count;
    size_t match_count;
    size_t match_ring_count;
    size_t match_ring_start;
    size_t bytes_read;
    size_t lines_scanned;
    off_t prev_offset;
    off_t next_offset;
    bool reached_eof;
    bool scan_limited;
};

typedef bool (*hold_log_viewer_running_fn)(void *userdata);

struct hold_log_viewer_follow {
    bool enabled;
    hold_log_viewer_running_fn is_running;
    void *userdata;
};

struct hold_log_viewer_context {
    const char *run_id;
    const char *profile;
    const char *command;
    const char *log_path;
};

void hold_log_filter_options_init(struct hold_log_filter_options *opts);
void hold_log_filter_result_free(struct hold_log_filter_result *result);
int hold_log_filter_fd(int fd,
                         const struct hold_log_filter_options *opts,
                         struct hold_log_filter_result *result);
int hold_log_filter_backward_fd(int fd,
                                  const struct hold_log_filter_options *opts,
                                  off_t anchor_offset,
                                  size_t byte_budget,
                                  struct hold_log_filter_result *result);
int hold_log_viewer_tty_fd(int fd,
                             const char *title,
                             const struct hold_log_filter_options *opts,
                             const struct hold_log_viewer_follow *follow,
                             const struct hold_log_viewer_context *context,
                             bool debug_stats);

#endif /* HOLD_LOG_VIEWER_H */
