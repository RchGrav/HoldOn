#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"

/* Store layout: path resolution and table-driven creation. The permission
 * matrix is on-disk ABI — user stores 0700, system base+public 0755 root,
 * runs/logs/console 0700 root — and sudo-created user stores are chowned
 * back through ~/.local -> ~/.local/state -> base. */

static int ensure_dir(const char *path, mode_t mode, bool own_root) {
    if (hold_mkdir_p_mode(path, mode) != 0 || hold_chmod_dir_no_symlink(path, mode) != 0) return -1;
    return own_root ? hold_chown_dir_no_symlink_if_root(path, 0, 0) : 0;
}

static int chown_back(const char *path, uid_t uid, gid_t gid) {
    if (hold_chmod_dir_no_symlink(path, 0700) != 0) return -1;
    return hold_chown_dir_no_symlink_if_root(path, uid, gid);
}

int hold_init_user_store_from_home(const char *home, struct hold_store *store) {
    if (!home || !*home) {
        errno = EINVAL;
        return -1;
    }
    char resolved_home[HOLD_PATH_MAX];
    const char *base_home = home;
    if (realpath(home, resolved_home)) base_home = resolved_home;
    memset(store, 0, sizeof(*store));
    store->kind = STORE_USER_LOCAL;
    if (hold_checked_snprintf(store->base, sizeof(store->base), "%s/.local/state/hold", base_home) != 0 ||
        hold_checked_snprintf(store->record_dir, sizeof(store->record_dir), "%s", store->base) != 0 ||
        hold_checked_snprintf(store->log_dir, sizeof(store->log_dir), "%s", store->base) != 0 ||
        hold_checked_snprintf(store->console_dir, sizeof(store->console_dir), "%s/console", store->base) != 0)
        return -1;
    return 0;
}

int hold_ensure_user_store_for_current_user(struct hold_store *store) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "hold: error: HOME is not set\n");
        errno = EINVAL;
        return -1;
    }
    if (hold_init_user_store_from_home(home, store) != 0) return -1;
    if (ensure_dir(store->base, 0700, false) != 0) return -1;
    return ensure_dir(store->console_dir, 0700, false);
}

int hold_ensure_invoking_user_store(const struct hold_invocation *inv, struct hold_store *store) {
    if (!inv || !inv->have_sudo_user || !inv->invoking_home[0]) {
        errno = EINVAL;
        return -1;
    }
    char local_dir[HOLD_PATH_MAX], state_dir[HOLD_PATH_MAX];
    if (hold_init_user_store_from_home(inv->invoking_home, store) != 0) return -1;
    if (hold_checked_snprintf(local_dir, sizeof(local_dir), "%s/.local", inv->invoking_home) != 0 ||
        hold_checked_snprintf(state_dir, sizeof(state_dir), "%s/.local/state", inv->invoking_home) != 0)
        return -1;
    uid_t uid = inv->invoking_uid;
    gid_t gid = inv->invoking_gid;
    if (hold_mkdir_p_mode(store->base, 0700) != 0 ||
        chown_back(local_dir, uid, gid) != 0 ||
        chown_back(state_dir, uid, gid) != 0 ||
        chown_back(store->base, uid, gid) != 0 ||
        hold_mkdir_p_mode(store->console_dir, 0700) != 0 ||
        chown_back(store->console_dir, uid, gid) != 0)
        return -1;
    return 0;
}

int hold_init_system_store(struct hold_store *store) {
    const char *base = HOLD_SYSTEM_STATE_DIR;
#ifdef HOLD_TESTING
    const char *override = getenv("HOLD_TEST_SYSTEM_STATE_DIR");
    if (override && *override) base = override;
#endif
    memset(store, 0, sizeof(*store));
    store->kind = STORE_SYSTEM_MANAGED;
    if (hold_checked_snprintf(store->base, sizeof(store->base), "%s", base) != 0 ||
        hold_checked_snprintf(store->record_dir, sizeof(store->record_dir), "%s/runs", base) != 0 ||
        hold_checked_snprintf(store->log_dir, sizeof(store->log_dir), "%s/logs", base) != 0 ||
        hold_checked_snprintf(store->public_dir, sizeof(store->public_dir), "%s/public", base) != 0 ||
        hold_checked_snprintf(store->console_dir, sizeof(store->console_dir), "%s/console", base) != 0)
        return -1;
    return 0;
}

int hold_ensure_system_store(struct hold_store *store) {
    if (hold_init_system_store(store) != 0) return -1;
    const struct { const char *path; mode_t mode; } dirs[] = {
        { store->base, 0755 },        { store->record_dir, 0700 }, { store->log_dir, 0700 },
        { store->console_dir, 0700 }, { store->public_dir, 0755 },
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (ensure_dir(dirs[i].path, dirs[i].mode, true) != 0) return -1;
    return 0;
}
