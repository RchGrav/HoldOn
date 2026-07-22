#pragma once
#ifndef HOLD_TERM_H
#define HOLD_TERM_H

/* term: the one PTY spawn engine and the one PTY-master pump. This layer
 * knows PTYs, processes-on-PTYs, and pumps — never records, sockets, or
 * commands (layer DAG: store -> term -> console). */

#include "hold/config.h"

struct hold_term_spawn {
    char *const *argv;     /* required: argv[0] non-NULL, NULL-terminated */
    const char *exec_path; /* pre-resolved binary => execv; NULL/"" => execvp(argv[0]) */
    const char *cwd;       /* chdir in the child before exec; NULL/"" => inherit */
    unsigned short rows;   /* initial PTY window size; 0x0 => 80x24 preset */
    unsigned short cols;
};

/* Spawns argv on a fresh PTY: opens the pair (nonzero winsize before exec),
 * forks, and in the child does setsid + TIOCSCTTY + dup2 x3 + exec, with any
 * pre-exec failure reported over the errno handshake (EOF = exec succeeded).
 * On success returns 0 with the PTY master and child pid; on any failure
 * returns -1 with errno set and nothing left open or unreaped. */
int hold_term_pty_spawn(const struct hold_term_spawn *spec,
                        int *master_out, pid_t *pid_out);

/* THE CLOEXEC pipe: pipe2(O_CLOEXEC) where available, else pipe() + FD_CLOEXEC
 * (acceptable: hold is single-threaded at every spawn site). Returns 0 with
 * fds[0]=read, fds[1]=write, or -1 with errno set and nothing left open. */
int hold_cloexec_pipe(int fds[2]);

/* One drain step of a PTY master: reads once into buf[n], appends the bytes
 * to the indexed log, and returns them for caller fan-out (client, replay
 * ring, tty). Returns >0 bytes pumped; 0 when the target side is gone (EOF,
 * or EIO from a failed read after the last slave closed); -1 on a retryable
 * read error (errno preserved). */
ssize_t hold_term_pump_master(int master, int logfd, int logidxfd,
                              char *buf, size_t n);

#endif /* HOLD_TERM_H */
