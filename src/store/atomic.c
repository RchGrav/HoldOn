#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "store_internal.h"

/* The store's only commit tail: unique temp in the SAME dir -> emit ->
 * fflush -> fsync(fd) -> close -> rename -> fsync(dir). Everything before
 * the dir fsync is fatal; the dir fsync itself only warns. The temp file
 * is unlinked on every failure path, and fchmod defends against umask. */
int hold_atomic_write_json(const char *dir, const char *final_name, mode_t mode,
                           bool chown_root, hold_json_emit_fn emit, void *ctx,
                           char *out_path, size_t out_n) {
    char tmp[HOLD_PATH_MAX], fin[HOLD_PATH_MAX];
    if (hold_checked_snprintf(fin, sizeof(fin), "%s/%s", dir, final_name) != 0) return -1;
    if (out_path && hold_checked_snprintf(out_path, out_n, "%s", fin) != 0) return -1;
    int rc = -1;
    FILE *f = NULL;
    int fd = hold_open_unique_temp(dir, final_name, mode, tmp, sizeof(tmp));
    if (fd < 0) return -1;
    if (fchmod(fd, mode) != 0) goto out;
    if (chown_root && geteuid() == 0 && fchown(fd, 0, 0) != 0) goto out;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        fd = -1;
        goto out;
    }
    if (emit(f, ctx) != 0 || ferror(f) || fflush(f) != 0 || fsync(fd) != 0) goto out;
    if (fclose(f) != 0) {
        f = NULL;
        goto out;
    }
    f = NULL;
    fd = -1;
    if (rename(tmp, fin) != 0) goto out;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        if (fsync(dfd) != 0)
            fprintf(stderr, "hold: warning: failed to fsync storage dir: %s\n", strerror(errno));
        close(dfd);
    }
    rc = 0;
out:
    if (f) fclose(f);
    else if (fd >= 0) close(fd);
    if (rc != 0) unlink(tmp);
    return rc;
}
