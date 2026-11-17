#pragma once
#ifndef HOLD_CONSOLE_INTERNAL_H
#define HOLD_CONSOLE_INTERNAL_H

/* Console-private wire constants and the socket seam, shared by the broker
 * (serve side) and attach (client side) translation units. Not part of the
 * public console.h surface. */
#include "hold/config.h"

/* Wire protocol: a client that opens with the magic speaks 3-byte
 * type + be16-length frames; anything else is raw passthrough (decision D-5
 * keeps nc/socat attaches working). */
#define CONSOLE_ATTACH_MAGIC "holdv1\0\0"
#define CONSOLE_ATTACH_MAGIC_LEN 8
#define CONSOLE_FRAME_DATA 'D'
#define CONSOLE_FRAME_RESIZE 'W'
#define CONSOLE_FRAME_DETACH 'X'
#define CONSOLE_FRAME_HEADER_LEN 3

/* Control clients (hold peek/send/wait/resize) open with this magic, then
 * send one be32-length-prefixed text request; the broker answers with one
 * be32-length-prefixed reply ("ok ...\n" | "error ...\n" | "timeout ...\n",
 * optionally followed by a binary body) and closes. Requests:
 *   peek                         -> "ok\n" + HSNAP1 snapshot
 *   send\n<op>\n...              -> "ok seq=N [mouse=off]\n"
 *        ops: text <hex> | paste <hex> | key <name> | mouse <action> <btn> <row> <col>
 *   resize rows=R cols=C         -> "ok rows=R cols=C\n"
 *   wait [idle=MS] [since=SEQ] [match=<hex>] [regex=1] [exit=1] [timeout=MS]
 *                                -> "ok reason=idle|match|exit seq=N [exit_code=C]\n"
 *                                 | "ended reason=exit ..." | "timeout seq=N\n"
 *   msg [from=<hex>] [quiet=MS] [timeout=MS] [submit=0]\n<hex text>
 *                                -> "ok delivered verified=0|1\n" once typed in */
#define CONSOLE_CONTROL_MAGIC "holdctl1"
#define CONSOLE_CONTROL_MAGIC_LEN 8

static inline uint16_t console_load_be16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t console_load_be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline void console_store_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static inline void console_store_be16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

/* socket.c: the with_console_dir seam (bind/connect via a short relative
 * sun_path from inside the console directory) and peer-uid authn. */
int hold_make_console_listener(const char *sock_path);
int hold_console_peer_uid(int fd, uid_t *uid_out);
int hold_console_peer_pid(int fd, pid_t *pid_out);
int hold_connect_console_socket(const char *sock_path);

#endif /* HOLD_CONSOLE_INTERNAL_H */
