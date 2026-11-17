#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"

static volatile sig_atomic_t g_tail_interrupted = 0;

static void handle_tail_sigint(int signo) {
    (void)signo;
    g_tail_interrupted = 1;
}

/* ---- signal-target safety chain -------------------------------------- */

enum signal_validation_state { SIGNAL_TARGET_RUNNING, SIGNAL_TARGET_EXITED };

static int refuse_signal(const char *id, const char *why) {
    fprintf(stderr, "hold: error: call %s %s\n", id, why);
    return 2;
}

/* The safety chain: boot-id -> starttime -> exe dev/ino -> pgid/sid. A call
 * is signaled only when the live process tree still matches the recorded
 * identity; a record with no identity token is refused outright. */
static int validate_signal_target(const char *id,
                                  const struct hold_run_record *r,
                                  bool require_live,
                                  enum signal_validation_state *state_out) {
    if (state_out) *state_out = SIGNAL_TARGET_RUNNING;
    if (r->pgid <= 1) {
        fprintf(stderr, "hold: error: invalid pgid %ld in record file\n", (long)r->pgid);
        return 5;
    }
    if (r->sid <= 0) {
        fprintf(stderr, "hold: error: invalid sid %ld in record file\n", (long)r->sid);
        return 5;
    }

    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    if (r->has_boot && boot_id && strcmp(r->boot_id, boot_id) != 0)
        return refuse_signal(id, "is stale and cannot be signaled");

    bool have_identity_token = r->proc_starttime_ticks != 0 || (r->exe_dev != 0 && r->exe_ino != 0);
    char leader_state = 0;
    uint64_t leader_starttime = 0;
    bool have_leader_stat = hold_read_proc_stat_tokens(r->pid, &leader_state, &leader_starttime) == 0;
    if (have_leader_stat && leader_state != 'Z') {
        if (r->proc_starttime_ticks != 0 && leader_starttime != r->proc_starttime_ticks)
            return refuse_signal(id, "process identity differs from the record and cannot be signaled");
        if (r->proc_starttime_ticks == 0 && r->exe_dev != 0 && r->exe_ino != 0) {
            uint64_t exe_dev = 0;
            uint64_t exe_ino = 0;
            if (hold_read_proc_exe(r->pid, &exe_dev, &exe_ino) == 0 &&
                (exe_dev != r->exe_dev || exe_ino != r->exe_ino))
                return refuse_signal(id, "process identity differs from the record and cannot be signaled");
        }
        pid_t current_pgid = getpgid(r->pid);
        pid_t current_sid = getsid(r->pid);
        if ((current_pgid >= 0 && current_pgid != r->pgid) ||
            (current_sid >= 0 && current_sid != r->sid))
            return refuse_signal(id, "process group/session differs from the record and cannot be signaled");
        if (current_pgid >= 0 && current_sid >= 0) {
            if (!have_identity_token)
                return refuse_signal(id, "has no recorded process identity token and cannot be signaled");
            return 0;
        }
    }
    if (have_leader_stat && r->proc_starttime_ticks != 0 && leader_starttime != r->proc_starttime_ticks)
        return refuse_signal(id, "process identity differs from the record and cannot be signaled");

    /* Leader gone, group live: there is nothing left to compare the recorded
     * identity against, and no atomic verify-and-signal exists for a process
     * group. Matching the recorded pgid+sid within the same boot is the
     * deliberate best effort SPEC.md states; the zombie-leader test pins it. */
    enum group_liveness gl = hold_group_session_liveness(r->pgid, r->sid);
    if (gl == GROUP_LIVE) {
        if (!have_identity_token)
            return refuse_signal(id, "has no recorded process identity token and cannot be signaled");
        return 0;
    }
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        if (require_live)
            return refuse_signal(id, "is not running and cannot be printed as a signal command");
        if (state_out) *state_out = SIGNAL_TARGET_EXITED;
        return 0;
    }

    return refuse_signal(id, "could not be tied to its recorded process group/session and cannot be signaled");
}

/* ---- end/stop/kill ---------------------------------------------------- */

/* One target: load, validate, then either print the equivalent kill command
 * (--print revalidates with require_live) or deliver sig with the graceful
 * TERM -> revalidate -> KILL escalation. */
static int signal_one(const struct hold_store *store,
                      const char *id,
                      int sig,
                      bool graceful,
                      bool print_cmd,
                      bool *already_done) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    *already_done = false;
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) return 5;
    enum signal_validation_state state = SIGNAL_TARGET_RUNNING;
    int rc = validate_signal_target(id, &r, print_cmd, &state);
    if (rc != 0) goto out;
    if (print_cmd) {
        printf("kill -%s -- -%ld\n", sig == SIGKILL ? "KILL" : "TERM", (long)r.pgid);
        goto out;
    }
    if (state == SIGNAL_TARGET_EXITED) { *already_done = true; goto out; }
    if (kill(-r.pgid, sig) != 0) {
        if (errno == ESRCH) *already_done = true;
        else rc = errno == EPERM ? 3 : 4;
        goto out;
    }
    if (!graceful) { hold_report_session_escapees(&r); goto out; }
    if (hold_wait_target_group_gone(&r, STOP_TIMEOUT_MS)) { hold_report_session_escapees(&r); goto out; }
    state = SIGNAL_TARGET_RUNNING;
    rc = validate_signal_target(id, &r, false, &state);
    if (rc != 0) goto out;
    if (state == SIGNAL_TARGET_EXITED) { *already_done = true; goto out; }
    if (kill(-r.pgid, SIGKILL) != 0 && errno != ESRCH) {
        rc = errno == EPERM ? 3 : 4;
        goto out;
    }
    if (hold_wait_target_group_gone(&r, 1000)) { hold_report_session_escapees(&r); goto out; }
    rc = 4;
out:
    hold_free_run_record(&r);
    return rc;
}

int hold_cmd_signal_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful,
                             bool all,
                             bool print_cmd) {
    if (argc <= 0) {
        fprintf(stderr, "usage: hold %s <target>...\n", command);
        return 5;
    }
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    for (int i = 0; i < argc; i++) {
        struct hold_resolved_target *one = NULL;
        int none = 0;
        int rc = hold_resolve_action_token(inv, user_store, system_store, command, argv[i], all, &one, &none);
        if (rc != 0) {
            free(one);
            free(targets);
            return rc;
        }
        if (none > 0) {
            struct hold_resolved_target *next = realloc(targets, (size_t)(ntargets + none) * sizeof(*targets));
            if (!next) {
                free(one);
                free(targets);
                return 3;
            }
            targets = next;
            memcpy(targets + ntargets, one, (size_t)none * sizeof(*targets));
            ntargets += none;
        }
        free(one);
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "hold: nothing to %s\n", command);
        return 0;
    }
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].requires_root) {
            int rc = hold_report_requires_root(targets[i].id);
            free(targets);
            return rc;
        }
    }
    int worst = 0;
    for (int i = 0; i < ntargets; i++) {
        bool already_done = false;
        int rc = signal_one(&targets[i].store, targets[i].id, sig, graceful, print_cmd, &already_done);
        if (!print_cmd && rc == 0) {
            if (already_done) {
                hold_sig_note(inv, "hold: %s already exited\n", targets[i].id);
            } else {
                hold_sig_note(inv, "hold: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", targets[i].id);
            }
        }
        if (rc > worst) worst = rc;
    }
    free(targets);
    return worst;
}

/* ---- single-target prologue (tail / inspect / logs) ------------------- */

/* Resolve a single-target token; on a hit the target is copied out and the
 * list freed. *hit false means the caller returns rc as its exit status
 * (covers resolve errors and the friendly "nothing to <verb>" no-op). */
int hold_resolve_single(const struct hold_invocation *inv,
                          const struct hold_store *user_store,
                          const struct hold_store *system_store,
                          const char *command,
                          const char *verb,
                          const char *token,
                          struct hold_resolved_target *out,
                          bool *hit) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    *hit = false;
    int rc = hold_resolve_action_token(inv, user_store, system_store, command, token, false, &targets, &ntargets);
    if (rc == 0 && ntargets == 0) {
        hold_sig_note(inv, "hold: nothing to %s\n", verb);
    } else if (rc == 0) {
        *out = targets[0];
        *hit = true;
    }
    free(targets);
    return rc;
}

/* Requires-root gate + record load; *ok false means the caller returns rc. */
int hold_load_target_record(const struct hold_resolved_target *target,
                              bool need_log,
                              struct hold_run_record *r,
                              char *path,
                              size_t path_len,
                              bool *ok) {
    *ok = false;
    if (target->requires_root) return hold_report_requires_root(target->id);
    if (hold_load_record_by_id(target->store.record_dir, target->id, r, path, path_len) != 0) return 5;
    if (need_log && !r->has_log) {
        fprintf(stderr, "hold: record has no log path: %s\n", target->id);
        hold_free_run_record(r);
        return 5;
    }
    *ok = true;
    return 0;
}

/* ---- raw tail ---------------------------------------------------------- */

int hold_tail_log_until_exit(const struct hold_run_record *r, bool from_end, bool follow_until_exit) {
    int fd = open(r->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) hold_die_errno("hold: failed to open log for tail");

    char boot[128] = {0};
    const char *boot_id = r->has_boot ? hold_boot_id_or_null(boot) : NULL;
    if (from_end) lseek(fd, 0, SEEK_END);

    struct sigaction sa = {0}, old_sa = {0};
    sa.sa_handler = handle_tail_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_sa);
    g_tail_interrupted = 0;

    /* Logs are raw captured bytes (the pre-0.5 JSON-lines encoding is no
     * longer decoded); tailing is a straight copy to stdout. */
    char buf[4096];
    int sleep_polls = 0;
    ssize_t n;
    while (!g_tail_interrupted) {
        n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (hold_write_all(STDOUT_FILENO, buf, (size_t)n) != 0)
                hold_die_errno("hold: failed writing tailed output");
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            hold_die_errno("hold: failed while tailing log");
        }
        if (!follow_until_exit) break;
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        nanosleep(&sl, NULL);
        sleep_polls++;
        if (sleep_polls % 10 == 0 && hold_eval_state(r, boot_id) != STATE_RUNNING) {
            /* One final drain: output written between the last read and the
             * exit still lands. */
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                if (hold_write_all(STDOUT_FILENO, buf, (size_t)n) != 0) break;
            break;
        }
    }

    close(fd);
    sigaction(SIGINT, &old_sa, NULL);
    return 0;
}

int hold_cmd_tail_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *id_token) {
    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "tail", "tail", id_token, &target, &hit);
    if (!hit) return rc;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, true, &r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    bool running = hold_eval_state(&r, boot_id) == STATE_RUNNING;
    rc = hold_tail_log_until_exit(&r, running, running);
    hold_free_run_record(&r);
    return rc;
}

/* ---- inspect ------------------------------------------------------------ */

/* Build the neutral fd-target object for a live call's leader: where fds 0/1/2
 * actually point right now (a pipe, the pty, /dev/null, a redirected file).
 * Returns a heap string like "  \"Stdio\": {\n ... }" or NULL when the process
 * is gone or none of its fds could be read. Caller frees. */
static char *build_stdio_object(pid_t pid) {
    static const struct { int fd; const char *key; } slots[] = {
        {0, "Stdin"}, {1, "Stdout"}, {2, "Stderr"}
    };
    char targets[3][HOLD_PATH_MAX];
    bool any = false;
    for (size_t i = 0; i < 3; i++) {
        if (hold_proc_fd_target(pid, slots[i].fd, targets[i], sizeof(targets[i])) == 0) any = true;
        else targets[i][0] = '\0';
    }
    if (!any) return NULL;
    size_t cap = 4096;
    char *obj = malloc(cap);
    if (!obj) return NULL;
    FILE *f = fmemopen(obj, cap, "w");
    if (!f) {
        free(obj);
        return NULL;
    }
    fprintf(f, "  \"Stdio\": {\n");
    for (size_t i = 0; i < 3; i++) {
        fprintf(f, "    \"%s\": \"", slots[i].key);
        hold_json_escape(f, targets[i]);
        fprintf(f, "\"%s\n", i < 2 ? "," : "");
    }
    fprintf(f, "  }");
    if (fclose(f) != 0) {
        free(obj);
        return NULL;
    }
    return obj;
}

/* Print the record file as a one-object JSON array. With a live "Stdio"
 * object, splice it in before the object's closing brace: the record is a
 * single top-level object, so the last '}' in the file is its terminator. */
static int write_inspect(const char *path, const char *stdio_obj) {
    char *buf = NULL;
    if (hold_read_owned_file_no_symlink(path, &buf) != 0 || !buf) return -1;
    int rc = 0;
    if (stdio_obj) {
        char *close = strrchr(buf, '}');
        if (!close) {
            free(buf);
            return -1;
        }
        /* Trim whitespace before the closing brace so the spliced comma lands
         * right after the last existing value. */
        while (close > buf &&
               (close[-1] == '\n' || close[-1] == ' ' || close[-1] == '\t' || close[-1] == '\r'))
            close--;
        *close = '\0';
        if (fputs("[\n", stdout) == EOF || fputs(buf, stdout) == EOF ||
            fputs(",\n", stdout) == EOF || fputs(stdio_obj, stdout) == EOF ||
            fputs("\n}\n]\n", stdout) == EOF)
            rc = -1;
    } else {
        if (fputs("[\n", stdout) == EOF || fputs(buf, stdout) == EOF ||
            fputs("\n]\n", stdout) == EOF)
            rc = -1;
    }
    free(buf);
    return rc;
}

int hold_cmd_inspect_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token) {
    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "inspect", "inspect", id_token, &target, &hit);
    if (!hit) return rc;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, false, &r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    enum run_state st = hold_eval_state(&r, boot_id);
    /* Live fd targets are observable state, added only for a running call whose
     * leader still exists; ended calls fall back to the verbatim record. */
    char *stdio_obj = st == STATE_RUNNING && r.pid > 1 ? build_stdio_object(r.pid) : NULL;
    rc = write_inspect(path, stdio_obj);
    free(stdio_obj);
    hold_free_run_record(&r);
    if (rc != 0) hold_die_errno("hold: failed to inspect run record");
    return 0;
}

/* ---- logs ----------------------------------------------------------------- */

/* Offset where the newest `lines` lines begin (a trailing newline does not
 * start an empty line). Scans backward in chunks; 0 when the file is shorter. */
static off_t offset_of_last_lines(int fd, long lines) {
    off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0) return 0;
    char last = 0;
    if (pread(fd, &last, 1, end - 1) != 1) return 0;
    long seen = last == '\n' ? -1 : 0; /* the final newline only ends the last line */
    char buf[8192];
    off_t pos = end;
    while (pos > 0) {
        size_t chunk = pos < (off_t)sizeof(buf) ? (size_t)pos : sizeof(buf);
        pos -= (off_t)chunk;
        ssize_t got = pread(fd, buf, chunk, pos);
        if (got <= 0) return 0;
        for (ssize_t i = got - 1; i >= 0; i--) {
            if (buf[i] == '\n' && ++seen == lines) return pos + i + 1;
        }
    }
    return 0;
}

static const char logs_usage[] = "usage: hold logs <target> [--follow|-f] [--tail|-n N]\n";

/* hold logs: the call's captured output as plain bytes. -n N starts at the
 * newest N lines; -f keeps following until the call ends (Ctrl-C stops). */
int hold_cmd_view_action(const struct hold_invocation *inv,
                         const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         int argc,
                         char **argv) {
    const char *token = NULL;
    bool follow = false;
    long tail = -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = NULL;
        if (!strcmp(a, "--follow") || !strcmp(a, "-f")) {
            follow = true;
        } else if (!strcmp(a, "--tail") || !strcmp(a, "-n")) {
            if (++i >= argc) {
                fputs(logs_usage, stderr);
                return 5;
            }
            v = argv[i];
        } else if (!strncmp(a, "--tail=", 7)) {
            v = a + 7;
        } else if (!strcmp(a, "--plain") || !strcmp(a, "--print") || !strcmp(a, "-p")) {
            /* output is always plain; accepted for older scripts */
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            fputs(logs_usage, stdout);
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "hold: error: unknown flag '%s'\n%s", a, logs_usage);
            return 5;
        } else if (!token) {
            token = a;
        } else {
            fputs(logs_usage, stderr);
            return 5;
        }
        if (v) {
            char *end = NULL;
            errno = 0;
            tail = strtol(v, &end, 10);
            if (!*v || *end || errno || tail < 0) {
                fprintf(stderr, "hold: error: invalid log line limit\n");
                return 5;
            }
        }
    }
    if (!token) {
        fputs(logs_usage, stderr);
        return 5;
    }
    struct hold_resolved_target target;
    bool hit = false, ok = false;
    int rc = hold_resolve_single(inv, user_store, system_store, "view", "log", token, &target, &hit);
    if (!hit) return rc;
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    rc = hold_load_target_record(&target, true, &r, path, sizeof(path), &ok);
    if (!ok) return rc;
    char boot[128];
    bool running = hold_eval_state(&r, hold_boot_id_or_null(boot)) == STATE_RUNNING;
    if (tail >= 0) {
        int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) hold_die_errno("hold: failed to open log");
        off_t from = tail == 0 ? lseek(fd, 0, SEEK_END) : offset_of_last_lines(fd, tail);
        char buf[8192];
        ssize_t got;
        while ((got = pread(fd, buf, sizeof(buf), from)) > 0) {
            if (hold_write_all(STDOUT_FILENO, buf, (size_t)got) != 0) hold_die_errno("hold: failed writing log");
            from += got;
        }
        close(fd);
        rc = follow ? hold_tail_log_until_exit(&r, true, running) : 0;
    } else {
        rc = hold_tail_log_until_exit(&r, false, follow && running);
    }
    hold_free_run_record(&r);
    return rc;
}
