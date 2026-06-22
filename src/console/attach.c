#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/console.h"
#include "sigmund/core.h"
#include "sigmund/console_internal.h"

int run_native_console(const char *sock_path) {
    int sock = connect_console_socket(sock_path);
    if (sock < 0) {
        return errno == ENOTSOCK || errno == ENAMETOOLONG ? 5 : 3;
    }

    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    bool terminal_saved = false;
    bool alt_screen = false;
    struct termios old_termios;
    if (interactive && tcgetattr(STDIN_FILENO, &old_termios) == 0) {
        struct termios raw;
        make_raw_termios(&old_termios, &raw);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
            terminal_saved = true;
            if (write_all(STDOUT_FILENO, "\033[?1049h\033[H\033[2J", 15) == 0) {
                alt_screen = true;
            }
        }
    }

    struct sigaction sa, old_winch;
    bool have_old_winch = false;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_console_sigwinch;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGWINCH, &sa, &old_winch) == 0) {
        have_old_winch = true;
    }
    struct sigaction pipe_ign, old_pipe;
    bool have_old_pipe = false;
    memset(&pipe_ign, 0, sizeof(pipe_ign));
    pipe_ign.sa_handler = SIG_IGN;
    sigemptyset(&pipe_ign.sa_mask);
    if (sigaction(SIGPIPE, &pipe_ign, &old_pipe) == 0) {
        have_old_pipe = true;
    }
    g_console_resized = 1;

    int rc = 0;
    bool stdin_open = true;
    if (write_all(sock, CONSOLE_ATTACH_MAGIC, CONSOLE_ATTACH_MAGIC_LEN) != 0) {
        rc = 3;
        goto out;
    }

    while (1) {
        if (g_console_resized) {
            struct winsize ws;
            g_console_resized = 0;
            if (maybe_get_terminal_size(&ws) == 0 && send_console_resize(sock, &ws) != 0) {
                rc = 3;
                break;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        int maxfd = sock;
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &rfds);
            if (STDIN_FILENO > maxfd) {
                maxfd = STDIN_FILENO;
            }
        }

        int sr = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (sr < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = 3;
            break;
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &rfds)) {
            unsigned char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                if (interactive) {
                    size_t write_start = 0;
                    for (ssize_t i = 0; i < n; i++) {
                        if (buf[i] != CONSOLE_ATTACH_DETACH) {
                            continue;
                        }
                        if ((size_t)i > write_start &&
                            write_console_frame(sock, CONSOLE_FRAME_DATA, buf + write_start, (uint16_t)((size_t)i - write_start)) != 0) {
                            rc = 3;
                        }
                        if (rc == 0 && write_console_frame(sock, CONSOLE_FRAME_DETACH, NULL, 0) != 0) {
                            rc = 3;
                        }
                        goto out;
                    }
                    if (rc != 0) {
                        break;
                    }
                    if (write_console_frame(sock, CONSOLE_FRAME_DATA, buf, (uint16_t)n) != 0) {
                        rc = 3;
                        break;
                    }
                } else if (write_console_frame(sock, CONSOLE_FRAME_DATA, buf, (uint16_t)n) != 0) {
                    rc = 3;
                    break;
                }
            } else if (n == 0) {
                stdin_open = false;
                shutdown(sock, SHUT_WR);
            } else if (errno != EINTR) {
                rc = 3;
                break;
            }
        }

        if (FD_ISSET(sock, &rfds)) {
            char buf[4096];
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n > 0) {
                if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
                    rc = 3;
                    break;
                }
            } else if (n == 0) {
                break;
            } else if (errno != EINTR) {
                rc = 3;
                break;
            }
        }
    }

out:
    if (have_old_winch) {
        sigaction(SIGWINCH, &old_winch, NULL);
    }
    if (have_old_pipe) {
        sigaction(SIGPIPE, &old_pipe, NULL);
    }
    if (alt_screen) {
        (void)write_all(STDOUT_FILENO, "\033[?1049l", 8);
    }
    if (terminal_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
    }
    close(sock);
    return rc;
}
