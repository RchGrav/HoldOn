#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"

void hold_json_escape(FILE *f, const char *s) {
    for (; *s; s++) {
        char letter = 0;
        switch (*s) {
        case '"': letter = '"'; break;
        case '\\': letter = '\\'; break;
        case '\n': letter = 'n'; break;
        case '\r': letter = 'r'; break;
        case '\t': letter = 't'; break;
        case '\b': letter = 'b'; break;
        case '\f': letter = 'f'; break;
        default: break;
        }
        if (letter) {
            fprintf(f, "\\%c", letter);
        } else if ((unsigned char)*s < 32) {
            fprintf(f, "\\u%04x", (unsigned char)*s);
        } else {
            fputc(*s, f);
        }
    }
}

int hold_write_json_argv(FILE *f, int argc, char **argv) {
    fputs("[", f);
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputs(", ", f);
        fputc('"', f);
        hold_json_escape(f, argv[i]);
        fputc('"', f);
    }
    fputs("]", f);
    return 0;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* THE string scanner, one codepoint loop with three modes: skip (out and lit
 * NULL), parse into out (BMP-only; \u escapes become UTF-8, raw bytes copy
 * verbatim), or match against the ASCII literal lit. Every mode rejects raw
 * control bytes, escaped NUL and surrogate escapes. */
static int scan_string(const char **pp, char *out, size_t n, const char *lit, bool *matched) {
    const char *p = *pp;
    if (*p != '"') return -1;
    p++;
    size_t i = 0, li = 0;
    bool ok = true;
    while (*p) {
        if (*p == '"') {
            if (out) {
                if (i >= n) return -1;
                out[i] = '\0';
            }
            if (matched) *matched = ok && (!lit || lit[li] == '\0');
            *pp = p + 1;
            return 0;
        }
        unsigned v;
        bool from_u = false;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': v = '\n'; break;
            case 't': v = '\t'; break;
            case 'r': v = '\r'; break;
            case 'b': v = '\b'; break;
            case 'f': v = '\f'; break;
            case '\\': case '"': case '/': v = (unsigned char)*p; break;
            case 'u':
                v = 0;
                for (int j = 0; j < 4; j++) {
                    p++;
                    if (!isxdigit((unsigned char)*p)) return -1;
                    v = (v << 4) + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                }
                if (v == 0 || (v >= 0xD800 && v <= 0xDFFF)) return -1;
                from_u = true;
                break;
            default:
                return -1;
            }
            p++;
        } else {
            if ((unsigned char)*p < 0x20) return -1;
            v = (unsigned char)*p;
            p++;
        }
        if (out) {
            /* \u escapes become UTF-8 (1-3 bytes, BMP); raw bytes copy verbatim. */
            size_t need = (!from_u || v <= 0x7F) ? 1 : (v <= 0x7FF ? 2 : 3);
            if (i + need >= n) return -1;
            if (need == 1) out[i++] = (char)v;
            if (need == 3) out[i++] = (char)(0xE0 | (v >> 12));
            if (need == 2) out[i++] = (char)(0xC0 | (v >> 6));
            if (need == 3) out[i++] = (char)(0x80 | ((v >> 6) & 0x3F));
            if (need >= 2) out[i++] = (char)(0x80 | (v & 0x3F));
        }
        if (lit) {
            if (v > 0x7F || lit[li] == '\0' || (unsigned char)lit[li] != v) {
                ok = false;
            } else {
                li++;
            }
        }
    }
    return -1;
}

int hold_parse_json_string(const char *p, char *out, size_t n, const char **endp) {
    const char *q = p;
    if (scan_string(&q, out, n, NULL, NULL) != 0) return -1;
    if (endp) *endp = q;
    return 0;
}

static int skip_json_value(const char **pp, int depth) {
    if (depth > JSON_MAX_DEPTH) {
        errno = EINVAL;
        return -1;
    }
    const char *p = skip_ws(*pp);
    if (*p == '"') {
        if (scan_string(&p, NULL, 0, NULL, NULL) != 0) return -1;
        *pp = p;
        return 0;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        p++;
        while (*p) {
            p = skip_ws(p);
            if (*p == close) {
                *pp = p + 1;
                return 0;
            }
            if (open == '{') {
                if (scan_string(&p, NULL, 0, NULL, NULL) != 0) return -1;
                p = skip_ws(p);
                if (*p != ':') return -1;
                p++;
            }
            if (skip_json_value(&p, depth + 1) != 0) return -1;
            p = skip_ws(p);
            if (*p == ',') p++;
        }
        return -1;
    }
    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '}' && *p != ']') p++;
    *pp = p;
    return 0;
}

int hold_json_find_key(const char *j, const char *k, const char **v) {
    const char *p = skip_ws(j);
    if (*p != '{') return -1;
    p++;
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') return -1;
        bool key_match = false;
        if (scan_string(&p, NULL, 0, k, &key_match) != 0) return -1;
        p = skip_ws(p);
        if (*p != ':') return -1;
        p = skip_ws(p + 1);
        if (key_match) {
            *v = p;
            return 0;
        }
        if (skip_json_value(&p, 0) != 0) return -1;
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    return -1;
}

/* Strict scalar terminator: only ws then end-of-value punctuation may follow. */
static bool value_end_ok(const char *end) {
    end = skip_ws(end);
    return *end == '\0' || *end == ',' || *end == '}' || *end == ']';
}

int hold_json_get_i64(const char *j, const char *k, int64_t *out) {
    const char *v;
    if (hold_json_find_key(j, k, &v) != 0 || *v == '+') return -1;
    char *end = NULL;
    errno = 0;
    long long x = strtoll(v, &end, 10);
    if (end == v || errno != 0 || !value_end_ok(end)) return -1;
    *out = x;
    return 0;
}

int hold_json_get_u64(const char *j, const char *k, uint64_t *out) {
    const char *v;
    if (hold_json_find_key(j, k, &v) != 0 || *v == '+' || *v == '-') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long x = strtoull(v, &end, 10);
    if (end == v || errno != 0 || !value_end_ok(end)) return -1;
    *out = x;
    return 0;
}

int hold_json_get_bool(const char *j, const char *k, bool *out) {
    const char *v;
    if (hold_json_find_key(j, k, &v) != 0) return -1;
    v = skip_ws(v);
    if (strncmp(v, "true", 4) == 0 && value_end_ok(v + 4)) {
        *out = true;
        return 0;
    }
    if (strncmp(v, "false", 5) == 0 && value_end_ok(v + 5)) {
        *out = false;
        return 0;
    }
    return -1;
}

int hold_json_get_str(const char *j, const char *k, char *out, size_t n) {
    const char *v;
    if (hold_json_find_key(j, k, &v) != 0) return -1;
    return hold_parse_json_string(skip_ws(v), out, n, NULL);
}

void hold_free_argv_alloc(char **argv, int argc) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
}

/* THE array walk: materializes a non-empty JSON string array. */
static int get_string_array_alloc(const char *j, const char *key, char ***argv_out, int *argc_out) {
    *argv_out = NULL;
    *argc_out = 0;
    const char *v;
    if (hold_json_find_key(j, key, &v) != 0 || *v != '[') return -1;
    v = skip_ws(v + 1);
    int cap = 4, argc = 0;
    char **argv = calloc((size_t)cap + 1, sizeof(char *));
    if (!argv) return -1;
    while (*v && *v != ']') {
        char arg[HOLD_PATH_MAX];
        if (hold_parse_json_string(v, arg, sizeof(arg), &v) != 0) goto fail;
        if (argc == cap) {
            cap *= 2;
            char **next = realloc(argv, ((size_t)cap + 1) * sizeof(char *));
            if (!next) goto fail;
            argv = next;
        }
        argv[argc] = strdup(arg);
        if (!argv[argc]) goto fail;
        argv[++argc] = NULL;
        v = skip_ws(v);
        if (*v == ',') {
            v = skip_ws(v + 1);
        } else if (*v != ']') {
            goto fail;
        }
    }
    if (*v != ']' || argc == 0) goto fail;
    *argv_out = argv;
    *argc_out = argc;
    return 0;
fail:
    hold_free_argv_alloc(argv, argc);
    return -1;
}

int hold_json_get_argv_alloc(const char *j, char ***argv_out, int *argc_out) {
    return get_string_array_alloc(j, "argv", argv_out, argc_out);
}

int hold_json_get_env_alloc(const char *j, char ***env_out, int *envc_out) {
    return get_string_array_alloc(j, "env", env_out, envc_out);
}

int hold_json_get_argv_display(const char *j, char *out, size_t n) {
    char **argv = NULL;
    int argc = 0;
    if (hold_json_get_argv_alloc(j, &argv, &argc) != 0) return -1;
    int r = hold_format_argv_human(out, n, argc, argv);
    hold_free_argv_alloc(argv, argc);
    return r;
}
