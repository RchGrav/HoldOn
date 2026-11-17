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
        hold_sig_note(inv, "grutty: %s has ended - see 'grutty logs %s'\n", r->id, r->id);
        return 0;
    }
    if (!r->has_console) {
        hold_sig_note(inv, "grutty: %s has no console (start with -it)\n", r->id);
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
        hold_sig_note(inv, "grutty: nothing to attach\n");
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
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    enum run_state st = hold_eval_state(&r, boot_id);
    rc = attach_console_record(inv, &r, st);
    hold_free_run_record(&r);
    free(targets);
    return rc;
}

void hold_usage(void) {
    printf("grutty %s - a tty that scripts and agents can see and drive\n\n"
           "Place a command as a durable call: see its screen, type into it, wait on\n"
           "it, list it, pick it back up, end it politely. No daemon, no config server.\n\n"
           "PLACE A CALL\n"
           "  grutty <cmd> [args...]         foreground: stream its output\n"
           "  grutty -d <cmd> [args...]      detached: print the bare 64-hex call id\n"
           "  grutty -it <cmd> [args...]     attached on a PTY (Ctrl-P Ctrl-Q holds it)\n"
           "  grutty <id|name>               redial: restart a call from its recipe\n"
           "  grutty --name web -d <cmd>...  name the call and place it in the background\n\n"
           "DRIVE A TTY CALL (scripts and agents)\n"
           "  grutty peek <target>           the screen as text (--json, --image out.png)\n"
           "  grutty send <target> TEXT      type into it (-k Enter, --paste, --mouse)\n"
           "  grutty wait <target>           until it settles (--idle, --match, exit)\n"
           "  grutty resize <target> 120 40  change its terminal size\n"
           "  grutty msg <target> TEXT       message an agent (it replies the same way)\n\n"
           "MANAGE YOUR CALLS\n"
           "  grutty list                    your ledger: your calls, live and past\n"
           "  grutty ps                      the Docker view: running calls, machine-wide\n"
           "  grutty attach <target>         pick the call back up (Ctrl-P Ctrl-Q detaches)\n"
           "  grutty logs <target> [-f]      print captured output (-n N newest lines)\n"
           "  grutty inspect <target>        structured JSON details\n"
           "  grutty ports <target>          listening sockets in use by the call\n"
           "  grutty stats <target>          live CPU, memory, and pid usage\n"
           "  grutty end <target> [--all]    end the call politely: TERM, then KILL\n"
           "  grutty kill <target>           KILL now, when it won't listen\n"
           "  grutty save <target>           protect a call from purge\n"
           "  grutty rename <target> <name>  name a call; naming it also saves it\n"
           "  grutty purge [<target>]        the removal verb (-a stale, --force saved)\n\n"
           "  target = call id, id prefix, or call name\n\n"
           "MORE\n"
           "  grutty help targets            id and scope resolution\n"
           "  grutty help scripting          exit codes, --print, --quiet, stdout\n"
           "  grutty <command> -h            help for one command\n\n"
           "  grutty --version\n",
           HOLD_VERSION);
}
