#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/term.h"
#include "hold/console_internal.h"

#include <poll.h>

/* Set by the SIGWINCH handler below; read by run_native_console in attach.c. */
volatile sig_atomic_t g_console_resized = 0;

/* The child-mode broker's forked target group, set right after the pid is
 * reported and cleared at every reap site. The SIGTERM forwarder reads it so a
 * TERM aimed at the broker (e.g. a raw kill of the broker pid) reaches the held
 * group instead of orphaning it. Never set by the adopted entry, which must
 * never signal its adopted group. */
static volatile pid_t g_broker_forward_target = 0;

static void broker_forward_term(int signo) {
    (void)signo;
    pid_t t = g_broker_forward_target;
    if (t > 1) {
        kill(-t, SIGTERM);
    }
}

static void broker_cleanup_and_exit(int parent_pipe,
                                    const char *sock_path,
                                    int listener,
                                    int master,
                                    int logfd,
                                    int logidxfd,
                                    pid_t target,
                                    int exit_code);
static void broker_fail_errno(int parent_pipe,
                              const char *sock_path,
                              int listener,
                              int master,
                              int logfd,
                              int logidxfd,
                              pid_t target,
                              int err);
static bool console_peer_uid_allowed(uid_t peer_uid,
                                     uid_t owner_uid,
                                     bool have_allowed_peer_uid,
                                     uid_t allowed_peer_uid);
static bool authorize_console_client(int client,
                                     uid_t owner_uid,
                                     bool have_allowed_peer_uid,
                                     uid_t allowed_peer_uid);
static int set_fd_nonblocking(int fd);
static void broker_serve(const struct hold_store *store,
                         const char *run_id,
                         const char *sock_path,
                         int listener,
                         int master,
                         int logfd,
                         int logidxfd,
                         uid_t owner_uid,
                         bool have_allowed_peer_uid,
                         uid_t allowed_peer_uid,
                         pid_t child_target,
                         pid_t adopted_pgid,
                         pid_t adopted_sid,
                         pid_t hup_pid);

void hold_handle_console_sigwinch(int signo) {
    (void)signo;
    g_console_resized = 1;
}

static void broker_cleanup_and_exit(int parent_pipe,
                                    const char *sock_path,
                                    int listener,
                                    int master,
                                    int logfd,
                                    int logidxfd,
                                    pid_t target,
                                    int exit_code) {
    if (target > 0) {
        kill(target, SIGKILL);
        int st = 0;
        while (waitpid(target, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
    if (parent_pipe >= 0) close(parent_pipe);
    if (listener >= 0) close(listener);
    if (master >= 0) close(master);
    if (logidxfd >= 0) close(logidxfd);
    if (logfd >= 0) close(logfd);
    if (sock_path && *sock_path) unlink(sock_path);
    _exit(exit_code);
}

static void broker_fail_errno(int parent_pipe,
                              const char *sock_path,
                              int listener,
                              int master,
                              int logfd,
                              int logidxfd,
                              pid_t target,
                              int err) {
    if (err == 0) {
        err = EIO;
    }
    (void)hold_write_all(parent_pipe, &err, sizeof(err));
    broker_cleanup_and_exit(parent_pipe, sock_path, listener, master, logfd, logidxfd, target, 127);
}

static int set_fd_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    return 0;
}

static bool console_peer_uid_allowed(uid_t peer_uid,
                                     uid_t owner_uid,
                                     bool have_allowed_peer_uid,
                                     uid_t allowed_peer_uid) {
    return peer_uid == 0 || peer_uid == owner_uid || (have_allowed_peer_uid && peer_uid == allowed_peer_uid);
}

static bool authorize_console_client(int client,
                                     uid_t owner_uid,
                                     bool have_allowed_peer_uid,
                                     uid_t allowed_peer_uid) {
    uid_t peer_uid = (uid_t)-1;
    if (hold_console_peer_uid(client, &peer_uid) != 0 ||
        !console_peer_uid_allowed(peer_uid, owner_uid, have_allowed_peer_uid, allowed_peer_uid)) {
        static const char msg[] = "hold: console attach denied\n";
        (void)hold_write_all(client, msg, sizeof(msg) - 1);
        return false;
    }
    return true;
}

void hold_run_console_broker(int parent_pipe,
                               int target_pid_fd,
                               const struct hold_store *store,
                               const char *run_id,
                               const char *log_path,
                               const char *sock_path,
                               uid_t owner_uid,
                               bool have_allowed_peer_uid,
                               uid_t allowed_peer_uid,
                               int argc,
                               char **argv,
                               const char *exec_path,
                               unsigned short init_rows,
                               unsigned short init_cols) {
    int listener = -1;
    int master = -1;
    int logfd = -1;
    int logidxfd = -1;
    pid_t target = -1;

    if (argc <= 0 || !argv || !argv[0]) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, logfd, logidxfd, target, EINVAL);
    }

    listener = hold_make_console_listener(sock_path);
    if (listener < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, logfd, logidxfd, target, errno);
    }
    logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    logidxfd = logfd >= 0 ? hold_open_log_index_fd(log_path, logfd) : -1;
    if (logfd < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, logfd, logidxfd, target, errno);
    }

    /* The one spawn engine puts the target on a fresh PTY (setsid + TIOCSCTTY
     * + dup2 x3 + exec behind the errno handshake). On failure nothing is
     * left open or unreaped, and the errno rides our own handshake pipe up to
     * the launching parent. The broker's listener/log fds are all CLOEXEC, so
     * the target never inherits them past exec. */
    struct hold_term_spawn spawn = {
        .argv = argv,
        .exec_path = exec_path,
        .cwd = NULL,
        .rows = init_rows,
        .cols = init_cols,
    };
    if (hold_term_pty_spawn(&spawn, &master, &target) != 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, logfd, logidxfd, target, errno);
    }
    /* Report the real held group to the parent before releasing the handshake:
     * a failed write must still ride the handshake pipe as an errno, and the
     * write-before-close ordering keeps the parent's pid read deadlock-free. */
    if (target_pid_fd >= 0) {
        if (hold_write_all(target_pid_fd, &target, sizeof(target)) != 0) {
            broker_fail_errno(parent_pipe, sock_path, listener, master, logfd, logidxfd, target, errno);
        }
        close(target_pid_fd);
    }
    close(parent_pipe);
    parent_pipe = -1;

    /* Forward a TERM aimed at the broker to the held group (no SA_RESTART so the
     * serve loop's poll/waitpid see EINTR). The handler is async-signal-safe and
     * also unblocks a TERM that arrives during the post-loop blocking waitpid. */
    g_broker_forward_target = target;
    struct sigaction term_sa;
    memset(&term_sa, 0, sizeof(term_sa));
    term_sa.sa_handler = broker_forward_term;
    sigemptyset(&term_sa.sa_mask);
    (void)sigaction(SIGTERM, &term_sa, NULL);

    broker_serve(store, run_id, sock_path, listener, master, logfd, logidxfd,
                 owner_uid, have_allowed_peer_uid, allowed_peer_uid,
                 target, 0, 0, -1);
}

/* Log-only fallback for an adopted PTY when no broker socket could be
 * provisioned: drain the master through the one pump until the adopted side
 * is gone, then hang up the wrapper shell. */
static void adopted_log_only_pump(int master, const char *log_path, pid_t hup_pid,
                                  pid_t adopted_pgid, pid_t adopted_sid) {
    int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) _exit(1);
    int idxfd = hold_open_log_index_fd(log_path, fd);
    while (1) {
        struct pollfd pfd = {
            .fd = master,
            .events = POLLIN,
            .revents = 0,
        };
        int ready;
        do {
            ready = poll(&pfd, 1, 200);
        } while (ready < 0 && errno == EINTR);
        if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            ssize_t n = hold_term_pump_master(master, fd, idxfd, buf, sizeof(buf));
            if (n == 0 || (n < 0 && errno != EINTR)) {
                break;
            }
        }
        if (adopted_pgid > 1 && adopted_sid > 0 &&
            hold_group_session_liveness(adopted_pgid, adopted_sid) != GROUP_LIVE) {
            break;
        }
    }
    kill(hup_pid, SIGHUP);
    if (idxfd >= 0) close(idxfd);
    close(fd);
    close(master);
    _exit(0);
}

pid_t hold_spawn_adopted_console_server(const struct hold_store *store,
                                          const char *run_id,
                                          const char *log_path,
                                          int master,
                                          pid_t adopted_pgid,
                                          pid_t adopted_sid,
                                          pid_t hup_pid,
                                          char *console_sock_out,
                                          size_t console_sock_n) {
    /* Prefer serving the adopted PTY through a broker so the run stays
     * reattachable; every fd is opened here so the child has no failure path.
     * If broker setup fails, fall back to the log-only capture. */
    char console_sock[HOLD_PATH_MAX];
    console_sock[0] = '\0';
    int listener = -1, logfd = -1, logidxfd = -1;
    if (hold_format_console_sock_path(store, run_id, console_sock, sizeof(console_sock)) == 0) {
        listener = hold_make_console_listener(console_sock);
        if (listener < 0) {
            console_sock[0] = '\0';
        } else {
            logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
            logidxfd = logfd >= 0 ? hold_open_log_index_fd(log_path, logfd) : -1;
            if (logfd < 0) {
                close(listener);
                listener = -1;
                unlink(console_sock);
                console_sock[0] = '\0';
            }
        }
    } else {
        console_sock[0] = '\0';
    }

    pid_t server = fork();
    if (server < 0) {
        int saved = errno;
        if (listener >= 0) {
            close(listener);
            if (logidxfd >= 0) close(logidxfd);
            if (logfd >= 0) close(logfd);
            unlink(console_sock);
        }
        errno = saved;
        return -1;
    }
    if (server == 0) {
        signal(SIGHUP, SIG_IGN);
        hold_close_stdio_to_devnull();
        if (listener >= 0) {
            broker_serve(store, run_id, console_sock, listener, master, logfd, logidxfd,
                         geteuid(), false, 0, -1, adopted_pgid, adopted_sid, hup_pid);
            _exit(0);
        }
        adopted_log_only_pump(master, log_path, hup_pid, adopted_pgid, adopted_sid);
    }
    if (listener >= 0) {
        close(listener);
        if (logidxfd >= 0) close(logidxfd);
        if (logfd >= 0) close(logfd);
    }
    if (hold_checked_snprintf(console_sock_out, console_sock_n, "%s", console_sock) != 0) {
        console_sock_out[0] = '\0';
    }
    return server;
}

/* Shared serve loop. child_target > 0 means the target is our child (waitpid
 * lifecycle, killed on cleanup); otherwise the target is an adopted foreground
 * group we must never kill, whose exit shows as PTY EOF or dead group/session. */
static void broker_serve(const struct hold_store *store,
                         const char *run_id,
                         const char *sock_path,
                         int listener,
                         int master,
                         int logfd,
                         int logidxfd,
                         uid_t owner_uid,
                         bool have_allowed_peer_uid,
                         uid_t allowed_peer_uid,
                         pid_t child_target,
                         pid_t adopted_pgid,
                         pid_t adopted_sid,
                         pid_t hup_pid) {
    pid_t target = child_target;
    bool adopted = child_target <= 0;

    struct sigaction pipe_ign, old_pipe;
    bool have_old_pipe = false;
    memset(&pipe_ign, 0, sizeof(pipe_ign));
    pipe_ign.sa_handler = SIG_IGN;
    sigemptyset(&pipe_ign.sa_mask);
    if (sigaction(SIGPIPE, &pipe_ign, &old_pipe) == 0) {
        have_old_pipe = true;
    }

    int client = -1;
    bool client_input_closed = false;
    struct console_client_state client_state;
    memset(&client_state, 0, sizeof(client_state));
    struct console_replay_buffer replay;
    hold_console_replay_init(&replay);
    bool target_done = false;
    bool target_marked = false;
    int target_status = 0;
    while (1) {
        if (!adopted && !target_done) {
            int st = 0;
            pid_t got = waitpid(target, &st, WNOHANG);
            if (got == target) {
                target_done = true;
                target_status = st;
                if (store && run_id && *run_id) {
                    /* Best-effort while serving: an instantly-exiting target
                     * can beat the parent's record write, so a failure here
                     * is retried patiently after the serve loop ends. */
                    target_marked = hold_mark_run_finished(store, run_id, target_status) == 0;
                }
                target = -1;
                g_broker_forward_target = 0;
            }
        }
        if (adopted && !target_done && adopted_pgid > 1 && adopted_sid > 0 &&
            hold_group_session_liveness(adopted_pgid, adopted_sid) != GROUP_LIVE) {
            target_done = true;
        }

        struct pollfd pfds[3];
        nfds_t nfds = 0;
        nfds_t master_idx = nfds;
        pfds[nfds++] = (struct pollfd){.fd = master, .events = POLLIN};
        nfds_t listener_idx = nfds;
        pfds[nfds++] = (struct pollfd){.fd = listener, .events = POLLIN};
        nfds_t client_idx = 0;
        bool poll_client = client >= 0 && !client_input_closed;
        if (poll_client) {
            client_idx = nfds;
            pfds[nfds++] = (struct pollfd){.fd = client, .events = POLLIN};
        }

        int pr = poll(pfds, nfds, 1000);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr == 0) {
            if (target_done) {
                break;
            }
            continue;
        }

        short listener_events = pfds[listener_idx].revents;
        short client_events = poll_client ? pfds[client_idx].revents : 0;
        short master_events = pfds[master_idx].revents;

        if (listener_events & POLLIN) {
            int next = accept(listener, NULL, NULL);
            if (next >= 0) {
                if (set_fd_nonblocking(next) != 0) {
                    close(next);
                } else if (!authorize_console_client(next, owner_uid, have_allowed_peer_uid, allowed_peer_uid)) {
                    close(next);
                } else if (client >= 0) {
                    static const char msg[] = "hold: console already attached\n";
                    (void)hold_write_all(next, msg, sizeof(msg) - 1);
                    close(next);
                } else if (hold_console_replay_write(&replay, next) != 0) {
                    close(next);
                } else {
                    client = next;
                    client_input_closed = false;
                    memset(&client_state, 0, sizeof(client_state));
                }
            }
        }
        if (client >= 0 && !client_input_closed &&
            (client_events & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            unsigned char buf[4096];
            ssize_t n = read(client, buf, sizeof(buf));
            if (n > 0) {
                int input_rc = hold_broker_process_client_input(&client_state, master, buf, (size_t)n);
                if (input_rc != 0) {
                    close(client);
                    client = -1;
                    client_input_closed = false;
                    memset(&client_state, 0, sizeof(client_state));
                }
            } else if (n == 0) {
                if (!client_state.decided && client_state.pending_len > 0) {
                    (void)hold_write_all(master, client_state.pending, client_state.pending_len);
                    client_state.pending_len = 0;
                }
                client_input_closed = true;
            } else {
                close(client);
                client = -1;
                client_input_closed = false;
                memset(&client_state, 0, sizeof(client_state));
            }
        }
        if (master_events & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            char buf[4096];
            ssize_t n = hold_term_pump_master(master, logfd, logidxfd, buf, sizeof(buf));
            if (n > 0) {
                hold_console_replay_append(&replay, buf, (size_t)n);
                if (client >= 0 && hold_write_all(client, buf, (size_t)n) != 0) {
                    close(client);
                    client = -1;
                    client_input_closed = false;
                }
            } else if (n == 0) {
                break;
            }
        }
    }

    if (!adopted && !target_done && target > 0) {
        int st = 0;
        pid_t got;
        do {
            got = waitpid(target, &st, 0);
        } while (got < 0 && errno == EINTR);
        if (got == target) {
            target_done = true;
            target_status = st;
            target = -1;
            g_broker_forward_target = 0;
        } else if (got < 0 && errno == ECHILD) {
            target = -1;
            g_broker_forward_target = 0;
        }
    }
    if (!adopted && target_done && !target_marked && store && run_id && *run_id) {
        /* The exit stamp must not be lost to the launch race: retry until the
         * parent's record exists. rc 1 means the record was purged while the
         * call was exiting — that removal is final, do not resurrect it. */
        for (int i = 0; i < 50; i++) {
            int mark_rc = hold_mark_run_finished(store, run_id, target_status);
            if (mark_rc == 0 || mark_rc == 1) break;
            struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
            while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
                continue;
            }
        }
    }
    if (adopted && hup_pid > 0) {
        kill(hup_pid, SIGHUP);
    }

    if (client >= 0) close(client);
    hold_console_replay_free(&replay);
    if (have_old_pipe) {
        sigaction(SIGPIPE, &old_pipe, NULL);
    }
    broker_cleanup_and_exit(-1, sock_path, listener, master, logfd, logidxfd, adopted ? -1 : target, 0);
}
