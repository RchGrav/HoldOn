#pragma once
#ifndef HOLD_LOG_VIEWER_H
#define HOLD_LOG_VIEWER_H

#include "hold/config.h"

#define HOLD_LOG_VIEWER_MAX_EXAMPLES 8

/* Source visibility bits. A mask of 0 means "all sources visible" so
 * zero-initialised options keep every record (no metadata lookup needed). */
#define HOLD_LOG_SRC_STDOUT 0x1u
#define HOLD_LOG_SRC_STDERR 0x2u
#define HOLD_LOG_SRC_STDIN 0x4u
#define HOLD_LOG_SRC_PTY 0x8u
#define HOLD_LOG_SRC_ALL 0u

struct hold_logidx_map; /* from hold/core.h */

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
    /* Optional sidecar-backed source filtering. When source_mask is nonzero,
     * records whose stream bit is clear are skipped using idx_map metadata
     * keyed by byte offset; records with no index entry are kept. */
    const struct hold_logidx_map *idx_map;
    unsigned source_mask;
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
#endif /* HOLD_LOG_VIEWER_H */
