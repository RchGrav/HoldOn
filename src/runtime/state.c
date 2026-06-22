#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/runtime.h"
#include "sigmund/runtime_internal.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"
#include "sigmund/store.h"
#include "sigmund/console.h"
#include "sigmund/access.h"

static bool target_group_gone(const struct record *r);

void report_session_escapees(const struct record *r) {
    int escaped = count_session_escapees(r->sid, r->pgid);
    if (escaped > 0) {
        fprintf(stderr,
                "sigmund: warning: %d process(es) escaped process-group %ld but remain in session %ld\n",
                escaped, (long)r->pgid, (long)r->sid);
    }
}

enum run_state eval_state(const struct record *r, const char *current_boot) {
    if ((r->has_state && strcmp(r->state, "failed") == 0) || (r->has_launch_error && r->launch_error[0] != '\0')) {
        return STATE_FAILED;
    }
    if (r->pgid <= 1 || r->sid <= 0) {
        return STATE_UNKNOWN;
    }
    if (r->has_boot && current_boot && strcmp(r->boot_id, current_boot) != 0) {
        return STATE_STALE;
    }

    char state = 0;
    uint64_t now_starttime = 0;
    bool has_stat = read_proc_stat_tokens(r->pid, &state, &now_starttime) == 0;
    bool present = has_stat || leader_present(r->pid);
    enum group_liveness gl = group_session_liveness(r->pgid, r->sid);

    if (has_stat && state == 'Z') {
        if (gl == GROUP_LIVE) {
            return STATE_RUNNING;
        }
        if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
            return STATE_EXITED;
        }
        return STATE_UNKNOWN;
    }

    if (present) {
        if (r->proc_starttime_ticks && has_stat) {
            if (now_starttime != r->proc_starttime_ticks) {
                return STATE_STALE;
            }
        } else if (r->exe_dev && r->exe_ino) {
            uint64_t d, i;
            if (read_proc_exe(r->pid, &d, &i) == 0 && (d != r->exe_dev || i != r->exe_ino)) {
                return STATE_STALE;
            }
        }
        return STATE_RUNNING;
    }

    if (gl == GROUP_LIVE) {
        return STATE_RUNNING;
    }
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        return STATE_EXITED;
    }

    int g = group_exists(r->pgid);
    if (g == 0) {
        return STATE_EXITED;
    }
    return STATE_UNKNOWN;
}

void rollback_spawned_group(pid_t pid, pid_t pgid) {
    if (pgid > 1) {
        kill(-pgid, SIGKILL);
    }
    if (pid > 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
}

static bool target_group_gone(const struct record *r) {
    enum group_liveness gl = group_session_liveness(r->pgid, r->sid);
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        return true;
    }
    if (gl == GROUP_LIVE) {
        return false;
    }
    return group_exists(r->pgid) == 0;
}

bool wait_target_group_gone(const struct record *r, int timeout_ms) {
    int waited = 0;
    while (waited <= timeout_ms) {
        if (target_group_gone(r)) {
            return true;
        }
        if (waited == timeout_ms) {
            break;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = POLL_SLEEP_MS * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
        waited += POLL_SLEEP_MS;
    }
    return false;
}

const char *state_str(enum run_state s) {
    switch (s) {
    case STATE_RUNNING:
        return "running";
    case STATE_EXITED:
        return "exited";
    case STATE_STALE:
        return "stale";
    case STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
