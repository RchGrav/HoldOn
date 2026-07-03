#include "hold/config.h"
#include "hold/types.h"
#include "hold/cli.h"
#include "hold/runtime.h"

static int help_targets(void);
static int help_system(void);
static int help_scripting(void);
static int help_console(void);
static int help_action(const char *action);

enum {
    HOLD_CLI_ALLOW_ALL = 1 << 0,
    HOLD_CLI_ALLOW_DDASH = 1 << 1
};

struct hold_cli_command_spec {
    const char *name;
    int min_args;
    int max_args; /* -1 means unbounded. */
    unsigned flags;
    const char *usage;
    const char *help_topic;
};

static const struct hold_cli_command_spec command_specs[] = {
    {"list", 0, 1, 0, "usage: hold list [name]", "list"},
    {"ps", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold ps [-a|--all]", "ps"},
    {"run", 1, -1, HOLD_CLI_ALLOW_DDASH, "usage: hold run [run-options] <cmd> [args...]", "run"},
    {"start", 1, -1, HOLD_CLI_ALLOW_DDASH, "usage: hold start <id|name>\n       hold start <cmd> [args...]", "start"},
    {"stop", 1, -1, HOLD_CLI_ALLOW_ALL, "usage: hold stop [--print] [--all] <target>...", "stop"},
    {"kill", 1, -1, HOLD_CLI_ALLOW_ALL, "usage: hold kill [--print] [--all] <target>...", "kill"},
    {"tail", 1, 1, 0, "usage: hold tail <target>", "tail"},
    {"logs", 1, -1, 0, "usage: hold logs <target> [--follow|-f] [--tail|-n N] [--plain|--interactive]", "logs"},
    {"status", 0, 1, 0, "usage: hold status [target]", "status"},
    {"inspect", 1, 1, 0, "usage: hold inspect <target>", "inspect"},
    {"dump", 1, 1, 0, "usage: hold dump <target>", "dump"},
    {"__view", 1, -1, 0, "usage: hold __view <target> [internal viewer test options]", "__view"},
    {"console", 1, 1, 0, "usage: hold console <target>", "console"},
    {"attach", 1, 1, 0, "usage: hold attach <target>", "attach"},
    {"prune", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold prune [target|all] [--all]", "prune"},
    {"rm", 1, 1, 0, "usage: hold rm [--force] <inactive-runid>", "rm"},
    {"show", 1, 2, 0, "usage: hold show <runs|running|dormant|failed|stale> [name]", "show"},
    {"clean", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold clean [target|all]", "clean"},
    {"doctor", 0, 0, 0, "usage: hold doctor", "doctor"},
    {"shell", 0, 0, 0, "usage: hold shell", "shell"},
    {"help", 0, 1, 0, "usage: hold help [topic]", "help"},
};

static const struct hold_cli_command_spec *find_public_command_spec(const char *s) {
    if (!s) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(command_specs) / sizeof(command_specs[0]); i++) {
        if (!strcmp(command_specs[i].name, s)) {
            return &command_specs[i];
        }
    }
    return NULL;
}

static int help_targets(void) {
    printf("hold help targets\n\n"
           "  <target>          resolve in the current context\n"
           "  user:<target>     force user-local lookup\n"
           "  system:<target>   force root-managed lookup\n\n"
           "target = run id, leading id prefix, or run name\n\n"
           "A run id addresses one run directly, always. A run name resolves to the\n"
           "single run recorded under that name, narrowed by the verb: stop/kill/logs\n"
           "look at running or logged runs, inspect looks at retained records, and prune\n"
           "looks at removable past run data.\n");
    return 0;
}

static int help_system(void) {
    printf("hold help system\n\n"
           "Root, sudo, and --system runs use the root-managed store:\n\n"
           "  Linux: /var/lib/hold\n"
           "  macOS: /var/db/hold\n\n"
           "Private root records and logs stay root-only. Normal users see only the\n"
           "redacted public index. Acting on a root-managed target requires root;\n"
           "user-local targets win over root-public collisions.\n\n"
           "  hold --system <cmd...>       start in root-managed state\n"
           "  hold --system list           list authoritative root records\n");
    return 0;
}

static int help_scripting(void) {
    printf("hold help scripting\n\n"
           "stdout is for machine data. Human banners, confirmations, warnings, and\n"
           "errors go to stderr; --quiet suppresses normal human status.\n\n"
           "  id=$(hold -d <cmd...>)       capture the bare 12-hex run id\n"
           "  hold stop --print <id>       print kill -TERM -- -<pgid>\n"
           "  hold kill --print <id>       print kill -KILL -- -<pgid>\n\n"
           "Exit codes:\n"
           "  0  success (includes known name with nothing to do)\n"
           "  1  usage / generic error\n"
           "  2  refused for safety\n"
           "  3  permission denied or storage/security failure\n"
           "  4  signal delivery failed\n"
           "  5  target not found or invalid target\n"
           "  6  must disambiguate\n");
    return 0;
}

static int help_console(void) {
    printf("hold help console\n\n"
           "Start a run with an attachable PTY console, then reconnect to it later.\n"
           "Console output is still tee'd to the normal log, so logs continue\n"
           "to work.\n\n"
           "  hold -t <cmd...>             start with an attachable PTY console\n"
           "  hold attach <target>         reconnect to a running console/TTY run\n\n"
           "Console attach is native: Hold saves your terminal, enters an alternate\n"
           "screen for interactive attaches, forwards terminal size changes to the PTY,\n"
           "and restores your original screen on exit. Ctrl-P Ctrl-Q detaches without\n"
           "ending the run.\n");
    return 0;
}

static int help_action(const char *action) {
    if (!strcmp(action, "list")) {
        printf("usage: hold list [name] [--iso|-l]\n\nShow all visible runs, optionally filtered by run name.\n");
    } else if (!strcmp(action, "ps")) {
        printf("usage: hold ps [-a|--all]\n\nDocker-shaped run listing. Shows Hold run IDs and names.\n");
    } else if (!strcmp(action, "start")) {
        printf("usage: hold start <id|name>\n       hold start <cmd> [args...]\n\nRestart a retained run by id or name, or use the explicit start form for a raw command.\n");
    } else if (!strcmp(action, "stop")) {
        printf("usage: hold stop [--print] [--all] <target>...\n\nGracefully stop matching runs with TERM, then KILL if needed.\n");
    } else if (!strcmp(action, "kill")) {
        printf("usage: hold kill [--print] [--all] <target>...\n\nForce matching runs down with KILL.\n");
    } else if (!strcmp(action, "run")) {
        printf("usage: hold run [run-options] <cmd> [args...]\n\nDocker-shaped launch. Without -d, Hold starts the run and follows its log in the foreground.\nCommon options:\n  -d, --detach          run in the background and print the run ID\n  -i, --interactive     keep non-PTY stdin open\n  -t, --tty             allocate Hold's PTY/console path\n  -e, --env KEY=VALUE   set launch environment\n      --env-file FILE   load KEY=VALUE launch environment lines\n  -p, --publish SPEC    unsupported: Hold observes in-use ports in `hold ps`\n  -v, --volume SPEC     unsupported: pass host paths directly; no mounts/remaps\n      --rm              remove run record/log after exit\n      --restart POLICY  restart rule: no|always|unless-stopped|on-failure[:N]\n      --restart-delay N delay seconds between restart attempts\n      --name NAME       assign this run's container-style name\n      --detach-keys SEQ set TTY detach keys (default ctrl-p,ctrl-q)\nUse -- before a command whose name conflicts with a Hold command or option.\n");
    } else if (!strcmp(action, "tail") || !strcmp(action, "logs")) {
        if (!strcmp(action, "logs")) {
            printf("usage: hold logs <target> [--follow|-f] [--tail|-n N] [--plain|--interactive]\n\nOpen the log viewer for a run. In a TTY, type directly in the full-screen viewer to filter dynamically; Backspace relaxes the filter, Space excludes lines like the highlighted line, and Ctrl-R resets filters. Non-TTY output stays script-friendly.\n");
        } else {
            printf("usage: hold tail <target>\n\nFollow live output for a run name match, or follow an id's log directly.\n");
        }
    } else if (!strcmp(action, "console")) {
        printf("usage: hold console <target>\n\nAttach to a running console-enabled run. Prefer Docker-shaped `hold -it <cmd>`.\n");
    } else if (!strcmp(action, "attach")) {
        printf("usage: hold attach <target>\n\nAttach your terminal to a running console/TTY run (Docker-style). Detach again with Ctrl-P Ctrl-Q. Start attachable runs with `hold -it <cmd>`.\n");
    } else if (!strcmp(action, "dump")) {
        printf("usage: hold logs <target> --plain\n\nThe public 0.4 log-text command is `hold logs <target> --plain`; structured details are `hold inspect <target>`.\n");
    } else if (!strcmp(action, "__view")) {
        printf("usage: hold __view <target> [internal viewer test options]\n\nInternal regression/debug entrypoint for the log viewer engine. The product UX is hold logs <target>, then type inside the full-screen viewer to filter dynamically.\n");
    } else if (!strcmp(action, "prune")) {
        printf("usage: hold prune [target|all] [--all]\n\nClear removable past run data. Running valid runs are never pruned.\n");
    } else if (!strcmp(action, "rm")) {
        printf("usage: hold rm [--force] <inactive-runid>\n\nRemove an inactive run record/log. With --force, stop and remove one concrete active run ID.\n");
    } else if (!strcmp(action, "status")) {
        printf("usage: hold status [target]\n\nShow runs, optionally narrowed by target.\n");
    } else if (!strcmp(action, "inspect")) {
        printf("usage: hold inspect <target>\n\nPrint structured JSON details for a run target. Log text belongs to `hold logs <target> --plain`.\n");
    } else if (!strcmp(action, "show")) {
        printf("usage: hold show <runs|running|dormant|failed|stale> [name]\n\nNavigate alternate views of the same runtime tree.\n");
    } else if (!strcmp(action, "clean")) {
        printf("usage: hold clean [target|all]\n\nClear removable past run data.\n");
    } else if (!strcmp(action, "doctor")) {
        printf("usage: hold doctor\n\nCheck local Hold paths and build identity.\n");
    } else if (!strcmp(action, "shell")) {
        printf("usage: hold shell\n\nStart an ordinary user shell under Hold's PTY/session wrapper. Typing `exit` returns without creating a runid. Pressing the classic detach sequence Ctrl-P Ctrl-Q captures the current foreground process group as a Hold run and returns to the caller.\n");
    } else {
        return -1;
    }
    return 0;
}

int hold_show_help(const char *topic) {
    if (!topic || !*topic) {
        hold_usage();
        return 0;
    }
    if (!strcmp(topic, "targets")) return help_targets();
    if (!strcmp(topic, "system")) return help_system();
    if (!strcmp(topic, "scripting")) return help_scripting();
    if (!strcmp(topic, "console")) return help_console();
    if (help_action(topic) == 0) return 0;
    fprintf(stderr, "hold: unknown help topic '%s'\n", topic);
    return 5;
}

bool hold_cli_command_is_parser_owned(const char *s) {
    return find_public_command_spec(s) != NULL;
}

bool hold_cli_command_is_public(const char *s) {
    return find_public_command_spec(s) != NULL;
}

bool hold_cli_command_allows_all(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    return spec && (spec->flags & HOLD_CLI_ALLOW_ALL);
}

const char *hold_cli_command_usage(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    return spec ? spec->usage : NULL;
}

int hold_validate_owned_command_arity(const char *command, int argc) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(command);
    if (!spec) {
        return 0;
    }
    if (argc < spec->min_args || (spec->max_args >= 0 && argc > spec->max_args)) {
        fprintf(stderr, "%s\n", spec->usage);
        return 5;
    }
    return 0;
}

bool hold_parse_positive_count(const char *s, int *out) {
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
