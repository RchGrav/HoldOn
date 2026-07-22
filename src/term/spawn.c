#include "hold/config.h"
#include "hold/core.h"
#include "hold/term.h"

/* THE spawn engine. Every hold process that puts a child on a PTY — console
 * broker target, `hold on` shell — goes through hold_term_pty_spawn; there is
 * no second copy of the fork/setsid/TIOCSCTTY/dup2/exec ladder or of the
 * errno handshake anywhere else. */

/* Open a PTY pair. The master is O_CLOEXEC; the slave is opened O_NOCTTY so
 * the calling process (often a session leader without a controlling terminal)
 * never adopts the PTY as its own — the spawned child claims it explicitly
 * via setsid + TIOCSCTTY, scoping terminal-generated signals (Ctrl-C, etc.)
 * and the foreground process group to the child. The PTY gets a real window
 * size before anyone execs on it: the kernel default of 0x0 makes
 * shells/readline and TUIs misrender and appear to drop keystrokes, so absent
 * dimensions fall back to 80x24 (e.g. a detached `-d -t` start). */
static int term_open_pty(int *master_out, int *slave_out,
                         unsigned short rows, unsigned short cols) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        return -1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    char *slave_name = ptsname(master);
    if (!slave_name) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    int slave = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = rows ? rows : 24;
    ws.ws_col = cols ? cols : 80;
    (void)ioctl(master, TIOCSWINSZ, &ws);
    *master_out = master;
    *slave_out = slave;
    return 0;
}

static int term_cloexec_pipe(int fds[2]) {
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(fds, O_CLOEXEC) == 0) {
        return 0;
    }
#endif
    /* Non-Linux builds set FD_CLOEXEC after pipe(); unlike pipe2(O_CLOEXEC),
     * that is not atomic with respect to concurrent fork/exec in a
     * multi-threaded process. hold is single-threaded at every spawn site, so
     * the fallback is acceptable. */
    if (pipe(fds) != 0) {
        return -1;
    }
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        close(fds[0]);
        close(fds[1]);
        errno = saved;
        return -1;
    }
    return 0;
}

/* Child side: any pre-exec failure rides the handshake pipe as an errno,
 * then _exit(127) — never exit(), never a return into the caller's stack. */
static _Noreturn void term_child_fail(int handshake_fd) {
    int e = errno;
    (void)hold_write_all(handshake_fd, &e, sizeof(e));
    _exit(127);
}

int hold_term_pty_spawn(const struct hold_term_spawn *spec,
                        int *master_out, pid_t *pid_out) {
    if (!spec || !spec->argv || !spec->argv[0] || !master_out || !pid_out) {
        errno = EINVAL;
        return -1;
    }
    int master = -1;
    int slave = -1;
    if (term_open_pty(&master, &slave, spec->rows, spec->cols) != 0) {
        return -1;
    }
    int hs[2];
    if (term_cloexec_pipe(hs) != 0) {
        int saved = errno;
        close(master);
        close(slave);
        errno = saved;
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(hs[0]);
        close(hs[1]);
        close(master);
        close(slave);
        errno = saved;
        return -1;
    }
    if (pid == 0) {
        /* Every hold-owned inherited fd (master, handshake read end, caller
         * sockets/logs) is CLOEXEC, so exec sheds them; only the slave must
         * survive as stdio. */
        if (spec->cwd && *spec->cwd && chdir(spec->cwd) != 0) {
            term_child_fail(hs[1]);
        }
        /* Become a session leader and claim the PTY slave as the controlling
         * terminal, so Ctrl-C interrupts the child (not its parent) and
         * interactive shells can run job control. */
        if (setsid() < 0) {
            term_child_fail(hs[1]);
        }
#ifdef TIOCSCTTY
        if (ioctl(slave, TIOCSCTTY, 0) != 0) {
            term_child_fail(hs[1]);
        }
#endif
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) {
            term_child_fail(hs[1]);
        }
        if (slave > STDERR_FILENO) {
            close(slave);
        }
        if (spec->exec_path && *spec->exec_path) {
            execv(spec->exec_path, spec->argv);
        } else {
            execvp(spec->argv[0], spec->argv);
        }
        term_child_fail(hs[1]);
    }

    close(hs[1]);
    int child_errno = 0;
    int handshake = hold_read_exec_handshake(hs[0], &child_errno);
    int read_errno = errno;
    close(hs[0]);
    close(slave);
    if (handshake != 0) {
        /* handshake < 0: our read failed and the child's fate is unknown —
         * kill it. handshake > 0: the child reported errno and _exit(127)ed;
         * the kill is a no-op on the corpse and the waitpid reaps it. */
        kill(pid, SIGKILL);
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
        close(master);
        errno = handshake > 0 ? (child_errno ? child_errno : EIO) : read_errno;
        return -1;
    }
    *master_out = master;
    *pid_out = pid;
    return 0;
}
