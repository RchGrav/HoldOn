#include "hold/config.h"
#include "hold/types.h"
#include "hold/cli.h"
#include "hold/runtime.h"
#include "hold/access.h"
#include "hold/console.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/core.h"

static void print_command_usage_stderr(const char *command);
static bool parse_docker_run_flag(const char *arg,
                                  bool *detach,
                                  bool *interactive,
                                  bool *tty,
                                  bool *privileged);
static int append_env_assignment(const char *arg, char ***env_out, int *envc_out);
static int append_string_option(const char *what, const char *arg, char ***items_out, int *count_out);
static int append_string_option(const char *what, const char *arg, char ***items_out, int *count_out) {
    if (!arg || !*arg) {
        fprintf(stderr, "hold: error: %s requires a value\n", what);
        return 5;
    }
    char *copy = strdup(arg);
    if (!copy) return 3;
    char **next = realloc(*items_out, ((size_t)*count_out + 1) * sizeof(*next));
    if (!next) {
        free(copy);
        return 3;
    }
    next[*count_out] = copy;
    *items_out = next;
    (*count_out)++;
    return 0;
}

static int reject_publish_option(void);
static int reject_volume_option(void);
static int parse_restart_policy_arg(const char *arg, char out[64]);
static int parse_restart_delay_arg(const char *arg, int *out);
static bool restart_policy_arg_enabled(const char *arg);
static int append_env_file(const char *path, char ***env_out, int *envc_out);
static int configure_detach_keys_option(const char *value);
static bool is_legacy_run_namespace_verb(const char *arg);
static bool command_supports_multiplicity(const char *command);

struct cli_run_options {
    bool detach;
    bool interactive;
    bool tty;
    bool privileged;
    bool auto_remove;
    const char *name;
    char **env;
    int envc;
    char **ports;
    int portc;
    char **volumes;
    int volumec;
    char **cap_add;
    int cap_addc;
    char **cap_drop;
    int cap_dropc;
    char restart_policy[64];
    int restart_delay_seconds;
    bool restart_delay_seen;
};

static void free_run_options(struct cli_run_options *run) {
    if (!run) return;
    hold_free_argv_alloc(run->env, run->envc);
    hold_free_argv_alloc(run->ports, run->portc);
    hold_free_argv_alloc(run->volumes, run->volumec);
    hold_free_argv_alloc(run->cap_add, run->cap_addc);
    hold_free_argv_alloc(run->cap_drop, run->cap_dropc);
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
        .portc = run->portc,
        .ports = run->ports,
        .volumec = run->volumec,
        .volumes = run->volumes,
        .cap_addc = run->cap_addc,
        .cap_add = run->cap_add,
        .cap_dropc = run->cap_dropc,
        .cap_drop = run->cap_drop,
        .restart_policy = run->restart_policy[0] ? run->restart_policy : NULL,
        .restart_delay_seconds = run->restart_delay_seconds,
    };
}

static void print_command_usage_stderr(const char *command) {
    const char *usage = hold_cli_command_usage(command);
    if (usage) {
        fprintf(stderr, "%s\n", usage);
    }
}

static bool command_supports_multiplicity(const char *command) {
    return command && (!strcmp(command, "start") || !strcmp(command, "run"));
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
        token[4] == '-' &&
        token[5] >= '@' && token[5] <= '_') {
        *out = (unsigned char)(token[5] - '@');
        return 0;
    }
    if (len == 6 &&
        tolower((unsigned char)token[0]) == 'c' &&
        tolower((unsigned char)token[1]) == 't' &&
        tolower((unsigned char)token[2]) == 'r' &&
        tolower((unsigned char)token[3]) == 'l' &&
        token[4] == '-' &&
        token[5] >= 'a' && token[5] <= 'z') {
        *out = (unsigned char)(token[5] - 'a' + 1);
        return 0;
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
    if (setenv(key, eq + 1, 1) != 0) {
        hold_die_errno("hold: failed to set launch environment");
    }
    char *copy = strdup(arg);
    if (!copy) return 3;
    char **next = realloc(*env_out, ((size_t)*envc_out + 1) * sizeof(*next));
    if (!next) {
        free(copy);
        return 3;
    }
    next[*envc_out] = copy;
    *env_out = next;
    (*envc_out)++;
    return 0;
}

static int reject_publish_option(void) {
    fprintf(stderr,
            "hold: error: Hold is not containerized and does not publish or forward ports; listening ports are observed automatically and shown in `hold ps`\n");
    return 5;
}

static int reject_volume_option(void) {
    fprintf(stderr,
            "hold: error: Hold is not containerized and does not mount or remap volumes; pass host paths directly (prefer absolute paths)\n");
    return 5;
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

static bool is_legacy_run_namespace_verb(const char *arg) {
    static const char *verbs[] = {
        "stop", "kill", "logs", "tail", "dump", "inspect", "console", "rm", "prune",
        "show", "status", "ps", "list"
    };
    if (!arg) return false;
    for (size_t i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++) {
        if (!strcmp(arg, verbs[i])) return true;
    }
    return false;
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
    if (!strcmp(arg, "--name")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        run->name = argv[*index + 1];
        *index += 2;
        return 0;
    }
    if (!strcmp(arg, "-e") || !strcmp(arg, "--env")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = append_env_assignment(argv[*index + 1], &run->env, &run->envc);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--env=", 6)) {
        int rc = append_env_assignment(arg + 6, &run->env, &run->envc);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--env-file")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = append_env_file(argv[*index + 1], &run->env, &run->envc);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--env-file=", 11)) {
        int rc = append_env_file(arg + 11, &run->env, &run->envc);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "-p") || !strcmp(arg, "--publish") ||
        !strcmp(arg, "-P") || !strcmp(arg, "--publish-all") ||
        !strncmp(arg, "--publish=", 10)) {
        return reject_publish_option();
    }
    if (!strcmp(arg, "-v") || !strcmp(arg, "--volume") || !strncmp(arg, "--volume=", 9)) {
        return reject_volume_option();
    }
    if (!strcmp(arg, "--cap-add")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "hold: error: --cap-add requires a capability\n");
            return 5;
        }
        int rc = append_string_option("--cap-add", argv[*index + 1], &run->cap_add, &run->cap_addc);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--cap-add=", 10)) {
        int rc = append_string_option("--cap-add", arg + 10, &run->cap_add, &run->cap_addc);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--cap-drop")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "hold: error: --cap-drop requires a capability\n");
            return 5;
        }
        int rc = append_string_option("--cap-drop", argv[*index + 1], &run->cap_drop, &run->cap_dropc);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--cap-drop=", 11)) {
        int rc = append_string_option("--cap-drop", arg + 11, &run->cap_drop, &run->cap_dropc);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--detach-keys")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = configure_detach_keys_option(argv[*index + 1]);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--detach-keys=", 14)) {
        int rc = configure_detach_keys_option(arg + 14);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--restart")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = parse_restart_policy_arg(argv[*index + 1], run->restart_policy);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--restart=", 10)) {
        int rc = parse_restart_policy_arg(arg + 10, run->restart_policy);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--restart-delay")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = parse_restart_delay_arg(argv[*index + 1], &run->restart_delay_seconds);
        if (rc != 0) return rc;
        run->restart_delay_seen = true;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--restart-delay=", 16)) {
        int rc = parse_restart_delay_arg(arg + 16, &run->restart_delay_seconds);
        if (rc != 0) return rc;
        run->restart_delay_seen = true;
        (*index)++;
        return 0;
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
    bool requested_system = false;
    bool tail = false;
    bool console_mode = false;
    bool force_raw = false;
    bool all = false;
    bool multi = false;
    bool quiet = false;
    bool print_cmd = false;
    bool list_iso = false;
    bool rm_force = false;
    struct cli_run_options run = {0};
    int multi_count = 1;

    while (argi < argc) {
        bool parsed_run_option = false;
        int option_rc = parse_run_options(argc, argv, &argi, &run, &requested_system,
                                          &tail, &console_mode, &parsed_run_option);
        if (option_rc != 0) {
            free_run_options(&run);
            return option_rc;
        }
        if (parsed_run_option) {
            continue;
        }
        if (!strcmp(argv[argi], "--system")) {
            requested_system = true;
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
        hold_usage();
        return 5;
    }

    bool owned = !force_raw && !tail && hold_cli_command_is_parser_owned(argv[argi]);
    const char *command = owned ? argv[argi++] : NULL;
    int cmd_argc = 0;
    char **cmd_argv = NULL;
    bool saw_owned_delimiter = false;

    if (owned) {
        cmd_argv = calloc((size_t)(argc - argi + 1), sizeof(char *));
        if (!cmd_argv) {
            return 3;
        }
        bool literal_owned_arg = false;
        for (int i = argi; i < argc; i++) {
            if (!literal_owned_arg && !strcmp(argv[i], "--")) {
                literal_owned_arg = true;
                saw_owned_delimiter = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--system")) {
                requested_system = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--quiet")) {
                quiet = true;
                continue;
            }
            if (!literal_owned_arg && hold_cli_command_allows_all(command) && !strcmp(argv[i], "--all")) {
                all = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "-a") &&
                (!strcmp(command, "ps") || !strcmp(command, "prune") || !strcmp(command, "clean"))) {
                all = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "rm") && !strcmp(argv[i], "--force")) {
                rm_force = true;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "stop") || !strcmp(command, "kill")) &&
                !strcmp(argv[i], "--print")) {
                print_cmd = true;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run"))) {
                bool parsed_run_option = false;
                int option_index = i;
                int option_rc = parse_run_options(argc, argv, &option_index, &run, &requested_system,
                                                  &tail, &console_mode, &parsed_run_option);
                if (option_rc != 0) {
                    free(cmd_argv);
                    free_run_options(&run);
                    return option_rc;
                }
                if (parsed_run_option) {
                    i = option_index - 1;
                    continue;
                }
            }
            if (!literal_owned_arg && (!strcmp(command, "list") || !strcmp(command, "status")) &&
                (!strcmp(argv[i], "--iso") || !strcmp(argv[i], "-l"))) {
                list_iso = true;
                continue;
            }
            if (!literal_owned_arg && command_supports_multiplicity(command) && !strcmp(argv[i], "--force")) {
                multi = true;
                multi_count = 1;
                continue;
            }
            if (!literal_owned_arg && command_supports_multiplicity(command) && !strcmp(argv[i], "--multi")) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "hold: error: --multi requires a positive count\n");
                    free(cmd_argv);
                    return 5;
                }
                int parsed = 0;
                if (!hold_parse_positive_count(argv[i + 1], &parsed)) {
                    fprintf(stderr, "hold: error: invalid --multi count '%s'\n", argv[i + 1]);
                    free(cmd_argv);
                    return 5;
                }
                multi = true;
                multi_count = parsed;
                i++;
                continue;
            }
            if (!literal_owned_arg && command_supports_multiplicity(command) && strncmp(argv[i], "--multi=", 8) == 0) {
                multi = true;
                if (!hold_parse_positive_count(argv[i] + 8, &multi_count)) {
                    fprintf(stderr, "hold: error: invalid --multi count '%s'\n", argv[i] + 8);
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
    if (owned && !saw_owned_delimiter && cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
        int rc = hold_show_help(command);
        free(cmd_argv);
        return rc;
    }
    if (owned && !strcmp(command, "run") && !saw_owned_delimiter && cmd_argc >= 1 &&
        (is_legacy_run_namespace_verb(cmd_argv[0]) ||
         (cmd_argc >= 2 && is_legacy_run_namespace_verb(cmd_argv[1])))) {
        fprintf(stderr, "usage: hold run [run-options] <cmd> [args...]; use -- <cmd> when a command conflicts with Hold syntax\n");
        free(cmd_argv);
        free_run_options(&run);
        return 5;
    }
    if (console_mode && owned && strcmp(command, "start") != 0 && strcmp(command, "run") != 0) {
        fprintf(stderr, "hold: error: --console applies only to starts\n");
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
    if (hold_detect_invocation(&inv, requested_system) != 0) {
        hold_die_errno("hold: failed to resolve invocation context");
    }
    inv.quiet = quiet;
    inv.docker_run = owned && !strcmp(command, "run");
    if ((owned && !strcmp(command, "run")) && !run.detach && (!console_mode || run.tty)) {
        tail = true;
    }
    if (run.detach) {
        tail = false;
    }
    bool interactive_stdin = run.interactive && !run.tty;

    bool docker_ps_command = owned && !strcmp(command, "ps");
    if (owned && !strcmp(command, "logs")) {
        command = "__view";
    }
    if (owned && !strcmp(command, "attach")) {
        command = "console";
    }
    if (docker_ps_command) command = "list";
    if (owned && !strcmp(command, "status")) command = "list";
    if (owned && !strcmp(command, "clean")) command = "prune";

    bool is_list = owned && !strcmp(command, "list");
    /* The root-managed store is visible to everyone (list stays open), but only
     * root may act on it. Delegated execution is gone, so requesting it as a
     * normal user is refused rather than re-run under sudo. */
    if (requested_system && !inv.euid_root && !is_list) {
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

    if (!inv.euid_root || is_list || (owned && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                                               !strcmp(command, "tail") || !strcmp(command, "dump") ||
                                               !strcmp(command, "__view") || !strcmp(command, "prune") ||
                                               !strcmp(command, "console") ||
                                               !strcmp(command, "show") || !strcmp(command, "rm") ||
                                               !strcmp(command, "shell")))) {
        if (!inv.euid_root) {
            if (hold_ensure_user_store_for_current_user(&user_store) != 0) {
                hold_die_errno("hold: failed to init user storage");
            }
        }
    }

    if (owned && !strcmp(command, "run")) {
        struct hold_store start_store;
        if (hold_ensure_start_store_for_command(&inv, requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                hold_start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                hold_die_errno("hold: failed to init invoking-user storage");
            }
            hold_die_errno("hold: failed to init start storage");
        }
        if (multi) {
            fprintf(stderr, "hold: error: --multi applies only to profile starts\n");
            free(cmd_argv);
            free_run_options(&run);
            return 5;
        }
        char *shell_argv[4];
        int start_argc = cmd_argc;
        char **start_argv = cmd_argv;
        if (!saw_owned_delimiter && cmd_argc == 1) {
            shell_argv[0] = "sh";
            shell_argv[1] = "-c";
            shell_argv[2] = cmd_argv[0];
            shell_argv[3] = NULL;
            start_argc = 3;
            start_argv = shell_argv;
        }
        struct hold_start_options opts = start_options_from_run(&run, tail, console_mode,
                                                                interactive_stdin, start_argc, start_argv);
        int rc = hold_perform_start_options(&inv, &start_store, &opts);
        free(cmd_argv);
        free_run_options(&run);
        return rc;
    }

    if (owned && !strcmp(command, "shell")) {
        int rc = hold_cmd_shell_action(&inv, inv.euid_root ? &system_store : &user_store);
        free(cmd_argv);
        return rc;
    }

    if (!owned) {
        struct hold_store start_store;
        if (hold_ensure_start_store_for_command(&inv, requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                hold_start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                hold_die_errno("hold: failed to init invoking-user storage");
            }
            hold_die_errno("hold: failed to init start storage");
        }
        if (run.name) {
            struct hold_start_options opts = start_options_from_run(&run, tail, console_mode,
                                                                    interactive_stdin, cmd_argc, cmd_argv);
            int rc = hold_perform_start_options(&inv, &start_store, &opts);
            free_run_options(&run);
            return rc;
        }
        struct hold_start_options opts = start_options_from_run(&run, tail, console_mode,
                                                                interactive_stdin, cmd_argc, cmd_argv);
        int rc = hold_perform_start_options(&inv, &start_store, &opts);
        free_run_options(&run);
        return rc;
    }

    if (!strcmp(command, "doctor")) {
        printf("hold doctor\n");
        printf("version: %s\n", HOLD_VERSION);
        printf("user_store: %s\n", user_store.base[0] ? user_store.base : "(not initialized)");
        printf("system_store: %s\n", system_store.base);
        free(cmd_argv);
        return 0;
    }

    if (!strcmp(command, "show")) {
        const char *view = cmd_argv[0];
        const char *filter = cmd_argc > 1 ? cmd_argv[1] : NULL;
        int rc = 0;
        if (!strcmp(view, "runs") || !strcmp(view, "running") || !strcmp(view, "active") ||
                   !strcmp(view, "dormant") || !strcmp(view, "inactive") || !strcmp(view, "failed") ||
                   !strcmp(view, "stale") || !strcmp(view, "time") || !strcmp(view, "uptime")) {
            if (filter && !hold_valid_alias(filter)) {
                fprintf(stderr, "hold: error: invalid name '%s'\n", filter);
                free(cmd_argv);
                return 5;
            }
            rc = inv.euid_root ? hold_cmd_list_system(&system_store, filter, list_iso)
                               : hold_cmd_list_normal(&user_store, &system_store, filter, list_iso);
        } else {
            fprintf(stderr, "usage: hold show <runs|running|dormant|failed|stale> [name]\n");
            rc = 5;
        }
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "start")) {
        struct hold_store start_store;
        if (hold_ensure_start_store_for_command(&inv, requested_system, true, command, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                hold_start_target_is_within_invoking_home(&inv, true, command, cmd_argc, cmd_argv)) {
                hold_die_errno("hold: failed to init invoking-user storage");
            }
            hold_die_errno("hold: failed to init start storage");
        }
        int rc = hold_cmd_start_action_options(&inv, &user_store, &system_store, &start_store, tail, console_mode, run.auto_remove, interactive_stdin, multi, multi_count, run.restart_policy[0] ? run.restart_policy : NULL, run.restart_delay_seconds, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "list")) {
        int rc;
        if (docker_ps_command) {
            if (cmd_argc != 0) {
                fprintf(stderr, "usage: hold ps [-a|--all]\n");
                free(cmd_argv);
                return 5;
            }
            rc = inv.euid_root ? hold_cmd_ps_system(&system_store, all)
                               : hold_cmd_ps_normal(&user_store, &system_store, all);
            free(cmd_argv);
            return rc;
        }
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: hold list [profile]\n");
            free(cmd_argv);
            return 5;
        }
        const char *alias_filter = cmd_argc == 1 ? cmd_argv[0] : NULL;
        if (alias_filter && !hold_valid_alias(alias_filter)) {
            fprintf(stderr, "hold: error: invalid profile '%s'\n", alias_filter);
            free(cmd_argv);
            return 5;
        }
        if (inv.euid_root) {
            rc = hold_cmd_list_system(&system_store, alias_filter, list_iso);
        } else {
            rc = hold_cmd_list_normal(&user_store, &system_store, alias_filter, list_iso);
        }
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "tail")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: hold tail <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = hold_cmd_tail_action(&inv, &user_store, &system_store, cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "inspect")) {
        if (cmd_argc != 1) {
            fprintf(stderr, "usage: hold inspect <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = hold_cmd_inspect_action(&inv, &user_store, &system_store, cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "dump")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: hold dump <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = hold_cmd_dump_action(&inv, &user_store, &system_store, cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "__view")) {
        int rc = hold_cmd_view_action(&inv, &user_store, &system_store, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "console")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: hold console <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = hold_cmd_console_action(&inv, &user_store, &system_store, cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "prune")) {
        const char *target = cmd_argc > 0 ? cmd_argv[0] : NULL;
        if (target && target[0] == '-') {
            fprintf(stderr, "hold: error: unknown flag '%s'\n", target);
            print_command_usage_stderr("prune");
            free(cmd_argv);
            return 1;
        }
        int rc = hold_cmd_prune_action(&inv, &user_store, &system_store, target, all);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "rm")) {
        const char *target = cmd_argv[0];
        int rc = 0;
        if (rm_force) {
            rc = hold_cmd_signal_action(&inv, &user_store, &system_store, "stop", 1, cmd_argv, SIGTERM, true, false, false);
            if (rc != 0) {
                free(cmd_argv);
                return rc;
            }
        }
        rc = hold_cmd_prune_action(&inv, &user_store, &system_store, target, false);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "stop")) {
        int rc = hold_cmd_signal_action(&inv, &user_store, &system_store, "stop", cmd_argc, cmd_argv, SIGTERM, true, all, print_cmd);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "kill")) {
        int rc = hold_cmd_signal_action(&inv, &user_store, &system_store, "kill", cmd_argc, cmd_argv, SIGKILL, false, all, print_cmd);
        free(cmd_argv);
        return rc;
    }

    free(cmd_argv);
    hold_usage();
    return 1;
}
