/* cli/specs.c — command_specs[] and the flag engine that runs off it.
 *
 * One row per spelled verb: aliases are rows whose canon names the dispatch
 * verb they fold onto. Arity, the per-command flag mask, the usage line, and
 * the help prose all live in the row, so parsing, arity guards, usage text,
 * and help are driven from this one table; no verb parses its own flags by
 * hand. */
#include "hold/config.h"
#include <stddef.h>
#include "hold/types.h"
#include "hold/cli.h"
#include "hold/runtime.h"

/* Per-command flag mask: which owned flags a verb consumes. */
enum {
    SPEC_ALL       = 1 << 0, /* --all; -a is accepted wherever --all is */
    SPEC_LIVE      = 1 << 1, /* -l/--live, list's running-only filter */
    SPEC_SCOPE     = 1 << 2, /* -s/--system and -u/--user store scope */
    SPEC_FORCE     = 1 << 3, /* --force (purge family) */
    SPEC_PRINT     = 1 << 4, /* --print (end/stop/kill) */
    SPEC_NOSTREAM  = 1 << 5, /* --no-stream (stats) */
    SPEC_STRICT    = 1 << 6, /* reject any unconsumed flag (list/ps) */
    SPEC_PS_DOCKER = 1 << 7  /* ps speaks only Docker: --system not consumed */
};

struct hold_cli_command_spec {
    const char *name;
    const char *canon;      /* dispatch verb aliases fold onto; NULL = itself */
    int min_args;
    int max_args;           /* -1 means unbounded. */
    unsigned flags;
    const char *usage;
    const char *help_usage; /* help's usage line when it differs; NULL = usage */
    const char *help;       /* prose body; NULL = not a help topic */
};

/* Help prose, shared between a verb and its aliases. Prose is content, not
 * code: only its duplicated usage lines compress (help prints the row's
 * usage line above the body). */
static const char help_list[] =
    "list is Hold's scoped ledger, with a USER column (CALL ID, USER, COMMAND, CREATED,\n"
    "STATUS, PORTS, NAMES). By default you see your own calls, live and past. Scope flags:\n"
    "  -u/--user     your personal calls only (even under sudo: the invoking user's)\n"
    "  -s/--system   the global (root-managed) calls only\n"
    "  -a/--all      both scopes together\n"
    "  -l/--live     narrow any of the above to running calls (composes)\n\n"
    "Global calls a normal user is not entitled to read appear redacted: USER and COMMAND\n"
    "read 'hidden'. Run as root, list defaults to the global calls; -a also walks every\n"
    "user's store. An optional name narrows the listing. (For Docker's running view, see ps.)\n";
static const char help_ps[] =
    "ps is Docker's machine-wide view: running calls (add -a for ended too) across both your\n"
    "own calls and the global ones, rendered as Docker's table without an IMAGE or USER\n"
    "column (CALL ID, COMMAND, CREATED, STATUS, PORTS, NAMES). Global calls you cannot read\n"
    "show a 'hidden' COMMAND. ps speaks only Docker: it takes no scope flags. For Hold's\n"
    "scoped, past-inclusive ledger with a USER column, use list.\n";
static const char help_end[] =
    "End matching calls politely: TERM, then KILL if needed. (stop is an alias.)\n";
static const char help_kill[] =
    "Force matching calls down with KILL.\n";
static const char help_tail[] =
    "Follow a call's live output. Shorthand for hold logs <target> -f.\n";
static const char help_logs[] =
    "Print a call's captured output as plain text. -f follows a running call until it ends; -n N prints the newest N lines. For the live screen of a TTY call, use hold peek.\n";
static const char help_peek[] =
    "Show a TTY call's screen as it is right now. By default prints the visible screen as plain text, one line per row. --ruler adds column and row numbers. --json adds the cursor, terminal modes, title, output counter and idle time. --image FILE writes a color PNG with a coordinate ruler (the cursor's column and row marked in red; --no-ruler drops it); --cursor-ring circles the cursor and --scale N enlarges it. Run hold wait first (or hold send --wait) so you do not read a half-drawn frame. After a call ends, peek shows its last screen. --last-alt shows the last full-screen (alternate-screen) frame a program like vim or less displayed before it switched back.\n";
static const char help_send[] =
    "Type into a TTY call without attaching. Arguments are applied in order: TEXT is typed literally (--text TEXT for text starting with -), -k KEY presses a key (Enter, Tab, Escape, Backspace, Up, Down, Left, Right, Home, End, PageUp, PageDown, Insert, Delete, F1-F24, Space, with C- M- S- modifiers such as C-c or M-x), --keys \"K1 K2\" presses several, --enter presses Enter, --paste TEXT pastes (bracketed when the program asked for it; - reads stdin), --stdin types stdin, and --mouse ACTION,COL,ROW[,BUTTON] sends a mouse event (click, press, release, move, drag, scroll-up, scroll-down; exit 9 if the program has mouse reporting off). Keys are encoded for the program's current modes. --wait MS then waits until the program has answered and gone quiet for MS (bounded by --timeout, default 10s). --seq prints the output counter for hold wait --since.\n";
static const char help_wait[] =
    "Block until a condition holds, so a script or agent never has to guess with sleep. Wait before you peek: right after a send, the screen may be only half drawn (hold send --wait MS sends and waits in one step). --idle [MS]: the program has drawn something and the screen then stays quiet for MS, 500 if MS is left out (counted from when the wait began; with --since SEQ, output after SEQ is required). Silence before the first output never counts, so on a program that prints nothing (cat, a quiet server) --idle runs to the timeout, with or without --since 0; use --match, --exit or peek there. --match TEXT / --regex RE: the text is on the screen (already there counts, unless --since SEQ: then only after output past SEQ). --exit or no condition: the call ends; prints its exit code (docker wait). On an --rm call it returns once the call is removed, so its name is free; for a minute after an --rm call is removed, wait still reports how it exited. Screen conditions time out after 30s unless --timeout MS says otherwise (0 = never). Exit status: 0 met, 7 timed out, 8 the call ended first.\n";
static const char help_resize[] =
    "Resize a TTY call's terminal to <cols> <rows> (or <cols>x<rows>). The program receives SIGWINCH and the screen hold peek shows reflows with it.\n";
static const char help_msg[] =
    "Message another call, typically an agent harness running under Hold. The text is typed into its prompt as \"[msg ID from <you>; reply: hold msg <you> \\\"...\\\"; verify: hold msg --received ID] TEXT\" and submitted with Enter, after its screen has been quiet for --settle MS (default 1s; --now skips the wait; --timeout MS, default 60s, delivers anyway). <you> is your own call ($HOLD_CALL_NAME, set in every call), verified by the receiver; from outside any call the header is [msg via hold]. Multi-line text goes in as one bracketed paste when the program supports it. --no-enter types without submitting; --stdin reads the text from stdin. Delivery is capped at 20 messages a minute per receiver. A header is only text, and anything can print one; hold msg --received ID (run inside the receiving call, or hold msg CALL --received ID) prints the sender and full text of the message hold delivered under that ID, and fails if there was none, so an agent can tell a real message from a forged header. hold msg [CALL] --received lists every message delivered to the call.\n";
static const char help_attach[] =
    "Pick a running console/TTY call back up. Detach again with Ctrl-P Ctrl-Q. Start attachable calls with hold -it <cmd>. (console is an alias of attach.)\n";
static const char help_purge[] =
    "The one removal verb. With no target it sweeps ended calls; -a includes stale (a state\n"
    "widener, not a scope one). Scope: -u/--user sweeps your personal calls (the default);\n"
    "-s/--system sweeps the global store and, if you are not root, re-execs through sudo so\n"
    "sudo can prompt for the password. A target removes one call; --force removes regardless\n"
    "of state (live or saved). rm, prune, and drop are accepted aliases.\n";
static const char help_inspect[] =
    "Print structured JSON details for a call. For a live call the output includes a Stdio object showing where fds 0/1/2 currently point. Log text belongs to hold logs <target> --plain.\n";
static const char help_ports[] =
    "List the listening sockets in use by the call's process group, one per line (\"127.0.0.1:8080/tcp\"). Your own calls need no root.\n";
static const char help_stats[] =
    "Live resource usage for the call's process group: CPU %, resident memory, and process count, refreshing in place every second until Ctrl-C. Prints a single frame when stdout is not a TTY or with --no-stream.\n";
static const char help_save[] =
    "Protect a call from purge. There is no unsave; use hold purge --force to remove a saved call.\n";
static const char help_rename[] =
    "Rename a call. Live and ended calls are both renameable. Naming a call\ndeclares you want to keep it, so rename also saves it (protects it from\npurge); a targeted `hold purge <name> --force` still removes it.\n";

/* The purge family's help documents the scope flags its arity usage omits. */
static const char usage_purge_help[] =
    "usage: hold purge [<target>] [-a|--all] [-s|--system] [-u|--user] [--force]";
static const char usage_end[] = "usage: hold end [--print] [--all] <target>...";
static const char usage_attach[] = "usage: hold attach <target>";

#define SPEC_PURGE_FLAGS (SPEC_ALL | SPEC_SCOPE | SPEC_FORCE)

static const struct hold_cli_command_spec command_specs[] = {
    {"list", NULL, 0, 1, SPEC_ALL | SPEC_LIVE | SPEC_SCOPE | SPEC_STRICT,
     "usage: hold list [-a|--all] [-s|--system] [-u|--user] [-l|--live] [name]", NULL, help_list},
    {"ps", NULL, 0, 1, SPEC_ALL | SPEC_STRICT | SPEC_PS_DOCKER,
     "usage: hold ps [-a|--all] [name]", NULL, help_ps},
    {"end", NULL, 1, -1, SPEC_ALL | SPEC_PRINT, usage_end, NULL, help_end},
    {"stop", "end", 1, -1, SPEC_ALL | SPEC_PRINT,
     "usage: hold stop [--print] [--all] <target>...", usage_end, help_end},
    {"kill", NULL, 1, -1, SPEC_ALL | SPEC_PRINT,
     "usage: hold kill [--print] [--all] <target>...", NULL, help_kill},
    {"tail", NULL, 1, 1, 0, "usage: hold tail <target>", NULL, help_tail},
    {"logs", NULL, 1, -1, 0,
     "usage: hold logs <target> [--follow|-f] [--tail|-n N]", NULL, help_logs},
    {"inspect", NULL, 1, 1, 0, "usage: hold inspect <target>", NULL, help_inspect},
    {"ports", NULL, 1, 1, 0, "usage: hold ports <target>", NULL, help_ports},
    {"stats", NULL, 1, 1, SPEC_NOSTREAM, "usage: hold stats <target> [--no-stream]", NULL, help_stats},
    {"peek", NULL, 1, -1, 0,
     "usage: hold peek <target> [--json] [--ruler] [--last-alt] [--image FILE [--no-ruler] [--cursor-ring] [--scale N]]", NULL, help_peek},
    {"send", NULL, 1, -1, 0,
     "usage: hold send <target> [TEXT]... [--text TEXT] [-k KEY]... [--keys \"K1 K2\"] [--enter] [--paste TEXT|-]\n"
     "                 [--mouse ACTION,COL,ROW[,BUTTON]] [--stdin] [--wait MS [--timeout MS]] [--seq]", NULL, help_send},
    {"wait", NULL, 1, -1, 0,
     "usage: hold wait <target> [--idle [MS]] [--match TEXT | --regex RE] [--since SEQ] [--exit] [--timeout MS]", NULL, help_wait},
    {"msg", NULL, 1, -1, 0,
     "usage: hold msg <target> TEXT... [--stdin] [--now] [--settle MS] [--timeout MS] [--no-enter]\n"
     "       hold msg [<target>] --received [ID]", NULL, help_msg},
    {"resize", NULL, 1, 3, 0, "usage: hold resize <target> <cols> <rows>", NULL, help_resize},
    {"attach", NULL, 1, 1, 0, usage_attach, NULL, help_attach},
    {"console", "attach", 1, 1, 0, "usage: hold console <target>", usage_attach, help_attach},
    {"purge", NULL, 0, 1, SPEC_PURGE_FLAGS,
     "usage: hold purge [<target>] [-a|--all] [--force]", usage_purge_help, help_purge},
    {"prune", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: hold purge [<target>] [-a|--all] [--force]", usage_purge_help, help_purge},
    {"rm", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: hold purge [<target>] [--force]", usage_purge_help, help_purge},
    {"drop", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: hold purge [<target>] [-a|--all] [--force]", usage_purge_help, help_purge},
    {"save", NULL, 1, 1, 0, "usage: hold save <target>", NULL, help_save},
    {"rename", NULL, 2, 2, 0, "usage: hold rename <target> <name>", NULL, help_rename},
    {"help", NULL, 0, 1, 0, "usage: hold help [topic]", NULL, NULL},
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

/* The spelled flags the owned sweep can consume; each spec's mask says which
 * apply. -a rides SPEC_ALL: it is accepted wherever --all is. */
static const struct owned_flag {
    const char *shortf; /* NULL when the flag has no short spelling */
    const char *longf;
    unsigned bit;
    size_t out; /* offsetof the bool it sets in hold_cli_owned_opts */
} owned_flags[] = {
    {"-a", "--all", SPEC_ALL, offsetof(struct hold_cli_owned_opts, all)},
    {"-l", "--live", SPEC_LIVE, offsetof(struct hold_cli_owned_opts, live_only)},
    {"-s", "--system", SPEC_SCOPE, offsetof(struct hold_cli_owned_opts, system_scope)},
    {"-u", "--user", SPEC_SCOPE, offsetof(struct hold_cli_owned_opts, user_scope)},
    {NULL, "--force", SPEC_FORCE, offsetof(struct hold_cli_owned_opts, force)},
    {NULL, "--print", SPEC_PRINT, offsetof(struct hold_cli_owned_opts, print_cmd)},
    {NULL, "--no-stream", SPEC_NOSTREAM, offsetof(struct hold_cli_owned_opts, no_stream)},
};

/* Sweep an owned command's argv: consume the flags its spec allows (plus the
 * global --system/--quiet), collect everything else into cmd_argv. A literal
 * `--` passes the rest through untouched. Strict verbs (list/ps) reject any
 * flag they did not consume rather than misreading it as a name filter.
 * Returns 0, or 1 after printing the unknown-flag error and usage. */
int hold_cli_collect_owned_args(const char *command, int argc, char **argv,
                                char **cmd_argv, int *cmd_argc,
                                struct hold_cli_owned_opts *opts) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(command);
    unsigned allowed = spec ? spec->flags : 0;
    bool literal = false;
    for (int i = 0; i < argc; i++) {
        char *arg = argv[i];
        if (!literal) {
            if (!strcmp(arg, "--")) {
                literal = true;
                opts->saw_delimiter = true;
                continue;
            }
            /* --system selects the root-managed store for every owned verb
             * except ps, which speaks only Docker and takes no scope flag. */
            if (!(allowed & SPEC_PS_DOCKER) && !strcmp(arg, "--system")) {
                opts->requested_system = true;
                continue;
            }
            if (!strcmp(arg, "--quiet")) {
                opts->quiet = true;
                continue;
            }
            const struct owned_flag *f = NULL;
            for (size_t j = 0; j < sizeof(owned_flags) / sizeof(owned_flags[0]); j++) {
                if ((owned_flags[j].shortf && !strcmp(arg, owned_flags[j].shortf)) ||
                    !strcmp(arg, owned_flags[j].longf)) {
                    f = &owned_flags[j];
                    break;
                }
            }
            if (f && (allowed & f->bit)) {
                *(bool *)((char *)opts + f->out) = true;
                continue;
            }
            if ((allowed & SPEC_STRICT) && arg[0] == '-') {
                fprintf(stderr, "hold: error: unknown flag '%s'\n", arg);
                if (spec) {
                    fprintf(stderr, "%s\n", spec->usage);
                }
                return 1;
            }
        }
        cmd_argv[(*cmd_argc)++] = arg;
    }
    return 0;
}

static int help_targets(void) {
    printf("hold help targets\n\n"
           "  <target>          resolve in the current context\n"
           "  user:<target>     force user-local lookup\n"
           "  system:<target>   force root-managed lookup\n\n"
           "target = call id, leading id prefix, or call name\n\n"
           "A call id addresses one call directly, always. A call name resolves to the\n"
           "single call recorded under that name, narrowed by the verb: end/kill/logs\n"
           "look at running or logged calls, inspect looks at retained records, and purge\n"
           "looks at removable past call data.\n");
    return 0;
}

static int help_system(void) {
    printf("hold help system\n\n"
           "Root and sudo calls use the root-managed store:\n\n"
           "  Linux: /var/lib/hold\n"
           "  macOS: /var/db/hold\n\n"
           "Private root records and logs stay root-only. Normal users see only the\n"
           "redacted public index, where a call's USER and COMMAND read 'hidden'.\n"
           "Acting on a root-managed target requires root; user-local targets win\n"
           "over root-public collisions.\n\n"
           "  sudo hold <cmd...>           start in root-managed state\n"
           "  sudo hold list               list the global calls (authoritative)\n"
           "  sudo hold list -a            global calls plus every user's calls\n"
           "  hold list -s                 the redacted global view, as a normal user\n"
           "  hold purge -s                sweep the global store (re-execs via sudo)\n");
    return 0;
}

static int help_scripting(void) {
    printf("hold help scripting\n\n"
           "stdout is for machine data. Human banners, confirmations, warnings, and\n"
           "errors go to stderr; --quiet suppresses normal human status.\n\n"
           "  id=$(hold -d <cmd...>)       capture the bare 64-hex call id\n"
           "  hold end --print <id>        print kill -TERM -- -<pgid>\n"
           "  hold kill --print <id>       print kill -KILL -- -<pgid>\n\n"
           "Exit codes:\n"
           "  0  success (includes known name with nothing to do)\n"
           "  1  usage / generic error\n"
           "  2  refused for safety\n"
           "  3  permission denied or storage/security failure\n"
           "  4  signal delivery failed\n"
           "  5  target not found or invalid target\n"
           "  6  must disambiguate\n"
           "  7  hold wait timed out\n"
           "  8  the call ended before the wait condition held\n"
           "  9  send --mouse: the program has mouse reporting off (not delivered)\n");
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
    const struct hold_cli_command_spec *spec = find_public_command_spec(topic);
    if (spec && spec->help) {
        printf("%s\n\n%s", spec->help_usage ? spec->help_usage : spec->usage, spec->help);
        return 0;
    }
    fprintf(stderr, "hold: unknown help topic '%s'\n", topic);
    return 5;
}

bool hold_cli_command_is_parser_owned(const char *s) {
    return find_public_command_spec(s) != NULL;
}

const char *hold_cli_command_usage(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    return spec ? spec->usage : NULL;
}

const char *hold_cli_command_canon(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    if (!spec) return s;
    return spec->canon ? spec->canon : spec->name;
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
