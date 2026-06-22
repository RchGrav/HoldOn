#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/core.h"

bool valid_id(const char *id) {
    size_t len = strlen(id);
    if (len != ID_HEX_LEN) {
        return false;
    }
    if (strcmp(id, "00000000") == 0 || strcmp(id, "ffffffff") == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)id[i]) && !(id[i] >= 'a' && id[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

bool record_json_filename_id(const char *name, char *id, size_t n) {
    if (!name || !has_suffix(name, ".json")) {
        return false;
    }
    size_t len = strlen(name);
    size_t id_len = len - 5;
    if (id_len + 1 > n) {
        return false;
    }
    memcpy(id, name, id_len);
    id[id_len] = '\0';
    return valid_id(id);
}

bool valid_id_prefix(const char *id) {
    size_t len = strlen(id);
    if (len < 1 || len > ID_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)id[i]) && !(id[i] >= 'a' && id[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

bool valid_profile_hash(const char *hash) {
    if (!hash || strlen(hash) != PROFILE_HASH_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < PROFILE_HASH_HEX_LEN; i++) {
        if (!isdigit((unsigned char)hash[i]) && !(hash[i] >= 'a' && hash[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

bool valid_alias(const char *alias) {
    if (!alias) {
        return false;
    }
    size_t len = strlen(alias);
    if (len == 0 || len > ALIAS_MAX_LEN || valid_profile_hash(alias)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)alias[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool valid_record(const struct sigmund_run_record *r) {
    return r->pid > 0 && r->pgid > 1 && r->id[0] != '\0';
}

int parse_uid_env(const char *s, uid_t *out) {
    if (!s || !*s) {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (uid_t)v;
    return 0;
}

int parse_gid_env(const char *s, gid_t *out) {
    if (!s || !*s) {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (gid_t)v;
    return 0;
}

bool valid_runid_selector(const char *sel) {
    return sel && (valid_id(sel) || strcmp(sel, "00000000") == 0 || strcmp(sel, "ffffffff") == 0);
}

bool valid_target_atom(const char *id) {
    return valid_id_prefix(id) || valid_alias(id);
}
