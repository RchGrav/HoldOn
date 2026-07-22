#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"

static bool all_lower_hex(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i]) && !(s[i] >= 'a' && s[i] <= 'f')) return false;
    }
    return true;
}

bool hold_valid_id(const char *id) {
    return id && strlen(id) == ID_HEX_LEN && all_lower_hex(id, ID_HEX_LEN);
}

bool hold_record_json_filename_id(const char *name, char *id, size_t n) {
    if (!name || !hold_has_suffix(name, ".json")) return false;
    size_t id_len = strlen(name) - 5;
    if (id_len + 1 > n) return false;
    memcpy(id, name, id_len);
    id[id_len] = '\0';
    return hold_valid_id(id);
}

bool hold_valid_id_prefix(const char *id) {
    if (!id) return false;
    size_t len = strlen(id);
    return len >= 1 && len <= ID_HEX_LEN && all_lower_hex(id, len);
}

/* Validates a call name (the identifier kept for its own sake, not the deleted
 * profile-era alias). A name that looks like a full run id is rejected so
 * names can never shadow ids in resolution. */
bool hold_valid_alias(const char *alias) {
    if (!alias) return false;
    size_t len = strlen(alias);
    if (len == 0 || len > ALIAS_MAX_LEN || hold_valid_id(alias)) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)alias[i];
        if (!(isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

bool hold_valid_record(const struct hold_run_record *r) {
    return r && r->pid > 0 && r->pgid > 1 && hold_valid_id(r->id) &&
           (r->run_id[0] == '\0' || hold_valid_id(r->run_id));
}

static int parse_id_number_env(const char *s, unsigned long long *out) {
    if (!s || !*s) return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) return -1;
    *out = v;
    return 0;
}

int hold_parse_uid_env(const char *s, uid_t *out) {
    unsigned long long v = 0;
    if (parse_id_number_env(s, &v) != 0) return -1;
    uid_t narrowed = (uid_t)v;
    if ((unsigned long long)narrowed != v) return -1;
    *out = narrowed;
    return 0;
}

int hold_parse_gid_env(const char *s, gid_t *out) {
    unsigned long long v = 0;
    if (parse_id_number_env(s, &v) != 0) return -1;
    gid_t narrowed = (gid_t)v;
    if ((unsigned long long)narrowed != v) return -1;
    *out = narrowed;
    return 0;
}
