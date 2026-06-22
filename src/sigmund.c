#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/runtime.h"
#include "sigmund/access.h"
#include "sigmund/console.h"
#include "sigmund/store.h"
#include "sigmund/platform.h"
#include "sigmund/core.h"

static int show_help(const char *topic);

static int help_profiles(void) {
    printf("sigmund help profiles\n\n"
           "Pin a run's exact command (resolved binary path + argv) under a reusable\n"
           "name.\n\n"
           "  sigmund alias <id> <name>       pin the command behind <id> as <name>\n"
           "  sigmund aliases [-v]            list visible aliases\n"
           "  sigmund start <name>            start a fresh run under that name\n\n"
           "The name is also recorded on runs started as <name>, so later\n"
           "list, tail, console, dump, stop, kill, and prune commands can use <name>. If the command behind\n"
           "<name> is updated later, future starts use the updated command; prior runs\n"
           "remain under the same recorded alias label.\n");
    return 0;
}

static int help_targets(void) {
    printf("sigmund help targets\n\n"
           "  <target>          resolve in the current context\n"
           "  user:<target>     force user-local lookup\n"
           "  system:<target>   force root-managed lookup\n\n"
           "target = run id, leading id prefix, or alias name\n\n"
           "A run id addresses one run directly, always. An alias resolves among runs\n"
           "recorded under that name, narrowed by the verb: stop/kill/tail look at\n"
           "running runs, console looks at running console-enabled runs, dump looks at\n"
           "logged runs, and prune looks at removable past\n"
           "run data. One match acts. Several matches exit 6 and print candidates;\n"
           "--all resolves that ambiguity for stop, kill, and prune. A known alias with\n"
           "nothing to do exits 0.\n");
    return 0;
}

static int help_access(void) {
    printf("sigmund help access\n\n"
           "Grant another user permission to act on a specific root-managed alias as\n"
           "root, without a password, scoped to one immutable protected profile.\n\n"
           "  sigmund grant  <alias> <user> [actions]\n"
           "  sigmund revoke <alias> <user> [actions]\n\n"
           "actions = any of: start,stop,kill,tail,dump,prune,console   (default: all)\n\n"
           "The <user> field may be a username, %%group, or all. Sigmund stores one\n"
           "managed sudoers file per alias/user pair. The file contains the current\n"
           "protected profile hash for that alias, an anchored action alternation, and\n"
           "an 8-hex run selector slot. If root updates the alias profile and the hash\n"
           "changes, grant rewrites the same managed file via temp file, visudo check,\n"
           "and atomic rename.\n");
    return 0;
}

static int help_system(void) {
    printf("sigmund help system\n\n"
           "Root, sudo, and --system runs use the root-managed store:\n\n"
           "  Linux: /var/lib/sigmund\n"
           "  macOS: /var/db/sigmund\n\n"
           "Private root records, logs, and profiles stay root-only. Normal users see\n"
           "only the redacted public index and public alias dictionary. A normal action\n"
           "on a root-only public target self-elevates through sudo; user-local targets\n"
           "win over root-public collisions.\n\n"
           "  sigmund --system <cmd...>       start in root-managed state\n"
           "  sigmund --system list           list authoritative root records\n");
    return 0;
}

static int help_scripting(void) {
    printf("sigmund help scripting\n\n"
           "stdout is for machine data. Human banners, confirmations, warnings, and\n"
           "errors go to stderr; --quiet suppresses normal human status.\n\n"
           "  id=$(sigmund <cmd...>)          capture the bare 8-hex run id\n"
           "  sigmund stop --print <id>       print kill -TERM -- -<pgid>\n"
           "  sigmund kill --print <id>       print kill -KILL -- -<pgid>\n\n"
           "Exit codes:\n"
           "  0  success (includes known alias with nothing to do)\n"
           "  1  usage / generic error\n"
           "  2  refused for safety\n"
           "  3  permission denied or storage/security failure\n"
           "  4  signal delivery failed\n"
           "  5  target not found or invalid target\n"
           "  6  must disambiguate\n");
    return 0;
}

static int help_console(void) {
    printf("sigmund help console\n\n"
           "Start a run with an attachable PTY console, then reconnect to it later.\n"
           "Console output is still tee'd to the normal log, so tail and dump continue\n"
           "to work.\n\n"
           "  sigmund --console <cmd...>      start with an attachable console\n"
           "  sigmund start <alias> --console start an alias with a console\n"
           "  sigmund console <target>        attach to that console\n\n"
           "Console attach is native: Sigmund saves your terminal, enters an alternate\n"
           "screen for interactive attaches, forwards terminal size changes to the PTY,\n"
           "and restores your original screen on exit. Ctrl-] detaches without asking\n"
           "Sigmund to stop the run.\n");
    return 0;
}

static int help_action(const char *action) {
    if (!strcmp(action, "list")) {
        printf("usage: sigmund list [alias] [--iso|-l]\n\nShow all visible runs, optionally filtered by recorded alias label.\n");
    } else if (!strcmp(action, "start")) {
        printf("usage: sigmund start <alias> [--multi [N]] [--console]\n       sigmund start <cmd> [args...]\n\nStart an alias recipe, or use explicit start form for a raw command.\n");
    } else if (!strcmp(action, "stop")) {
        printf("usage: sigmund stop [--print] [--all] <target>...\n\nGracefully stop matching runs with TERM, then KILL if needed.\n");
    } else if (!strcmp(action, "kill")) {
        printf("usage: sigmund kill [--print] [--all] <target>...\n\nForce matching runs down with KILL.\n");
    } else if (!strcmp(action, "tail")) {
        printf("usage: sigmund tail <target>\n\nFollow live output for an alias match, or follow an id's log directly.\n");
    } else if (!strcmp(action, "console")) {
        printf("usage: sigmund console <target>\n\nAttach to a running console-enabled run.\n");
    } else if (!strcmp(action, "dump")) {
        printf("usage: sigmund dump <target>\n\nPrint a run log and exit.\n");
    } else if (!strcmp(action, "prune")) {
        printf("usage: sigmund prune [target|all] [--all]\n\nClear removable past run data. Running valid runs are never pruned.\n");
    } else if (!strcmp(action, "alias")) {
        printf("usage: sigmund alias <id> <name> [-v]\n\nPin the command behind a run id as a reusable alias.\n");
    } else if (!strcmp(action, "aliases")) {
        printf("usage: sigmund aliases [-v]\n\nList visible aliases. User aliases show commands; system commands are redacted.\n");
    } else if (!strcmp(action, "grant") || !strcmp(action, "revoke")) {
        printf("usage: sigmund %s <alias> <user> [start,stop,kill,tail,dump,prune,console]\n\nManage Sigmund-owned sudoers access for a root-managed alias.\n", action);
    } else {
        return -1;
    }
    return 0;
}

static int show_help(const char *topic) {
    if (!topic || !*topic) {
        usage();
        return 0;
    }
    if (!strcmp(topic, "profiles")) return help_profiles();
    if (!strcmp(topic, "targets")) return help_targets();
    if (!strcmp(topic, "access")) return help_access();
    if (!strcmp(topic, "system")) return help_system();
    if (!strcmp(topic, "scripting")) return help_scripting();
    if (!strcmp(topic, "console")) return help_console();
    if (help_action(topic) == 0) return 0;
    fprintf(stderr, "sigmund: unknown help topic '%s'\n", topic);
    return 5;
}

static bool is_sigmund_owned_command(const char *s) {
    return s && (!strcmp(s, "list") || !strcmp(s, "stop") || !strcmp(s, "kill") ||
                 !strcmp(s, "tail") || !strcmp(s, "dump") || !strcmp(s, "prune") ||
                 !strcmp(s, "console") ||
                 !strcmp(s, "start") ||
                 !strcmp(s, "alias") || !strcmp(s, "aliases") ||
                 !strcmp(s, "grant") || !strcmp(s, "revoke") ||
                 !strcmp(s, "help"));
}

static bool parse_positive_count(const char *s, int *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < 1 || v > 1000) {
        return false;
    }
    *out = (int)v;
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    int argi = 1;
    bool requested_system = false;
    bool elevated = false;
    bool tail = false;
    bool console_mode = false;
    bool force_raw = false;
    bool all = false;
    bool multi = false;
    bool quiet = false;
    bool print_cmd = false;
    bool list_iso = false;
    int multi_count = 1;

    while (argi < argc) {
        if (!strcmp(argv[argi], "--system")) {
            requested_system = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--elevated")) {
            elevated = true;
            requested_system = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--tail") || !strcmp(argv[argi], "-f")) {
            tail = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--console")) {
            console_mode = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--quiet")) {
            quiet = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--")) {
            force_raw = true;
            argi++;
            break;
        }
        break;
    }

    if (argi >= argc) {
        usage();
        return 5;
    }

    bool owned = !force_raw && !tail && is_sigmund_owned_command(argv[argi]);
    const char *command = owned ? argv[argi++] : NULL;
    int cmd_argc = 0;
    char **cmd_argv = NULL;

    if (owned) {
        cmd_argv = calloc((size_t)(argc - argi + 1), sizeof(char *));
        if (!cmd_argv) {
            return 3;
        }
        bool literal_owned_arg = false;
        for (int i = argi; i < argc; i++) {
            if (!literal_owned_arg && !strcmp(argv[i], "--")) {
                literal_owned_arg = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--system")) {
                requested_system = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--elevated")) {
                elevated = true;
                requested_system = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--quiet")) {
                quiet = true;
                continue;
            }
            if (!literal_owned_arg && command_accepts_target_tokens(command) && !strcmp(argv[i], "--all")) {
                all = true;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "stop") || !strcmp(command, "kill")) &&
                !strcmp(argv[i], "--print")) {
                print_cmd = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") &&
                (!strcmp(argv[i], "--tail") || !strcmp(argv[i], "-f"))) {
                tail = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && !strcmp(argv[i], "--console")) {
                console_mode = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "list") &&
                (!strcmp(argv[i], "--iso") || !strcmp(argv[i], "-l"))) {
                list_iso = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && !strcmp(argv[i], "--multi")) {
                multi = true;
                multi_count = 1;
                if (i + 1 < argc) {
                    int parsed = 0;
                    if (parse_positive_count(argv[i + 1], &parsed)) {
                        multi_count = parsed;
                        i++;
                    }
                }
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && strncmp(argv[i], "--multi=", 8) == 0) {
                multi = true;
                if (!parse_positive_count(argv[i] + 8, &multi_count)) {
                    fprintf(stderr, "sigmund: error: invalid --multi count '%s'\n", argv[i] + 8);
                    free(cmd_argv);
                    return 5;
                }
                continue;
            }
            cmd_argv[cmd_argc++] = argv[i];
        }
    } else {
        command = NULL;
        cmd_argc = argc - argi;
        cmd_argv = argv + argi;
    }

    if (!owned && !force_raw && !tail && !strcmp(argv[argi], "--version")) {
        puts(SIGMUND_VERSION);
        return 0;
    }
    if (!owned && !force_raw && !tail && (!strcmp(argv[argi], "--help") || !strcmp(argv[argi], "-h"))) {
        usage();
        return 0;
    }
    if (owned && !strcmp(command, "help")) {
        int rc = 0;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: sigmund help [topic]\n");
            rc = 5;
        } else if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
            rc = show_help(NULL);
        } else {
            rc = show_help(cmd_argc == 1 ? cmd_argv[0] : NULL);
        }
        free(cmd_argv);
        return rc;
    }
    if (owned && cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
        int rc = show_help(command);
        free(cmd_argv);
        return rc;
    }
    if (console_mode && owned && strcmp(command, "start") != 0) {
        fprintf(stderr, "sigmund: error: --console applies only to starts\n");
        free(cmd_argv);
        return 5;
    }

    struct invocation inv;
    if (detect_invocation(&inv, requested_system, elevated) != 0) {
        die_errno("sigmund: failed to resolve invocation context");
    }
    inv.quiet = quiet;
    if (inv.elevated && !inv.euid_root) {
        fprintf(stderr, "sigmund: internal error: --elevated without root authority\n");
        if (owned) {
            free(cmd_argv);
        }
        return 3;
    }

    bool is_list = owned && !strcmp(command, "list");
    if (requested_system && !inv.euid_root && owned && !strcmp(command, "start") && cmd_argc == 1) {
        struct store_paths pre_system_store;
        if (init_system_store(&pre_system_store) == 0) {
            const char *atom = NULL;
            enum id_token_scope start_scope = parse_id_token(cmd_argv[0], &atom);
            if ((start_scope == ID_TOKEN_PLAIN || start_scope == ID_TOKEN_SYSTEM) && atom &&
                (valid_profile_hash(atom) || valid_alias(atom))) {
                char hash[PROFILE_HASH_STR_LEN];
                if (resolve_public_profile_token(&pre_system_store, atom, hash) == 1) {
                    int rc = 0;
                    int starts = multi ? multi_count : 1;
                    for (int i = 0; i < starts; i++) {
                        rc = elevate_start_token(argv[0],
                                                 tail,
                                                 console_mode,
                                                 valid_alias(atom) ? atom : hash,
                                                 valid_alias(atom) ? hash : NULL,
                                                 false,
                                                 1);
                        if (rc != 0) {
                            break;
                        }
                    }
                    free(cmd_argv);
                    return rc;
                }
            }
        }
    }
    if (requested_system && !inv.euid_root && !is_list) {
        int canonical_rc = 0;
        if (owned && maybe_elevate_requested_system_targets(argv[0], command, cmd_argc, cmd_argv, all, &canonical_rc)) {
            free(cmd_argv);
            return canonical_rc;
        }
        int rc = elevate_with_sudo_parsed(argv[0], owned, command, tail, console_mode, all, print_cmd, multi, multi_count, force_raw, cmd_argc, cmd_argv);
        if (owned) {
            free(cmd_argv);
        }
        return rc;
    }

    struct store_paths user_store;
    struct store_paths system_store;
    memset(&user_store, 0, sizeof(user_store));
    if (init_system_store(&system_store) != 0) {
        die_errno("sigmund: failed to resolve system storage");
    }

    if (!inv.euid_root || is_list || (owned && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                                               !strcmp(command, "tail") || !strcmp(command, "dump") ||
                                               !strcmp(command, "prune") || !strcmp(command, "console")))) {
        if (!inv.euid_root) {
            if (ensure_user_store_for_current_user(&user_store) != 0) {
                die_errno("sigmund: failed to init user storage");
            }
        }
    }

    if (inv.elevated && inv.euid_root && owned && cmd_argc == 3 &&
        (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
         !strcmp(command, "tail") || !strcmp(command, "dump") || !strcmp(command, "prune") ||
         !strcmp(command, "console"))) {
        int sig = !strcmp(command, "kill") ? SIGKILL : SIGTERM;
        bool graceful = !strcmp(command, "stop");
        int rc = cmd_elevated_capability_action(&inv, &system_store, command, tail, console_mode, sig, graceful, cmd_argc, cmd_argv);
        if (rc >= 0) {
            free(cmd_argv);
            return rc;
        }
    }

    if (!owned) {
        struct store_paths start_store;
        if (ensure_start_store_for_command(&inv, requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                die_errno("sigmund: failed to init invoking-user storage");
            }
            die_errno("sigmund: failed to init start storage");
        }
        return perform_start(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, NULL);
    }

    if (!strcmp(command, "start")) {
        struct store_paths start_store;
        if (ensure_start_store_for_command(&inv, requested_system, true, command, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                start_target_is_within_invoking_home(&inv, true, command, cmd_argc, cmd_argv)) {
                die_errno("sigmund: failed to init invoking-user storage");
            }
            die_errno("sigmund: failed to init start storage");
        }
        int rc = cmd_start_action(&inv, &user_store, &system_store, argv[0], &start_store, tail, console_mode, multi, multi_count, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "list")) {
        int rc;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: sigmund list [alias]\n");
            free(cmd_argv);
            return 5;
        }
        const char *alias_filter = cmd_argc == 1 ? cmd_argv[0] : NULL;
        if (alias_filter && !valid_alias(alias_filter)) {
            fprintf(stderr, "sigmund: error: invalid alias '%s'\n", alias_filter);
            free(cmd_argv);
            return 5;
        }
        if (inv.euid_root) {
            rc = cmd_list_system(&system_store, alias_filter, list_iso);
        } else {
            rc = cmd_list_normal(&user_store, &system_store, alias_filter, list_iso);
        }
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "tail")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund tail <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_tail_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "dump")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund dump <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_dump_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "console")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund console <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_console_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "prune")) {
        const char *target = cmd_argc > 0 ? cmd_argv[0] : NULL;
        int rc = cmd_prune_action(&inv, &user_store, &system_store, argv[0], target, all);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "alias")) {
        int rc = cmd_alias_action(&inv, &user_store, &system_store, argv[0], cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "aliases")) {
        bool aliases_verbose = false;
        if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "-v") || !strcmp(cmd_argv[0], "--verbose"))) {
            aliases_verbose = true;
        } else if (cmd_argc != 0) {
            fprintf(stderr, "usage: sigmund aliases [-v]\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_aliases_action(&inv, &user_store, &system_store, aliases_verbose);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "grant")) {
        if (ensure_system_store(&system_store) != 0) {
            die_errno("sigmund: failed to init system storage");
        }
        int rc = cmd_grant_revoke_action(&inv, &system_store, argv[0], true, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "revoke")) {
        if (ensure_system_store(&system_store) != 0) {
            die_errno("sigmund: failed to init system storage");
        }
        int rc = cmd_grant_revoke_action(&inv, &system_store, argv[0], false, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "stop")) {
        int rc = cmd_signal_action(&inv, &user_store, &system_store, argv[0], "stop", cmd_argc, cmd_argv, SIGTERM, true, all, print_cmd);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "kill")) {
        int rc = cmd_signal_action(&inv, &user_store, &system_store, argv[0], "kill", cmd_argc, cmd_argv, SIGKILL, false, all, print_cmd);
        free(cmd_argv);
        return rc;
    }

    free(cmd_argv);
    usage();
    return 1;
}
