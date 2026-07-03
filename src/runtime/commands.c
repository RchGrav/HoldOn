#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"

static int attach_console_record(const struct hold_invocation *inv,
                                 const struct hold_run_record *r,
                                 enum run_state st);

static int attach_console_record(const struct hold_invocation *inv,
                                 const struct hold_run_record *r,
                                 enum run_state st) {
    if (st != STATE_RUNNING) {
        hold_sig_note(inv, "hold: %s has exited - see 'hold dump %s'\n", r->id, r->id);
        return 0;
    }
    if (!r->has_console) {
        hold_sig_note(inv, "hold: %s has no console (start with --console)\n", r->id);
        return 0;
    }
    return hold_run_native_console(r->console_sock);
}

int hold_cmd_console_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "console", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "hold: nothing to console\n");
        return 0;
    }
    struct hold_resolved_target target = targets[0];
    if (target.requires_root) {
        free(targets);
        return hold_report_requires_root(target.id);
    }
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
    rc = attach_console_record(inv, &r, st);
    hold_free_run_record(&r);
    free(targets);
    return rc;
}

void hold_usage(void) {
    printf("hold %s - more than nohup, less than systemd\n\n"
           "Run a command under a durable run ID, then list it, watch it,\n"
           "inspect it, and stop it later. No daemon, no config server.\n\n"
           "USAGE\n"
           "  hold [run-options] <cmd> [args...]      Docker-shaped ad-hoc launch\n"
           "  hold run [run-options] <cmd>            Docker-shaped launch\n"
           "  hold <command> [args...]                manage runs\n\n"
           "RUN\n"
           "  hold <command>...                       foreground run, streaming logs\n"
           "  hold run <command>...                   foreground run, streaming logs\n"
           "  hold -d <command>...                    detach/background and print run ID\n"
           "  hold -it <command>...                   allocate Hold's PTY/console path\n"
           "  hold run --name web -d <command>...     name and run in the background\n\n"
           "MANAGE\n"
           "  hold ps [-a]                            list active (-a: retained inactive too)\n"
           "  hold attach <target>                    attach to a running console/TTY run\n"
           "  hold logs <target> [-f] [-n N]          open/filter logs\n"
           "  hold logs <target> --plain              print log text and exit\n"
           "  hold inspect <target>                   print structured JSON details\n"
           "  hold stop <target> [--all]              graceful stop (TERM, then KILL)\n"
           "  hold kill <target>                      force kill now (KILL)\n"
           "  hold start <id|name>                    restart a retained run\n"
           "  hold rm [--force] <target>              remove inactive run; force active runid\n"
           "  hold prune [target|all]                 clear inactive past run data\n\n"
           "SESSION\n"
           "  hold shell                              capture/adopt from a real system shell\n\n"
           "  target = run id, id prefix, or run name\n\n"
           "MORE\n"
           "  hold help targets            id and scope resolution\n"
           "  hold help system             root-managed runs\n"
           "  hold help scripting          exit codes, --print, --quiet, stdout\n"
           "  hold <command> -h            help for one command\n\n"
           "  hold --version\n",
           HOLD_VERSION);
}
