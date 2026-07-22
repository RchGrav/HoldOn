/* cli/main.c — dispatch, the launch-option grammar, and the privilege gates.
 * Per-command flag parsing, arity, usage text, and help live in cli/specs.c;
 * this file owns the bare-form (Docker-dialect) launch path, the value
 * grammars behind the launch options, and the verb dispatch. */
#include "hold/config.h"
#include "hold/types.h"
#include "hold/cli.h"
#include "hold/runtime.h"
#include "hold/access.h"
#include "hold/console.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/core.h"

struct cli_run_options {
    bool detach;
    bool interactive;
    bool tty;
    bool privileged;
    bool auto_remove;
    const char *name;
    char **env;
    int envc;
    char restart_policy[64];
    int restart_delay_seconds;
    bool restart_delay_seen;
};

static void free_run_options(struct cli_run_options *run) {
    if (!run) return;
    hold_free_argv_alloc(run->env, run->envc);
    memset(run, 0, sizeof(*run));
}

static struct hold_start_options start_options_from_run(const struct cli_run_options *run,
                                                        bool tail,
                                                        bool console_mode,
                                                        bool interactive_stdin,
                                                        int argc,
                                                        char **argv) {
    return (struct hold_start_options){
        .tail = tail,
        .console_mode = console_mode,
        .auto_remove = run->auto_remove,
        .interactive_stdin = interactive_stdin,
        .argc = argc,
        .argv = argv,
        .run_name = run->name,
        .envc = run->envc,
        .env = run->env,
        .restart_policy = run->restart_policy[0] ? run->restart_policy : NULL,
        .restart_delay_seconds = run->restart_delay_seconds,
    };
}

static bool parse_docker_run_flag(const char *arg,
                                  bool *detach,
                                  bool *interactive,
                                  bool *tty,
                                  bool *privileged) {
    if (!arg || arg[0] != '-') return false;
    if (!strcmp(arg, "-d") || !strcmp(arg, "--detach")) {
        *detach = true;
        return true;
    }
    if (!strcmp(arg, "-i") || !strcmp(arg, "--interactive")) {
        *interactive = true;
        return true;
    }
    if (!strcmp(arg, "-t") || !strcmp(arg, "--tty")) {
        *tty = true;
        return true;
    }
    if (!strcmp(arg, "-it") || !strcmp(arg, "-ti")) {
        *interactive = true;
        *tty = true;
        return true;
    }
    if (!strcmp(arg, "--privileged")) {
        *privileged = true;
        return true;
    }
    return false;
}

/* ---- value grammars behind the launch options (spec surface; frozen) ---- */

static int parse_restart_policy_arg(const char *arg, char out[64]) {
    if (!arg || !*arg) {
        fprintf(stderr, "hold: error: --restart requires a policy\n");
        return 5;
    }
    if (!strcmp(arg, "no") || !strcmp(arg, "always") || !strcmp(arg, "unless-stopped")) {
        snprintf(out, 64, "%s", arg);
        return 0;
    }
    if (!strncmp(arg, "on-failure", 10) && (arg[10] == '\0' || arg[10] == ':')) {
        if (arg[10] == ':') {
            char *end = NULL;
            long n = strtol(arg + 11, &end, 10);
            if (!end || *end || n < 0 || n > INT_MAX) {
                fprintf(stderr, "hold: error: invalid --restart on-failure retry count '%s'\n", arg + 11);
                return 5;
            }
        }
        snprintf(out, 64, "%s", arg);
        return 0;
    }
    fprintf(stderr, "hold: error: invalid --restart policy '%s'\n", arg);
    return 5;
}

static bool restart_policy_arg_enabled(const char *arg) {
    return arg && *arg && strcmp(arg, "no") != 0;
}

static int parse_restart_delay_arg(const char *arg, int *out) {
    if (!arg || !*arg) {
        fprintf(stderr, "hold: error: --restart-delay requires seconds\n");
        return 5;
    }
    char *end = NULL;
    long n = strtol(arg, &end, 10);
    if (!end || *end || n < 0 || n > INT_MAX) {
        fprintf(stderr, "hold: error: invalid --restart-delay '%s'\n", arg);
        return 5;
    }
    *out = (int)n;
    return 0;
}

/* One detach-key token: a literal byte, ^X, or ctrl-x / ctrl-X. */
static int detach_key_token_value(const char *token, size_t len, unsigned char *out) {
    if (!token || len == 0 || !out) return -1;
    if (len == 1) {
        *out = (unsigned char)token[0];
        return 0;
    }
    if (len == 2 && token[0] == '^' && token[1] >= '@' && token[1] <= '_') {
        *out = (unsigned char)(token[1] - '@');
        return 0;
    }
    if (len == 6 &&
        tolower((unsigned char)token[0]) == 'c' &&
        tolower((unsigned char)token[1]) == 't' &&
        tolower((unsigned char)token[2]) == 'r' &&
        tolower((unsigned char)token[3]) == 'l' &&
        token[4] == '-') {
        if (token[5] >= '@' && token[5] <= '_') {
            *out = (unsigned char)(token[5] - '@');
            return 0;
        }
        if (token[5] >= 'a' && token[5] <= 'z') {
            *out = (unsigned char)(token[5] - 'a' + 1);
            return 0;
        }
    }
    return -1;
}

static int configure_detach_keys_option(const char *value) {
    if (!value || !*value) {
        fprintf(stderr, "hold: error: --detach-keys requires a key sequence\n");
        return 5;
    }
    unsigned char keys[8];
    size_t nkeys = 0;
    const char *p = value;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
        if (nkeys >= sizeof(keys) || detach_key_token_value(start, (size_t)(p - start), &keys[nkeys]) != 0) {
            fprintf(stderr, "hold: error: invalid --detach-keys sequence '%s'\n", value);
            return 5;
        }
        nkeys++;
    }
    if (nkeys == 0 || hold_console_set_detach_keys(keys, nkeys) != 0) {
        fprintf(stderr, "hold: error: invalid --detach-keys sequence '%s'\n", value);
        return 5;
    }
    return 0;
}

static int append_env_assignment(const char *arg, char ***env_out, int *envc_out) {
    const char *eq = arg ? strchr(arg, '=') : NULL;
    if (!arg || !*arg || !eq || eq == arg) {
        fprintf(stderr, "hold: error: expected KEY=VALUE after --env/-e\n");
        return 5;
    }
    size_t key_len = (size_t)(eq - arg);
    char key[256];
    if (key_len >= sizeof(key)) {
        fprintf(stderr, "hold: error: environment key is too long\n");
        return 5;
    }
    memcpy(key, arg, key_len);
    key[key_len] = '\0';
    for (size_t i = 0; key[i]; i++) {
        if (key[i] == '=') {
            fprintf(stderr, "hold: error: invalid environment key\n");
            return 5;
        }
    }
    char *copy = strdup(arg);
    if (!copy) return 1;
    char **next = realloc(*env_out, ((size_t)*envc_out + 1) * sizeof(*next));
    if (!next) {
        free(copy);
        return 1;
    }
    next[*envc_out] = copy;
    *env_out = next;
    (*envc_out)++;
    return 0;
}

static int append_env_file(const char *path, char ***env_out, int *envc_out) {
    if (!path || !*path) {
        fprintf(stderr, "hold: error: --env-file requires a path\n");
        return 5;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "hold: error: cannot read env file '%s': %s\n", path, strerror(errno));
        return 5;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t nr;
    unsigned long lineno = 0;
    int rc = 0;
    while ((nr = getline(&line, &cap, f)) >= 0) {
        lineno++;
        while (nr > 0 && (line[nr - 1] == '\n' || line[nr - 1] == '\r')) {
            line[--nr] = '\0';
        }
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        rc = append_env_assignment(p, env_out, envc_out);
        if (rc != 0) {
            fprintf(stderr, "hold: error: invalid --env-file entry at %s:%lu\n", path, lineno);
            break;
        }
    }
    free(line);
    if (ferror(f) && rc == 0) {
        fprintf(stderr, "hold: error: failed reading env file '%s': %s\n", path, strerror(errno));
        rc = 5;
    }
    fclose(f);
    return rc;
}

/* ---- launch options: one table drives both spellings of every value flag ---- */

enum run_opt_id { OPT_NAME, OPT_ENV, OPT_ENV_FILE, OPT_DETACH_KEYS, OPT_RESTART, OPT_RESTART_DELAY };

/* Each is spelled `--flag value`; rows with eq_form also take `--flag=value`
 * (Docker parity; --name deliberately has no `=` spelling). */
static const struct run_value_opt {
    const char *shortf;
    const char *longf;
    bool eq_form;
    enum run_opt_id id;
} run_value_opts[] = {
    {NULL, "--name", false, OPT_NAME},
    {"-e", "--env", true, OPT_ENV},
    {NULL, "--env-file", true, OPT_ENV_FILE},
    {NULL, "--detach-keys", true, OPT_DETACH_KEYS},
    {NULL, "--restart", true, OPT_RESTART},
    {NULL, "--restart-delay", true, OPT_RESTART_DELAY},
};

static int apply_run_value_opt(enum run_opt_id id, const char *value, struct cli_run_options *run) {
    switch (id) {
    case OPT_NAME:
        run->name = value;
        return 0;
    case OPT_ENV:
        return append_env_assignment(value, &run->env, &run->envc);
    case OPT_ENV_FILE:
        return append_env_file(value, &run->env, &run->envc);
    case OPT_DETACH_KEYS:
        return configure_detach_keys_option(value);
    case OPT_RESTART:
        return parse_restart_policy_arg(value, run->restart_policy);
    case OPT_RESTART_DELAY: {
        int rc = parse_restart_delay_arg(value, &run->restart_delay_seconds);
        if (rc == 0) run->restart_delay_seen = true;
        return rc;
    }
    }
    return 1;
}

static int parse_run_options(int argc,
                             char **argv,
                             int *index,
                             struct cli_run_options *run,
                             bool *requested_system,
                             bool *tail,
                             bool *console_mode,
                             bool *parsed) {
    const char *arg = argv[*index];
    *parsed = true;
    if (parse_docker_run_flag(arg, &run->detach, &run->interactive, &run->tty, &run->privileged)) {
        if (run->privileged) *requested_system = true;
        if (run->tty) *console_mode = true;
        (*index)++;
        return 0;
    }
    /* Substrate flags Hold cannot honor fail honestly, never accept-and-ignore. */
    if (!strcmp(arg, "-p") || !strcmp(arg, "--publish") ||
        !strcmp(arg, "-P") || !strcmp(arg, "--publish-all") ||
        !strncmp(arg, "--publish=", 10)) {
        fprintf(stderr,
                "hold: error: Hold is not containerized and does not publish or forward ports; listening ports are observed automatically and shown in `hold ps`\n");
        return 5;
    }
    if (!strcmp(arg, "-v") || !strcmp(arg, "--volume") || !strncmp(arg, "--volume=", 9)) {
        fprintf(stderr,
                "hold: error: Hold is not containerized and does not mount or remap volumes; pass host paths directly (prefer absolute paths)\n");
        return 5;
    }
    if (!strcmp(arg, "--cap-add") || !strncmp(arg, "--cap-add=", 10) ||
        !strcmp(arg, "--cap-drop") || !strncmp(arg, "--cap-drop=", 11)) {
        /* Grants-era remnant: capabilities left with that subsystem. Reject
         * honestly like the other substrate flags rather than accept-and-ignore. */
        fprintf(stderr,
                "hold: error: %s is not supported; hold does not manage capabilities.\n"
                "  Constrain the command itself instead, e.g. via capsh or systemd-run.\n",
                arg);
        return 5;
    }
    for (size_t i = 0; i < sizeof(run_value_opts) / sizeof(run_value_opts[0]); i++) {
        const struct run_value_opt *opt = &run_value_opts[i];
        size_t longn = strlen(opt->longf);
        int rc;
        if ((opt->shortf && !strcmp(arg, opt->shortf)) || !strcmp(arg, opt->longf)) {
            if (*index + 1 >= argc) {
                fprintf(stderr, "usage: hold [flags] <cmd|id|name> [args...]\n");
                return 5;
            }
            rc = apply_run_value_opt(opt->id, argv[*index + 1], run);
            if (rc != 0) return rc;
            *index += 2;
            return 0;
        }
        if (opt->eq_form && !strncmp(arg, opt->longf, longn) && arg[longn] == '=') {
            rc = apply_run_value_opt(opt->id, arg + longn + 1, run);
            if (rc != 0) return rc;
            (*index)++;
            return 0;
        }
    }
    if (!strcmp(arg, "--rm")) {
        run->auto_remove = true;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--tail") || !strcmp(arg, "-f")) {
        *tail = true;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--console")) {
        *console_mode = true;
        (*index)++;
        return 0;
    }
    *parsed = false;
    return 0;
}

int hold_cli_main(int argc, char **argv) {
    if (argc < 2) {
        /* Docker parity: bare invocation prints help. */
        hold_usage();
        return 0;
    }

    int argi = 1;
    bool tail = false;
    bool console_mode = false;
    bool force_raw = false;
    struct cli_run_options run = {0};
    struct hold_cli_owned_opts oo = {0};

    while (argi < argc) {
        bool parsed_run_option = false;
        int option_rc = parse_run_options(argc, argv, &argi, &run, &oo.requested_system,
                                          &tail, &console_mode, &parsed_run_option);
        if (option_rc != 0) {
            free_run_options(&run);
            return option_rc;
        }
        if (parsed_run_option) {
            continue;
        }
        if (!strcmp(argv[argi], "--system")) {
            oo.requested_system = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--quiet")) {
            oo.quiet = true;
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
        fprintf(stderr, "usage: hold [flags] <cmd|id|name> [args...]\n");
        return 5;
    }

    /* Whether the invocation named a session mode explicitly, captured before the
     * bare-form tail defaulting below rewrites `tail`. On redial this decides
     * whether the recipe's recorded mode applies or the invocation overrides it. */
    bool explicit_session_mode = run.detach || run.interactive || run.tty || console_mode || tail;

    bool owned = !force_raw && !tail && hold_cli_command_is_parser_owned(argv[argi]);
    const char *command = owned ? argv[argi++] : NULL;
    int cmd_argc = 0;
    char **cmd_argv = NULL;

    if (owned) {
        cmd_argv = calloc((size_t)(argc - argi + 1), sizeof(char *));
        if (!cmd_argv) {
            return 1;
        }
        int flag_rc = hold_cli_collect_owned_args(command, argc - argi, argv + argi,
                                                  cmd_argv, &cmd_argc, &oo);
        if (flag_rc != 0) {
            free(cmd_argv);
            free_run_options(&run);
            return flag_rc;
        }
    } else {
        cmd_argc = argc - argi;
        cmd_argv = argv + argi;
    }

    if (!owned && !force_raw && !tail && !strcmp(argv[argi], "--version")) {
        puts(HOLD_VERSION);
        return 0;
    }
    if (!owned && !force_raw && !tail && (!strcmp(argv[argi], "--help") || !strcmp(argv[argi], "-h"))) {
        hold_usage();
        return 0;
    }
    if (owned && !strcmp(command, "help")) {
        int rc = 0;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: hold help [topic]\n");
            rc = 5;
        } else if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
            rc = hold_show_help(NULL);
        } else {
            rc = hold_show_help(cmd_argc == 1 ? cmd_argv[0] : NULL);
        }
        free(cmd_argv);
        return rc;
    }
    if (owned && !oo.saw_delimiter && cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
        int rc = hold_show_help(command);
        free(cmd_argv);
        return rc;
    }
    if (console_mode && owned) {
        fprintf(stderr, "hold: error: -t/--console applies only when launching a call\n");
        free(cmd_argv);
        return 5;
    }
    if (owned) {
        int arity_rc = hold_validate_owned_command_arity(command, cmd_argc);
        if (arity_rc != 0) {
            free(cmd_argv);
            return arity_rc;
        }
    }
    if (run.restart_delay_seen && !restart_policy_arg_enabled(run.restart_policy)) {
        fprintf(stderr, "hold: error: --restart-delay requires --restart with an active policy\n");
        if (owned) free(cmd_argv);
        free_run_options(&run);
        return 5;
    }

    struct hold_invocation inv;
    if (hold_detect_invocation(&inv, oo.requested_system) != 0) {
        hold_die_errno("hold: failed to resolve invocation context");
    }
    inv.quiet = oo.quiet;
    /* The bare form launches (or redials) a call and speaks Docker's output
     * dialect: foreground streams and prints nothing of its own; -d prints the
     * bare 64-hex call id. Management verbs keep their own output. */
    inv.docker_run = !owned;
    if (!owned) {
        if (!run.detach && (!console_mode || run.tty)) {
            tail = true;
        }
        if (run.detach) {
            tail = false;
        }
    }
    bool interactive_stdin = run.interactive && !run.tty;

    /* Aliases fold onto their canonical verb (the specs table); dispatch below
     * keys on canon. ps is deliberately NOT folded onto list: they diverge
     * (ps mirrors Docker's machine-wide running view, list is Hold's scoped
     * ledger). */
    const char *canon = owned ? hold_cli_command_canon(command) : NULL;
    /* Both read views may look at the system store without being root; only
     * acting on it requires root. */
    bool is_read_view = owned && (!strcmp(canon, "list") || !strcmp(canon, "ps"));

    /* The root-managed store is visible to everyone (the read views stay open),
     * but only root may act on it. Delegated execution is gone, so requesting it
     * as a normal user is refused rather than re-run under sudo. */
    if (oo.requested_system && !inv.euid_root && !is_read_view && !(owned && !strcmp(canon, "purge"))) {
        fprintf(stderr, "hold: error: the root-managed store requires root; re-run as root or with sudo\n");
        if (owned) free(cmd_argv);
        free_run_options(&run);
        return 3;
    }

    struct hold_store user_store;
    struct hold_store system_store;
    memset(&user_store, 0, sizeof(user_store));
    if (hold_init_system_store(&system_store) != 0) {
        hold_die_errno("hold: failed to resolve system storage");
    }

    if (!inv.euid_root) {
        if (hold_ensure_user_store_for_current_user(&user_store) != 0) {
            hold_die_errno("hold: failed to init user storage");
        }
    } else {
        /* Root has no store of its own in the normal flow (its calls are
         * system-managed), but list -u / purge -u still need a personal store
         * handle: the invoking user's under sudo, else root's own home. The
         * paths are resolved without creating anything; an absent store simply
         * lists empty. */
        const char *home = inv.have_sudo_user && inv.invoking_home[0] ? inv.invoking_home : getenv("HOME");
        if (home && *home) {
            (void)hold_init_user_store_from_home(home, &user_store);
        }
    }

    if (!owned) {
        struct hold_store start_store;
        if (hold_ensure_start_store_for_command(&inv, oo.requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || oo.requested_system) &&
                hold_start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                hold_die_errno("hold: failed to init invoking-user storage");
            }
            hold_die_errno("hold: failed to init start storage");
        }
        /* Redial: a lone token that resolves to a retained call (id then name)
         * restarts it from its recipe. `--`, --name, and multi-token argv all
         * force a fresh command launch (PATH lookup happens via sh -c). */
        if (!force_raw && !run.name && cmd_argc == 1) {
            bool redialed = false;
            int rc = hold_cmd_redial(&inv, &user_store, &system_store, tail, console_mode,
                                     run.auto_remove, interactive_stdin, explicit_session_mode,
                                     run.restart_policy[0] ? run.restart_policy : NULL,
                                     run.restart_delay_seconds, cmd_argv[0], &redialed);
            if (redialed) {
                free_run_options(&run);
                return rc;
            }
        }
        /* Resolution fell through to a PATH command: exec argv directly (a
         * missing command fails without leaving a record), matching the doc's
         * "call id -> call name -> PATH command" order. Shell snippets go
         * through an explicit `hold -- sh -c ...`. */
        struct hold_start_options opts = start_options_from_run(&run, tail, console_mode,
                                                                interactive_stdin, cmd_argc, cmd_argv);
        int rc = hold_perform_start_options(&inv, &start_store, &opts);
        free_run_options(&run);
        return rc;
    }

    int rc = 0;
    if (!strcmp(canon, "on")) {
        rc = hold_cmd_shell_action(&inv, inv.euid_root ? &system_store : &user_store);
    } else if (!strcmp(canon, "off")) {
        rc = hold_cmd_off_action();
    } else if (!strcmp(canon, "save")) {
        rc = hold_cmd_save_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "rename")) {
        rc = hold_cmd_rename_action(&inv, &user_store, &system_store, cmd_argv[0], cmd_argv[1]);
    } else if (!strcmp(canon, "list") || !strcmp(canon, "ps")) {
        const char *name_filter = cmd_argc == 1 ? cmd_argv[0] : NULL;
        if (name_filter && !hold_valid_alias(name_filter)) {
            fprintf(stderr, "hold: error: invalid name '%s'\n", name_filter);
            rc = 5;
        } else if (!strcmp(canon, "ps")) {
            /* ps is Docker's machine-wide running view: both scopes, no USER
             * column, only the -a (include-ended) flag. */
            rc = hold_cmd_ps(&inv, &user_store, &system_store, name_filter, oo.all);
        } else {
            /* list is Hold's scoped ledger. The scope flags select which stores
             * it draws from; the command resolves DEFAULT against privilege. */
            enum hold_list_scope scope = HOLD_LIST_SCOPE_DEFAULT;
            if (oo.user_scope) {
                scope = HOLD_LIST_SCOPE_USER;
            } else if (oo.system_scope || oo.requested_system) {
                scope = HOLD_LIST_SCOPE_SYSTEM;
            } else if (oo.all) {
                scope = HOLD_LIST_SCOPE_BOTH;
            }
            rc = hold_cmd_list(&inv, &user_store, &system_store, name_filter, scope, oo.live_only);
        }
    } else if (!strcmp(canon, "tail")) {
        rc = hold_cmd_tail_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "inspect")) {
        rc = hold_cmd_inspect_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "ports")) {
        rc = hold_cmd_ports_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "stats")) {
        rc = hold_cmd_stats_action(&inv, &user_store, &system_store, cmd_argv[0], oo.no_stream);
    } else if (!strcmp(canon, "__view")) {
        rc = hold_cmd_view_action(&inv, &user_store, &system_store, cmd_argc, cmd_argv);
    } else if (!strcmp(canon, "attach")) {
        rc = hold_cmd_console_action(&inv, &user_store, &system_store, cmd_argv[0]);
    } else if (!strcmp(canon, "purge")) {
        const char *target = cmd_argc > 0 ? cmd_argv[0] : NULL;
        if (target && target[0] == '-') {
            fprintf(stderr, "hold: error: unknown flag '%s'\n", target);
            const char *usage = hold_cli_command_usage(command);
            if (usage) fprintf(stderr, "%s\n", usage);
            rc = 1;
        } else {
            /* -u/--user forces the personal sweep; -s/--system (or --system)
             * sweeps the global store. -a keeps its state meaning here
             * (include stale). */
            bool purge_system = (oo.system_scope || oo.requested_system) && !oo.user_scope;
            if (purge_system && !target && !inv.euid_root) {
                /* A non-root global sweep re-execs through sudo so sudo prompts
                 * for the password. This is NOT the deleted elevation
                 * subsystem: no sudoers pinning, no capability tokens — one
                 * auditable execvp, whose root child re-enters here already
                 * privileged and sweeps directly. */
                char self[HOLD_PATH_MAX];
                if (hold_resolve_self_executable_path(argv[0], self, sizeof(self)) == 0) {
                    char *sudo_argv[8];
                    int k = 0;
                    sudo_argv[k++] = "sudo";
                    sudo_argv[k++] = self;
                    sudo_argv[k++] = "purge";
                    sudo_argv[k++] = "--system";
                    if (oo.all) sudo_argv[k++] = "--all";
                    if (oo.force) sudo_argv[k++] = "--force";
                    sudo_argv[k] = NULL;
                    execvp("sudo", sudo_argv);
                }
                fprintf(stderr, "hold: global purge needs root: sudo hold purge --system\n");
                rc = 3;
            } else {
                rc = hold_cmd_purge_action(&inv, &user_store, &system_store, target, oo.all, oo.force, purge_system);
            }
        }
    } else if (!strcmp(canon, "end")) {
        rc = hold_cmd_signal_action(&inv, &user_store, &system_store, "stop", cmd_argc, cmd_argv, SIGTERM, true, oo.all, oo.print_cmd);
    } else if (!strcmp(canon, "kill")) {
        rc = hold_cmd_signal_action(&inv, &user_store, &system_store, "kill", cmd_argc, cmd_argv, SIGKILL, false, oo.all, oo.print_cmd);
    } else {
        hold_usage();
        rc = 1;
    }
    free(cmd_argv);
    return rc;
}
