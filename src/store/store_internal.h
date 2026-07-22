#pragma once
#ifndef HOLD_STORE_INTERNAL_H
#define HOLD_STORE_INTERNAL_H

#include "hold/config.h"
#include "hold/types.h"

/* The one record schema: a single field table (record.c) drives the private
 * writer, the strict reader, and — through the shared emit helpers — the
 * public projection writer. Fields with structure (argv, env, mode, observed)
 * stay as explicit specials next to the table. */

enum { RF_STR, RF_BOOL, RF_INT, RF_I64, RF_U64, RF_PID, RF_UID, RF_GID };

enum {
    RF_REQ = 1 << 0,      /* read: required, the load fails when absent */
    RF_ALIAS = 1 << 1,    /* read: accept only a valid call name */
    RF_ABS = 1 << 2,      /* read: accept only an absolute path */
    RF_NONEMPTY = 1 << 3, /* read: treat an empty string as absent */
    RF_WRONLY = 1 << 4,   /* write only: the reader owns a default or shim */
};

#define RF_ALWAYS 0xffffffffu /* has_off value: field written unconditionally */

struct hold_record_field {
    const char *key;
    unsigned char type;
    unsigned char flags;
    unsigned short size;  /* string capacity */
    unsigned int off;     /* field offset in struct hold_run_record */
    unsigned int has_off; /* presence-flag offset, or RF_ALWAYS */
};

extern const struct hold_record_field hold_record_fields[];
extern const size_t hold_record_field_count;

void hold_emit_kv(FILE *f, bool *first, const char *key);
void hold_emit_kv_str(FILE *f, bool *first, const char *key, const char *val);
void hold_record_emit_fields(FILE *f, bool *first, const struct hold_run_record *r);
int hold_record_read_fields(const char *json, struct hold_run_record *r);
int hold_record_checked_i64_to_pid(int64_t v, pid_t *out);

/* The ONLY commit tail in the store (atomic.c). */
typedef int (*hold_json_emit_fn)(FILE *f, void *ctx);
int hold_atomic_write_json(const char *dir, const char *final_name, mode_t mode,
                           bool chown_root, hold_json_emit_fn emit, void *ctx,
                           char *out_path, size_t out_n);

#endif /* HOLD_STORE_INTERNAL_H */
