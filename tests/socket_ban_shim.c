/* LD_PRELOAD shim for tests: deny Unix-domain sockets with EPERM, as some
 * sandboxes do (ChatGPT's code environment refuses socket(AF_UNIX)), so the
 * FIFO console's automatic fallback runs for real. Linux, dynamic builds:
 *   cc -shared -fPIC -o socket-ban.so tests/socket_ban_shim.c -ldl
 *   LD_PRELOAD=$PWD/socket-ban.so HOLD_TEST_SOCKETS_DENIED=1 bash tests/test_hold.sh */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sys/socket.h>

int socket(int domain, int type, int protocol) {
    static int (*real_socket)(int, int, int);
    if (domain == AF_UNIX) {
        errno = EPERM;
        return -1;
    }
    if (!real_socket) real_socket = (int (*)(int, int, int))dlsym(RTLD_NEXT, "socket");
    return real_socket(domain, type, protocol);
}
