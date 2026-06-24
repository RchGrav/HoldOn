#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"

static const char b64url_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";

int hold_base64url_encode(const unsigned char *in, size_t in_len, char *out, size_t out_n) {
    size_t need = ((in_len + 2) / 3) * 4;
    size_t rem = in_len % 3;
    if (rem) need -= (3 - rem);
    if (out_n < need + 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t oi = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        bool have2 = i + 1 < in_len;
        bool have3 = i + 2 < in_len;
        if (have2) v |= (unsigned)in[i + 1] << 8;
        if (have3) v |= (unsigned)in[i + 2];
        out[oi++] = b64url_alphabet[(v >> 18) & 0x3f];
        out[oi++] = b64url_alphabet[(v >> 12) & 0x3f];
        if (have2) out[oi++] = b64url_alphabet[(v >> 6) & 0x3f];
        if (have3) out[oi++] = b64url_alphabet[v & 0x3f];
    }
    out[oi] = '\0';
    return 0;
}

static int b64url_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int hold_base64url_decode(const char *in, unsigned char *out, size_t out_n, size_t *out_len) {
    if (!in || !out_len) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(in);
    if (len == 0 || len % 4 == 1) {
        errno = EINVAL;
        return -1;
    }
    size_t need = (len / 4) * 3;
    if (len % 4 == 2) need += 1;
    else if (len % 4 == 3) need += 2;
    if (out_n < need + 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t oi = 0;
    for (size_t i = 0; i < len; ) {
        int vals[4] = {0, 0, 0, 0};
        int have = 0;
        for (; have < 4 && i < len; have++, i++) {
            vals[have] = b64url_value(in[i]);
            if (vals[have] < 0) {
                errno = EINVAL;
                return -1;
            }
        }
        if (have < 2) {
            errno = EINVAL;
            return -1;
        }
        unsigned v = ((unsigned)vals[0] << 18) | ((unsigned)vals[1] << 12) |
                     ((unsigned)vals[2] << 6) | (unsigned)vals[3];
        out[oi++] = (unsigned char)((v >> 16) & 0xff);
        if (have >= 3) out[oi++] = (unsigned char)((v >> 8) & 0xff);
        if (have == 4) out[oi++] = (unsigned char)(v & 0xff);
        if (have < 4 && i != len) {
            errno = EINVAL;
            return -1;
        }
    }
    out[oi] = '\0';
    *out_len = oi;
    return 0;
}

int hold_sha256_file_hex(const char *path, char out[PROFILE_HASH_STR_LEN]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct sha256_ctx ctx;
    unsigned char digest[32];
    hold_sha256_init(&ctx);
    unsigned char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            hold_sha256_update(&ctx, buf, (size_t)n);
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (close(fd) != 0) return -1;
    hold_sha256_final(&ctx, digest);
    hold_hex_encode(digest, sizeof(digest), out, PROFILE_HASH_STR_LEN);
    return 0;
}
