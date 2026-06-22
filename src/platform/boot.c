#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/platform.h"
#include "sigmund/core.h"

static int get_boot_id(char *buf, size_t n);

static int get_boot_id(char *buf, size_t n) {
    const char *path = getenv("SIGMUND_BOOT_ID_PATH");
    if (path && *path) {
        return read_file_trim(path, buf, n);
    }
    if (read_file_trim(SIGMUND_BOOT_ID_PATH, buf, n) == 0) {
        return 0;
    }
#if defined(__APPLE__)
    struct timeval boottime;
    size_t len = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0 && len == sizeof(boottime)) {
        snprintf(buf, n, "macos-%lld.%06d", (long long)boottime.tv_sec, boottime.tv_usec);
        return 0;
    }
#endif
    return -1;
}

bool current_boot_id(char *buf, size_t n) {
    if (n == 0) {
        return false;
    }
    buf[0] = '\0';
    return get_boot_id(buf, n) == 0 && buf[0] != '\0';
}
