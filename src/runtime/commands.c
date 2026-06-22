#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/runtime.h"
#include "sigmund/runtime_internal.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"
#include "sigmund/store.h"
#include "sigmund/console.h"
#include "sigmund/access.h"

static int attach_console_record(const struct sigmund_invocation *inv,
                                 const struct sigmund_run_record *r,
                                 enum run_state st);
static int print_aliases_for_store(const char *scope, const struct sigmund_store *store, bool verbose);

static int attach_console_record(const struct sigmund_invocation *inv,
                                 const struct sigmund_run_record *r,
                                 enum run_state st) {
    if (st != STATE_RUNNING) {
        sigmund_sig_note(inv, "sigmund: %s has exited - see 'sigmund dump %s'\n", r->id, r->id);
        return 0;
    }
    if (!r->has_console) {
        sigmund_sig_note(inv, "sigmund: %s has no console (start with --console)\n", r->id);
        return 0;
    }
    return sigmund_run_native_console(r->console_sock);
}

int sigmund_cmd_console_action(const struct sigmund_invocation *inv,
                              const struct sigmund_store *user_store,
                              const struct sigmund_store *system_store,
                              const char *program,
                              const char *id_token) {
    struct sigmund_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = sigmund_resolve_action_token(inv, user_store, system_store, "console", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sigmund_sig_note(inv, "sigmund: nothing to console\n");
        return 0;
    }
    struct sigmund_resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = sigmund_elevate_with_sudo_targets(program, "console", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct sigmund_run_record r;
    char path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
    enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
    rc = attach_console_record(inv, &r, st);
    free(targets);
    return rc;
}

int sigmund_cmd_alias_action(const struct sigmund_invocation *inv,
                            const struct sigmund_store *user_store,
                            const struct sigmund_store *system_store,
                            const char *program,
                            int argc,
                            char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: sigmund alias <id> <name> [-v]\n");
        return 5;
    }
    const char *target_token = argv[0];
    const char *name = argv[1];
    bool verbose = false;
    if (argc == 3) {
        if (strcmp(argv[2], "-v") != 0 && strcmp(argv[2], "--verbose") != 0) {
            fprintf(stderr, "usage: sigmund alias <id> <name> [-v]\n");
            return 5;
        }
        verbose = true;
    }
    if (!sigmund_valid_alias(name)) {
        fprintf(stderr, "sigmund: error: invalid alias '%s'\n", name);
        return 5;
    }

    struct sigmund_resolved_target target;
    if (sigmund_resolve_target(inv, user_store, system_store, target_token, &target) != 0) {
        return 5;
    }
    if (target.scope == RESOLVE_NOT_FOUND) {
        return sigmund_report_not_found(target_token);
    }
    if (target.needs_elevation) {
        char scoped[8 + PROFILE_HASH_STR_LEN];
        if (sigmund_checked_snprintf(scoped, sizeof(scoped), "system:%s", target.id) != 0) {
            return 3;
        }
        char *canon[3] = {"alias", scoped, (char *)name};
        return sigmund_elevate_with_sudo_canonical(program, 3, canon);
    }
    if (target.store.kind == STORE_USER_LOCAL && inv->euid_root) {
        fprintf(stderr, "sigmund: error: create user-local aliases as that user\n");
        return 5;
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        if (sigmund_ensure_system_store(&target.store) != 0) {
            sigmund_die_errno("sigmund: failed to init system storage");
        }
    } else if (sigmund_ensure_user_store_for_current_user(&target.store) != 0) {
        sigmund_die_errno("sigmund: failed to init user storage");
    }

    struct sigmund_run_record r;
    char record_path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(target.store.record_dir, target.id, &r, record_path, sizeof(record_path)) != 0) {
        return 5;
    }
    char *j = NULL;
    if (sigmund_read_owned_file_no_symlink(record_path, &j) != 0) {
        return 5;
    }
    char **profile_argv = NULL;
    int profile_argc = 0;
    char binary_path[SIGMUND_PATH_MAX];
    char hash[PROFILE_HASH_STR_LEN];
    int rc = 0;
    if (sigmund_json_get_argv_alloc(j, &profile_argv, &profile_argc) != 0 ||
        sigmund_resolve_binary_path(profile_argv[0], binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "sigmund: error: failed to derive profile from run %s\n", target.id);
        rc = 5;
        goto out;
    }
    char command[256];
    if (sigmund_format_argv_human(command, sizeof(command), profile_argc, profile_argv) != 0) {
        snprintf(command, sizeof(command), "%s", "?");
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        sigmund_profile_hash_for_argv(binary_path, profile_argc, profile_argv, hash);
        if (sigmund_write_profile_atomic(&target.store, hash, binary_path, profile_argc, profile_argv) != 0) {
            sigmund_die_errno("sigmund: failed to write profile");
        }
        if (sigmund_alias_upsert_hash(&target.store, name, hash) != 0) {
            sigmund_die_errno("sigmund: failed to write alias");
        }
        if (verbose) {
            sigmund_sig_note(inv, "sigmund: pinned '%s' -> %s (hash %s)\n", name, command, hash);
        } else {
            sigmund_sig_note(inv, "sigmund: pinned '%s' -> %s\n", name, command);
        }
    } else {
        if (sigmund_alias_upsert_recipe(&target.store, name, binary_path, profile_argc, profile_argv) != 0) {
            sigmund_die_errno("sigmund: failed to write alias");
        }
        sigmund_sig_note(inv, "sigmund: pinned '%s' -> %s\n", name, command);
    }

out:
    sigmund_free_argv_alloc(profile_argv, profile_argc);
    free(j);
    return rc;
}

static int print_aliases_for_store(const char *scope, const struct sigmund_store *store, bool verbose) {
    struct sigmund_alias *entries = NULL;
    size_t count = 0;
    if (sigmund_load_aliases(store, &entries, &count) != 0) {
        fprintf(stderr, "sigmund: warning: failed to read %s aliases\n", scope);
        return 5;
    }
    for (size_t i = 0; i < count; i++) {
        char command[96];
        char hash_display[PROFILE_HASH_STR_LEN];
        if (entries[i].has_recipe) {
            if (sigmund_format_argv_human(command, sizeof(command), entries[i].argc, entries[i].argv) != 0) {
                snprintf(command, sizeof(command), "%s", "?");
            }
        } else {
            snprintf(command, sizeof(command), "%s", "<root-managed>");
        }
        if (entries[i].has_hash) {
            if (verbose) {
                snprintf(hash_display, sizeof(hash_display), "%s", entries[i].hash);
            } else {
                snprintf(hash_display, sizeof(hash_display), "%.12s...", entries[i].hash);
            }
        } else {
            snprintf(hash_display, sizeof(hash_display), "%s", "-");
        }
        printf("%-12s %-6s %-40.40s %s\n", entries[i].name, scope, command, hash_display);
    }
    sigmund_free_aliases(entries, count);
    return 0;
}

int sigmund_cmd_aliases_action(const struct sigmund_invocation *inv,
                              const struct sigmund_store *user_store,
                              const struct sigmund_store *system_store,
                              bool verbose) {
    printf("%-12s %-6s %-40s %s\n", "NAME", "SCOPE", "COMMAND", "HASH");
    int rc = 0;
    if (inv->euid_root) {
        if (print_aliases_for_store("system", system_store, verbose) != 0) {
            rc = 5;
        }
        if (!inv->requested_system && inv->have_sudo_user) {
            struct sigmund_store sudo_user_store;
            if (sigmund_init_invoking_user_store(inv, &sudo_user_store) == 0 &&
                print_aliases_for_store("user", &sudo_user_store, verbose) != 0) {
                rc = 5;
            }
        }
        return rc;
    }
    if (print_aliases_for_store("user", user_store, verbose) != 0) {
        rc = 5;
    }
    if (print_aliases_for_store("system", system_store, verbose) != 0) {
        rc = 5;
    }
    return rc;
}

void sigmund_usage(void) {
    printf("sigmund %s - more than nohup, less than systemd\n\n"
           "Run a command that outlives your shell, then find it, watch it, and stop it\n"
           "safely later. No daemon, no config.\n\n"
           "USAGE\n"
           "  sigmund <command> [args...]      start a command in the background\n"
           "  sigmund <action>  [target...]    act on a tracked command\n\n"
           "START\n"
           "  sigmund <command>...             start it; prints a short run id\n"
           "  sigmund -f <command>...          start it and stream output\n"
           "  sigmund --console <command>...   start it with an attachable console\n"
           "  sigmund start <alias>            start a pinned alias\n\n"
           "MANAGE\n"
           "  sigmund list   [alias]          show tracked runs (optionally one alias)\n"
           "  sigmund tail   <target>         follow a run's live output\n"
           "  sigmund console <target>        attach to a run's console\n"
           "  sigmund dump   <target>         print a run's log and exit\n"
           "  sigmund stop   <target>         graceful stop (TERM, then KILL)\n"
           "  sigmund kill   <target>         force kill now (KILL)\n"
           "  sigmund prune  [target|all]     clear past run data\n\n"
           "  target = run id, id prefix, or alias name\n\n"
           "MORE\n"
           "  sigmund help profiles           pin a command as a reusable alias\n"
           "  sigmund help access             give another user scoped access\n"
           "  sigmund help targets            id, alias, and scope resolution\n"
           "  sigmund help system             root-managed runs and elevation\n"
           "  sigmund help scripting          exit codes, --print, --quiet, stdout\n"
           "  sigmund help console            attachable PTY consoles\n"
           "  sigmund <action> -h             help for one action\n\n"
           "  sigmund --version\n",
           SIGMUND_VERSION);
}

int sigmund_cmd_elevated_capability_action(const struct sigmund_invocation *inv,
                                          const struct sigmund_store *system_store,
                                          const char *command,
                                          bool tail,
                                          bool console_mode,
                                          int sig,
                                          bool graceful,
                                          int argc,
                                          char **argv) {
    if (!inv->euid_root || argc != 3) {
        return -1;
    }
    const char *runid_sel = argv[0];
    const char *alias = argv[1];
    const char *hash = argv[2];
    if (!sigmund_valid_runid_selector(runid_sel) || !sigmund_valid_alias(alias) || !sigmund_valid_profile_hash(hash)) {
        return -1;
    }
    if (sigmund_verify_system_alias_cap(system_store, alias, hash) != 0) {
        fprintf(stderr, "sigmund: error: capability for '%s' is no longer valid\n", alias);
        return 3;
    }

    if (!strcmp(command, "start")) {
        if (strcmp(runid_sel, "00000000") != 0) {
            fprintf(stderr, "sigmund: error: start capability requires selector 00000000\n");
            return 5;
        }
        return sigmund_perform_profile_start(inv, system_store, tail, console_mode, hash, alias);
    }

    if (strcmp(runid_sel, "00000000") == 0) {
        fprintf(stderr, "sigmund: error: selector 00000000 is only valid for start\n");
        return 5;
    }

    if (strcmp(runid_sel, "ffffffff") == 0) {
        if (!sigmund_command_all_allowed(command)) {
            fprintf(stderr, "sigmund: error: selector ffffffff is not valid for %s\n", command);
            return 5;
        }
        struct alias_match_list matches;
        if (sigmund_collect_private_alias_matches(system_store, alias, command, &matches) != 0) {
            return 3;
        }
        int worst = 0;
        int acted = 0;
        char boot[128] = {0};
        bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
        for (size_t i = 0; i < matches.count; i++) {
            int rc = 0;
            if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
                bool already_done = false;
                rc = sigmund_do_signal_action(system_store,
                                      matches.items[i].id,
                                      sig,
                                      graceful,
                                      &already_done);
                if (rc == 0) {
                    sigmund_sig_note(inv,
                             "sigmund: %s %s\n",
                             already_done ? matches.items[i].id : (!strcmp(command, "kill") ? "killed" : "stopped"),
                             already_done ? "already exited" : matches.items[i].id);
                }
            } else if (!strcmp(command, "prune")) {
                bool removed = false;
                rc = sigmund_prune_one_run(system_store, matches.items[i].id, have_boot ? boot : NULL, true, &removed);
                if (removed) {
                    acted++;
                }
            }
            if (rc == 0 && strcmp(command, "prune") != 0) {
                acted++;
            }
            if (rc > worst) {
                worst = rc;
            }
        }
        if (!strcmp(command, "prune") && worst == 0) {
            if (acted > 0) {
                sigmund_sig_note(inv, "sigmund: pruned %d past run%s for '%s'\n", acted, acted == 1 ? "" : "s", alias);
            } else {
                sigmund_sig_note(inv, "sigmund: nothing to prune\n");
            }
        } else if (acted == 0 && worst == 0) {
            sigmund_sig_note(inv, "sigmund: nothing to %s\n", command);
        }
        sigmund_free_alias_match_list(&matches);
        return worst;
    }

    if (sigmund_ensure_run_recorded_under_alias(system_store, runid_sel, alias) != 0) {
        fprintf(stderr, "sigmund: error: run %s is not recorded under alias '%s'\n", runid_sel, alias);
        return 3;
    }

    struct sigmund_run_record selected_record;
    char selected_path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(system_store->record_dir, runid_sel, &selected_record, selected_path, sizeof(selected_path)) != 0) {
        return 5;
    }
    char selected_boot[128] = {0};
    bool have_selected_boot = sigmund_current_boot_id(selected_boot, sizeof(selected_boot));
    enum run_state selected_state = sigmund_eval_state(&selected_record, have_selected_boot ? selected_boot : NULL);
    if (!strcmp(command, "console")) {
        return attach_console_record(inv, &selected_record, selected_state);
    }
    if (!sigmund_record_matches_alias_intent(command, &selected_record, selected_state)) {
        sigmund_sig_note(inv, "sigmund: nothing to %s\n", command);
        return 0;
    }

    if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
        bool already_done = false;
        int rc = sigmund_do_signal_action(system_store, runid_sel, sig, graceful, &already_done);
        if (rc == 0) {
            if (already_done) {
                sigmund_sig_note(inv, "sigmund: %s already exited\n", runid_sel);
            } else {
                sigmund_sig_note(inv, "sigmund: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", runid_sel);
            }
        }
        return rc;
    }
    if (!strcmp(command, "prune")) {
        char boot[128] = {0};
        bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
        bool removed = false;
        int rc = sigmund_prune_one_run(system_store, runid_sel, have_boot ? boot : NULL, true, &removed);
        if (rc == 0) {
            sigmund_sig_note(inv, removed ? "sigmund: pruned 1 past run for '%s'\n" : "sigmund: nothing to prune\n", alias);
        }
        return rc;
    }
    if (!strcmp(command, "tail") || !strcmp(command, "dump")) {
        struct sigmund_run_record r;
        char path[SIGMUND_PATH_MAX];
        if (sigmund_load_record_by_id(system_store->record_dir, runid_sel, &r, path, sizeof(path)) != 0 || !r.has_log) {
            return 5;
        }
        if (!strcmp(command, "tail")) {
            char boot[128] = {0};
            bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
            enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
            return sigmund_tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
        }
        int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            sigmund_die_errno("sigmund: failed to open log for dump");
        }
        char buf[4096];
        while (1) {
            ssize_t nr = read(fd, buf, sizeof(buf));
            if (nr == 0) {
                break;
            }
            if (nr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                sigmund_die_errno("sigmund: failed while dumping log");
            }
            if (sigmund_write_all(STDOUT_FILENO, buf, (size_t)nr) != 0) {
                close(fd);
                sigmund_die_errno("sigmund: failed writing dumped output");
            }
        }
        close(fd);
        return 0;
    }

    return -1;
}
