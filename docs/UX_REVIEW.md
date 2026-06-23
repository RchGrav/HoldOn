# Sigmund CLI UX review

Date: 2026-06-23

## Executive summary

## Focused 0.4 spec draft

The implementation-oriented version of these decisions now lives in [Mund 0.4 UX and CLI specification draft](MUND_0_4_UX_SPEC.md), including the detailed pager/live-filter/similarity-filter requirements.


Sigmund already has a strong product core: `sigmund <cmd>` is simpler than `nohup`, safer than PID files, and far lighter than `systemd`. The current CLI is scriptable and technically coherent, but it exposes internal concepts too early: run IDs, aliases, profiles, public/root stores, target scopes, grants, console mode, pruning, and validation semantics all appear as separate pieces the user must assemble.

The highest-leverage UX move is to add an interactive command mode that turns Sigmund from a collection of verbs into a guided workspace for long-running jobs. This should not replace the existing CLI contract; it should sit above it as a discoverable, forgiving, human-first layer while preserving the current stdout/stderr behavior for scripts.

Recommended direction:

1. Move to one unified stacked command grammar shared by normal CLI, captive shell, and Cisco-style config files.
2. Make `profile`, `run`, `show`, `start`, `stop`, `logs`, `grant`, `clean`, `doctor`, `import`, and `export` the primary language everywhere.
3. Let users stack captive-shell commands directly from the normal prompt, e.g. `sigmund profile web set env PORT=3000`, `sigmund show profile web`, `sigmund run web logs --follow`.
4. Make this a deliberate breaking CLI redesign for 0.4.0: replace current legacy commands (`alias`, `aliases`, `list`, `tail`, `dump`, `console`, `prune`, action-first forms) with the unified stacked grammar instead of carrying them as primary UX.
5. Add a profile editor: `sigmund profile web edit` and an interactive `profile web` submode for advanced recipe, cwd, env, console, restart/readiness metadata, and access policy.
6. Add profile config import/export so every captive-shell edit can be represented as a Cisco-style command transcript, while JSON remains canonical on disk.
7. Add safer lifecycle affordances: adopt current run into profile, “start or attach/tail”, “doctor/status”, suggested next commands, and cleanup prompts.

## Current UX strengths

- **Best possible first command**: `sigmund <command> [args...]` starts a background process without requiring config.
- **Scriptability is protected**: successful starts print only the run ID to stdout; banners go to stderr.
- **Safety story is excellent**: Sigmund validates process identity before signaling, and refuses unsafe actions.
- **Short run IDs are approachable**: 8 hex chars are easier than full UUIDs.
- **Docs are unusually complete**: README, quickstart, technical loop, CLI contract, profiles, security, and console docs exist.
- **Root delegation is differentiated**: scoped sudoers-managed aliases are a powerful capability few small launchers offer.

## Current UX friction

### 1. The human workflow is still command-memory heavy

The help text is concise, but users still need to remember which verb applies now: `list`, `tail`, `dump`, `console`, `stop`, `kill`, `prune`, `alias`, `aliases`, `grant`, `revoke`, `start`, plus target scoping. This is fine for scripts, but not “embarrassingly easy”.

Suggested fix: add an interactive dashboard / command mode where users can type `?`, select a run, and see valid actions.

### 2. “Alias” is underpowered as the main reusable concept

The docs already discuss protected profiles, but the user-facing command is `alias`. For advanced features, “alias” sounds like a shell nickname, not a managed reusable launch profile with policy, console behavior, environment, cwd, readiness checks, and access controls.

Suggested fix: introduce `profile` as the primary noun and keep `alias` as a friendly backwards-compatible shortcut.

### 3. Alias creation from a running command does not adopt that running command

Observed behavior:

```sh
id=$(sigmund sleep 20)
sigmund alias "$id" web
sigmund start web
```

This starts a second `sleep 20` because the original run was not labeled `web`; future alias starts are labeled, but the source run is not. The docs say aliases allow later commands to use the name, and also say `start <alias>` refuses if that alias already has a running process. That is technically true only after a run was started through the alias, but it is surprising immediately after alias creation.

Suggested fix options:

- Prefer: `sigmund alias <id> <name>` should label/adopt the source run as `<name>` by default.
- Or: print a warning and next step: “Pinned recipe as web. The existing run is still 04a7dda8; use `sigmund adopt 04a7dda8 web` to manage it as web.”
- Or: split verbs: `profile create-from-run <id> web` creates recipe only; `profile adopt <id> web` labels the current run.

### 4. `list` omits key human context

Current `sigmund list` columns are `RUNID STATE STARTED RESULT CMD`. It does not show alias/profile label, console availability, scope, or log size/path. The user must remember or inspect separate commands.

Suggested fix: default human list should include profile/alias and affordances, e.g.:

```text
NAME     ID        STATE    AGE  MODE     CMD
web      04a7dda8  running  12s  log      /usr/bin/python -m http.server 9000
api      fe21dfb8  running  2m   console  node server.js
-        a1b2c3d4  exited   1h   log      sleep 1
```

Keep current output available as `--plain`, `--columns`, or `--json` for scripts.

### 5. Cleanup/prune is conceptually separate from stop

For normal users, stop-then-prune is chore-like. The safe cleanup path should be discoverable and ideally one command.

Suggested fixes:

- Add `sigmund rm <target>`: stop if running, then prune after confirmation / `--force`.
- Add `sigmund stop --prune <target>`.
- In interactive mode, offer `stop`, `stop + prune`, and `prune exited`.

### 6. Advanced root/system profile features need a dedicated editor

`grant` and root-managed profiles are powerful but currently command-shaped. An advanced “profile editing mode” would let Sigmund grow without turning every setting into a long flag list.

Suggested fix: add `sigmund profile edit <name>` that opens `$EDITOR` or an interactive form with validation.



## Target model correction: profiles define, runs execute

A profile is not itself a running thing. A profile is a reusable definition for launching a tool. A run ID is the stable identity for one concrete execution and is the safest singular target for stop, kill, logs, console/open, and prune.

That means the command grammar should distinguish profile definition commands, ad hoc launch commands, and execution-control verbs:

```text
profile <name> ...          # create/edit/show/export/start a launch definition
run -- <cmd> [args...]      # launch one ad hoc command and print a run ID
stop|logs|open <target>     # act on concrete executions selected by run ID or safe profile-name shorthand
```

`run` should be a command, not a branch/namespace for existing executions. One-shot commands like `mund run web stop` read backwards and should be avoided. Use natural action verbs instead.

Profile-name targeting is a convenience resolver, not a different kind of process identity. It should work only when singular and safe:

- `mund logs web` is valid only if profile `web` has exactly one relevant run for the requested action.
- `mund stop web` is valid only if profile `web` has exactly one running run.
- If profile `web` has zero running runs, report that there is nothing to stop and suggest `mund profile web start`.
- If profile `web` has multiple running runs, refuse with candidates and require a run ID, `--all`, or an explicit selector.
- `mund profile web stop` should be avoided; the profile itself is not stopped.

Preferred wording:

```text
mund run -- npm run dev        # launch an ad hoc command, producing a run ID
mund profile web start         # launch the web definition, producing a run ID
mund stop 04a7dda8             # stop this exact execution
mund stop web                  # stop web only if exactly one web run is running
mund stop web --all            # stop all running executions launched from web
mund logs web --follow         # follow logs only if web resolves to one run
```

This preserves the safety property: concrete runs are controlled by run ID, while profile names are ergonomic selectors only when unambiguous.

## Unified stacked command grammar

If the goal is a drastic, appealing redesign, avoid having one grammar for scripts, another for captive shell, and another for profile config. Use one command language everywhere. The shell prompt, one-shot CLI, and Cisco-style config file should all accept the same command tree.

Examples:

```sh
# one-shot CLI
sigmund show runs
sigmund show profile web
sigmund profile web set command -- /usr/bin/python3 -m http.server 9000
sigmund profile web set cwd /srv/web
sigmund profile web set env PYTHONUNBUFFERED=1
sigmund profile web start
sigmund logs web --follow
sigmund stop web
sigmund clean exited --dry-run
```

The same operations in captive mode:

```text
sigmund> show runs
sigmund> profile web
sigmund(profile:web)> set command -- /usr/bin/python3 -m http.server 9000
sigmund(profile:web)> set cwd /srv/web
sigmund(profile:web)> set env PYTHONUNBUFFERED=1
sigmund(profile:web)> start
sigmund(profile:web)> logs --follow
sigmund(profile:web)> stop
```

The same operations as an importable config transcript:

```text
profile web
  set command -- /usr/bin/python3 -m http.server 9000
  set cwd /srv/web
  set env PYTHONUNBUFFERED=1
  set console off
  set multi deny
exit
```

Recommended grammar shape:

```text
sigmund show runs|profiles|profile <name>|run <target>|grants|config
sigmund profile <name> create|edit|delete|start|restart|status|export
sigmund profile <name> set command|cwd|env|console|multi|readiness|cleanup ...
sigmund profile <name> unset env|readiness|description|tag ...
sigmund status|inspect|logs|open|stop|kill|prune <id-or-singular-profile>
sigmund adopt <run-id> <profile>
sigmund grant <profile> <principal> <actions>
sigmund revoke <profile> <principal> <actions>
sigmund clean exited|stale|failed [--dry-run|--yes]
sigmund import <file> [--dry-run|--yes]
sigmund export profile <name> [--format cli|json]
```

This design lets users learn one mental model:

- In one-shot CLI, the first words are the context and action.
- In captive shell, selecting a context lets users omit the repeated prefix.
- In config files, indented commands are just the same context-local commands saved to disk.

Because there are no existing users to protect, 0.4.0 should cut over cleanly instead of preserving legacy verbs. Migration notes are still useful for maintainers and early testers, but old commands do not need to remain as supported UX.

Breaking 0.4.0 replacements:

```text
sigmund alias <id> <name>       => sigmund run <id> adopt <name> / sigmund profile <name> create-from-run <id>
sigmund aliases                 => sigmund show profiles
sigmund list                    => sigmund show runs
sigmund tail <target>           => sigmund run <target> logs --follow
sigmund dump <target>           => sigmund run <target> logs --dump
sigmund console <target>        => sigmund run <target> open
sigmund prune <target>          => sigmund run <target> prune
sigmund start <alias>           => sigmund profile <name> start
sigmund stop <target>           => sigmund run <target> stop
sigmund kill <target>           => sigmund run <target> kill
sigmund grant/revoke ...        => sigmund profile <name> grant/revoke ...
```

The cleaner product is one language, three surfaces. 0.4.0 should be the cutover point.


## 0.4.0 breaking redesign stance

No one is using Sigmund yet, so the product should spend this freedom now. Version 0.4.0 should be treated as the CLI redesign release that replaces the legacy command surface with the unified stacked grammar.

Principles for 0.4.0:

- Do not optimize for backwards compatibility with 0.3.x command names.
- Do optimize for one coherent grammar across CLI, captive shell, and config transcript.
- Keep the useful low-level behavior: daemonless starts, safe validation, logs, stores, profile JSON, root delegation.
- Change the command nouns and verbs aggressively where it makes the tool easier.
- Provide a migration table in the changelog/docs, but do not keep old verbs unless needed internally during implementation.
- Preserve scriptability by defining the new stdout/stderr and exit-code contract for the new grammar.

The 0.4.0 user promise should be:

```text
Use the same words everywhere:
  sigmund profile web start
  sigmund> profile web; start
  profile web; start; exit   # config transcript
```

## Proposed product model

Make the nouns explicit:

- **Run**: one concrete execution. Has ID, pid/pgid, state, log, timestamps, and optional profile label. Run ID is the stable singular control handle.
- **Profile**: reusable launch definition. Has command, args, cwd, env, mode, limits, hooks/readiness, policy, metadata. It can be started, edited, shown, exported, and granted, but it is not itself stopped; stop resolves to concrete runs.
- **Scope**: user or system.
- **Grant**: permission for another principal to act on a system profile.

Then layer the UX:

| Layer | Target user | Interface | Promise |
| --- | --- | --- | --- |
| Fast CLI | scripts and power users | `sigmund <cmd>`, `sigmund stop <id>` | stable, parseable, minimal |
| Friendly CLI | everyday users | `sigmund run`, `sigmund ps`, `sigmund rm`, `sigmund logs` | memorable aliases, good defaults |
| Interactive shell | humans operating jobs | `sigmund shell` / bare `sigmund` | discoverable dashboard and guided actions |
| Profile editor | advanced setup | `sigmund profile edit web` | validated durable configuration |

## Recommended command additions

### Friendly aliases for existing verbs

Keep current verbs, add familiar synonyms:

```text
sigmund ps                    -> list
sigmund logs <target>          -> tail or dump prompt/flag
sigmund log <target>           -> dump
sigmund rm <target>            -> safe stop/prune workflow
sigmund restart <profile>      -> stop profile then start profile
sigmund status [target]        -> richer inspection
sigmund inspect <target>       -> dump record/profile metadata
sigmund doctor                 -> explain stores, permissions, stale records
```

### Profile commands

```text
sigmund profile list
sigmund profile show <name>
sigmund profile create <name> -- <cmd> [args...]
sigmund profile from-run <id> <name> [--adopt]
sigmund profile edit <name>
sigmund profile rename <old> <new>
sigmund profile delete <name>
sigmund profile grant <name> <user> [actions]
sigmund profile revoke <name> <user> [actions]
```

Compatibility:

```text
sigmund alias <id> <name>      -> profile from-run <id> <name> --adopt? or legacy recipe-only mode
sigmund aliases                -> profile list
sigmund start <name>           -> profile start <name>
```

### Hybrid “do what I mean” commands

```text
sigmund up web                 # start if stopped; if running, print status + tail/console hint
sigmund down web               # stop all running runs for profile web
sigmund restart web
sigmund open web               # attach console if available, otherwise tail
sigmund clean                  # prune exited/stale with preview
```

These make Sigmund feel like a daily tool, not just a process record API.


## Navigable view namespaces and reversible redirects

The captive shell should not only execute commands; it should let users navigate the process universe from different useful viewpoints. These views are namespaces over the same underlying objects. Some paths are canonical, and some are convenience redirects, but navigation should preserve the path the user took so `back` feels natural.

Use better state words than only `active`/`inactive` where possible:

- **running**: currently validated as alive.
- **stopped**: intentionally stopped or exited cleanly.
- **failed**: exited nonzero or launch failed.
- **stale**: record exists but identity can no longer be validated.
- **dormant**: profile exists but has no current running execution.
- **all**: every visible profile/run.

Suggested top-level view namespaces:

```text
/runs                 concrete executions, grouped/filterable by state
/profiles             launch definitions
/running              running executions grouped by profile
/dormant              profiles with no running execution
/failed               failed runs grouped by profile
/stale                stale/unknown records
/system               system-scoped visible objects
/user                 user-scoped objects
/grants               delegation/access view
/logs                 recent log-centric view
/time                 runs grouped by start time / calendar bucket
/uptime               running runs grouped or sorted by current uptime
/recent               newest runs/events first
/oldest               oldest running runs first
```

The tree should be an indexed navigation system, not a single hierarchy. The same run can appear under state, profile, scope, start-time, uptime, failure, and log views. These are all views over one underlying run record.

Time-oriented examples:

```text
/time
  today
    10:14  web  04a7dda8  running
    09:58  api  fe21dfb8  failed
  yesterday
    16:20  worker a1b2c3d4 stopped

/uptime
  under-1m
    web  04a7dda8  12s
  1m-10m
    api  fe21dfb8  2m
  over-1h
    cache 9910aaef  3h

/recent
  04a7dda8  web     started 12s ago
  fe21dfb8  api     failed 2m ago
  a1b2c3d4  worker  stopped 1h ago
```

Useful view commands:

```text
show tree time
show tree uptime
show recent
show oldest
cd time/today
cd uptime/over-1h
cd recent
```

Example tree view:

```text
mund> tree running
/running
  web
    04a7dda8  12s  console off  python -m http.server 9000
  api
    fe21dfb8  2m   console on   node server.js
```

Profile-first view:

```text
mund> tree profiles
/profiles
  web
    definition
    running
      04a7dda8
    failed
      a1b2c3d4
  api
    definition
    dormant
```

Canonicalization rule:

- `/running/web` is the canonical path for currently running executions of profile `web`.
- `/profiles/web/running` is a convenience path that redirects to `/running/web`.
- The shell should remember that the user arrived through `/profiles/web/running`, so `back` returns to `/profiles/web`, not `/running`.

Example navigation:

```text
mund> cd profiles/web
mund(/profiles/web)> cd running
redirect: /profiles/web/running -> /running/web
mund(/running/web)> ls
04a7dda8  running  12s  python -m http.server 9000
mund(/running/web)> back
mund(/profiles/web)>
```

Direct navigation should backtrack canonically:

```text
mund> cd running/web
mund(/running/web)> back
mund(/running)>
```

This gives users multiple mental entry points without duplicating behavior. A profile-centric user can start from `profile web`; an operator can start from `running`; a debugger can start from `failed`; all routes resolve to the same concrete run objects.

Command implications:

```text
show tree running
show tree profiles
cd profiles/web
cd running/web
cd failed/web
back
pwd
ls
```

Inside a view, commands can be context-relative:

```text
mund(/running/web)> logs 04a7dda8
mund(/running/web)> stop 04a7dda8
mund(/profiles/web)> start
mund(/profiles/web)> export --format cli
mund(/failed/web)> logs a1b2c3d4 --dump
```

The important UX property: namespaces can connect backwards for discoverability, but concrete actions still resolve to stable run IDs or singular safe selectors before acting.

## Captive CLI / interactive mode proposal

### Entry points

```sh
sigmund              # if no args and TTY: open dashboard, not usage error
sigmund shell
sigmund menu
sigmund profile edit web
```

For non-TTY/no args, keep usage and exit nonzero for compatibility.

### Top-level dashboard sketch

```text
sigmund v0.4  daemonless process guardian

Runs
  NAME      ID        STATE    AGE   MODE     COMMAND
  web       04a7dda8  running  12s   log      python -m http.server 9000
  api       fe21dfb8  running  2m    console  node server.js
  scratch   a1b2c3d4  exited   1h    log      sleep 1

Profiles: web, api, cache(system)

Type: start <profile>, tail <name|id>, open <name|id>, stop <name|id>, profile, clean, help, quit
sigmund> 
```

### Cisco/diskpart-style submodes

```text
sigmund> profile web
sigmund(profile:web)> show
sigmund(profile:web)> set cwd /srv/web
sigmund(profile:web)> set env NODE_ENV=development
sigmund(profile:web)> set console on
sigmund(profile:web)> set readiness tcp localhost:3000 timeout 10s
sigmund(profile:web)> save
sigmund(profile:web)> start
```

```text
sigmund> run api
sigmund(run:fe21dfb8)> tail
sigmund(run:fe21dfb8)> stop
sigmund(run:fe21dfb8)> prune
```

### Captive menu variant

A menu/TUI would be even easier for first-time users:

- Arrow-key list of runs/profiles.
- Enter opens action palette.
- `l` logs, `c` console, `s` stop, `r` restart, `p` prune, `e` edit profile, `?` help.
- Prompts preview commands before privileged/destructive actions.

Implementation can start as a simple line REPL without curses, then graduate to an optional TUI later.

## Profile editor design

### Minimal v1 profile schema

Current profile hashing intentionally includes only resolved binary path and argv. For advanced features, do not silently change the existing hash contract. Add a versioned profile object with separately hashed launch identity and editable metadata.

Suggested conceptual shape:

```json
{
  "version": 2,
  "name": "web",
  "command": ["/usr/bin/python3", "-m", "http.server", "9000"],
  "cwd": "/srv/web",
  "env": {"PYTHONUNBUFFERED": "1"},
  "mode": {"console": false, "tail_on_start": false, "allow_multi": false},
  "readiness": {"type": "tcp", "host": "127.0.0.1", "port": 9000, "timeout_s": 10},
  "cleanup": {"stop_timeout_s": 5, "prune_on_success": false},
  "description": "local docs server",
  "tags": ["dev"],
  "access": {"alice": ["start", "stop", "tail", "dump"]}
}
```

### Editor UX options

1. **Guided form** for common fields:
   - command
   - working directory
   - environment variables
   - console/log mode
   - multi-run policy
   - readiness check
   - grants
2. **Text editor mode** for advanced users:
   - writes a temp JSON/TOML/YAML-like file
   - validates before save
   - shows diff from previous profile
3. **Command submode** for incremental changes:
   - `set command -- node server.js`
   - `set env PORT=3000`
   - `unset env DEBUG`
   - `grant alice start,stop,tail`

Start with JSON because Sigmund already has JSON infrastructure, but consider a more human format later if dependency policy allows it.


## Profile import/export and config files

The captive shell should not become a configuration island. Every profile that can be created or edited interactively should also be importable, exportable, diffable, and reviewable from the normal CLI. This is essential for backups, GitOps-style workflows, examples, sharing, CI setup, and scripted provisioning.

Recommended commands:

```text
sigmund profile export <name>              # print one profile config to stdout
sigmund profile export <name> -o web.json  # write one profile config
sigmund profile export --all -o profiles/  # write one file per profile
sigmund profile import web.json            # validate and create/update profile
sigmund profile import profiles/           # import a directory of profiles
sigmund profile validate web.json          # check without writing
sigmund profile diff <name> web.json       # compare stored profile to file
sigmund profile apply web.json             # validate, show change summary, then write
sigmund profile print <name>               # human-readable equivalent of export
```

For captive shell parity:

```text
sigmund> profile web
sigmund(profile:web)> export
sigmund(profile:web)> export web.json
sigmund(profile:web)> import web.json
sigmund(profile:web)> diff web.json
sigmund(profile:web)> validate
```

Design requirements:

- **JSON remains canonical on disk**: the existing profile store shape should stay the authoritative internal storage format.
- **Cisco-like config is a human interchange/editing format**: export can render a profile as the same commands a user would type in captive mode; import/apply parses those commands and writes validated JSON profiles.
- **Round-trip safe**: `export --format cli -> import/apply -> export --format cli` should preserve semantically meaningful fields, even if comments/spacing are not preserved.
- **Stdout friendly**: export defaults to stdout so users can pipe to files, review tools, or version control.
- **Dry-run by default for risky writes**: import/apply should validate and show a summary before overwriting unless `--yes` is supplied.
- **Secret aware**: environment values may need redaction/export modes, e.g. `--redact-secrets`, `--include-secrets`, or future secret references.
- **Versioned schema in JSON**: include `version` in the canonical JSON so profile config can evolve without breaking old files.
- **Scope explicit**: both JSON and CLI-config import should say whether the target is `user` or `system`, but system writes should still require explicit elevation.
- **Machine output still available**: `--format json` / `--json` should print the canonical JSON profile for automation; `--format cli` should print the Cisco-like command file for humans.

Example CLI-config export:

```text
profile web
  description local docs server
  command /usr/bin/python3 -m http.server 9000
  cwd /srv/web
  env PYTHONUNBUFFERED=1
  console off
  multi deny
  readiness tcp 127.0.0.1 9000 timeout 10s
  cleanup stop-timeout 5s
exit
```

Equivalent usage:

```sh
sigmund profile export web --format cli > web.sigmund
sigmund profile apply web.sigmund --dry-run
sigmund profile apply web.sigmund --yes
sigmund profile export web --format json > web.json
```

This makes the interactive shell a friendly editor for the same durable JSON artifact the CLI can manage, while giving humans a readable Cisco-style config transcript.


## Pager-style live filter viewer

For log, list, and tree views, `mund` should include a pager-style viewer with vi-like movement and immediate keystroke filtering. This is not just a command prompt filter. When the user is inside a page-up/page-down or vi-style viewer, typing should reveal a small filter field at the top and dynamically narrow the visible buffer on every keystroke.

Viewer behavior:

```text
┌ filter: error_                         12/834 lines ┐
├──────────────────────────────────────────────────────┤
│ 10:04:12 web ERROR missing config                    │
│ 10:05:01 web ERROR retry failed                      │
│ 10:05:07 api ERROR timeout                           │
└──────────────────────────────────────────────────────┘
```

Key model:

```text
plain typing     open/update live filter field
Esc              clear filter or leave filter mode
Enter            keep/pin current filter and return to navigation
Backspace        remove one character and redraw; repeated Backspace naturally clears the filter
Ctrl-u           optional acceleration to clear all filter text
/                explicit search mode, highlight/jump matches
f                explicit filter mode, hide non-matches
n/N              next/previous search match
j/k              line down/up
PgUp/PgDn        page up/down
g/G              top/bottom
q                quit viewer
```

Because filtering is dynamic, clearing should feel like editing text: Backspace back to an empty query restores the full view immediately. A dedicated clear key is optional, not required for the core UX.

Important distinction:

- **Search mode** highlights and jumps while keeping the full buffer.
- **Filter mode** hides non-matching rows and shows match counts.
- **Type-to-filter mode** is the fast path: in views where text entry is otherwise meaningless, normal printable keystrokes immediately build the filter.

This should work consistently for:

- `mund logs <target>`
- `mund show runs` when opened interactively
- `mund show profiles`
- `mund show tree running`
- `mund show tree time`
- `mund doctor` diagnostic output
- grants/access tables

For follow-mode logs, filtering should apply to the retained visible buffer and continue filtering new incoming lines. The UI should indicate when a filter is active so users do not think logs have stopped.

This viewer is one of the features that can make `mund` feel dramatically easier than raw shell pipelines: users can enter a view first, then discover the thing they need by simply typing.


## Example-line similarity filtering

The live filter viewer can go beyond text filtering by letting users mark representative lines and ask for visually/structurally similar rows. This should feel instant, not like a slow AI workflow.

Interaction model:

```text
Space            toggle current line as an example
S                show similar lines to selected examples
X                exclude similar lines to selected examples
A                add current line to positive examples
D                add current line to negative examples
U                unmark all examples
Enter            pin resulting similarity filter
Esc              leave similarity mode / return to normal viewer
```

Example:

```text
10:04:12 web INFO  listening on :9000
10:05:01 web ERROR missing config .env       [selected]
10:05:07 api ERROR missing config config.yml [selected]
10:06:44 api WARN  retrying database
```

User presses `S`; the viewer keeps lines that look like the selected failures:

```text
┌ similar: 2 examples                    8/834 lines ┐
│ 10:05:01 web ERROR missing config .env             │
│ 10:05:07 api ERROR missing config config.yml        │
│ 10:08:19 worker ERROR missing config worker.toml    │
└─────────────────────────────────────────────────────┘
```

Fast implementation should be lexical/structural first, semantic later if useful:

- tokenize log lines into words, numbers, paths, ids, timestamps, severity, profile, command, exit/status terms;
- downweight timestamps, PIDs, run IDs, UUIDs, hex IDs, ports, and monotonic counters;
- upweight severity, error class, command/profile, repeated message terms, path basenames, exit codes;
- score candidate lines with a cheap token-set similarity such as weighted Jaccard / cosine over hashed tokens;
- support positive and negative examples: “like these, not those”;
- update immediately on mark/toggle without network or model dependency.

This gives users a fast “find more like this” workflow for logs, failed-run lists, doctor diagnostics, and grants/access tables. It should be described as **similarity filtering** rather than AI search unless a later optional semantic backend exists.


### Viewport-driven similarity for huge logs

For very large logs, similarity filtering should not eagerly score every line. The viewer only needs enough matching rows to fill the terminal plus a small cache ahead/behind the viewport. Use an outward-expanding distance filter from the cursor/selected examples.

Algorithm shape:

1. User selects one or more example lines.
2. Build the example fingerprint.
3. Start at the current cursor/selection region.
4. Scan outward above and below by file distance.
5. Score candidate lines as they are encountered.
6. Add matching lines until the viewport is full, plus a small overscan buffer.
7. Stop scanning.
8. When the user pages up/down, continue scanning in that direction and fill the next viewport.

This keeps interaction fast even for million-line logs because the first response is proportional to terminal height and local match density, not file size.

Example behavior:

```text
selected line: 513,220
terminal height: 40
overscan: 80

scan 513,220 +/- outward until 120 matching rows are found
render nearest 40 matches
cache the next 80
```

Scrolling semantics:

- PageDown continues the outward/downward scan if the cache is low.
- PageUp continues the outward/upward scan if the cache is low.
- `G` / jump-to-end may scan from the end backward instead of from the original selection.
- `g` / jump-to-start may scan from the start forward.
- The UI can show partial-state text such as `similar: local matches` until a full-file scan is explicitly requested.

Optional explicit full scan:

```text
F                complete full-file similarity scan in background
```

But full scan should be opt-in. The default should feel immediate and local: find enough similar nearby lines to fill what the user can see.


Useful commands/labels:

```text
similar selected
more-like-this
exclude-like-this
filter similar
```

In non-interactive CLI, expose a deterministic version later if useful:

```sh
mund logs web --similar-line 120
mund logs web --similar 'missing config'
```

## Safety and automation compatibility

Protect the current CLI contract:

- Existing commands and stdout/stderr behavior should remain stable.
- Add `--json` for machine-readable rich output rather than changing default parseable assumptions.
- Interactive mode should only activate on TTY with no args or explicit `shell/menu`.
- Destructive interactive actions should show a preview and require confirmation unless the action is already non-destructive or scoped.
- Keep `--quiet`, exit codes, and current target resolution semantics for scripts.

## Phased roadmap

### Phase 1: Clarify and make current UX friendlier

- Add `profile` command aliases over existing alias/profile internals.
- Add alias/profile column to `list`.
- Add `status` / `inspect` command.
- Decide whether `alias <id> <name>` adopts the source run or warns clearly.
- Add `rm` or `stop --prune`.
- Improve start banner with profile/name hints.

### Phase 2: Hybrid interactive shell

- Add `sigmund shell` line-oriented REPL.
- Support `help`, `list`, `select`, `tail`, `dump`, `open`, `stop`, `prune`, `start`, `profile`.
- Implement command completion/history if practical.
- Add contextual help and suggested next actions.

### Phase 3: Profile editing mode

- Add versioned editable profile metadata.
- Add `profile show/edit/validate/from-run/delete/rename/import/export/diff`.
- Add readiness checks and cwd/env support if not already present.
- Keep legacy profile hash behavior isolated or explicitly versioned.

### Phase 4: TUI / captive menu polish

- Add full-screen dashboard if desired.
- Add action palette and keyboard shortcuts.
- Add safe privileged-flow previews.
- Add guided onboarding: “Start your first command”, “Save as profile”, “Run it again”.

## Highest-impact drastic change

The best drastic change for 0.4.0 is not to preserve old commands and add a menu beside them. It is to replace the legacy surface with a **one-language, three-surface product**:

- **One-shot CLI**: `sigmund profile web start`, `sigmund logs web --follow`, `sigmund stop web`.
- **Captive shell**: `profile web`, then `start`, `logs`, `stop`, `set env ...`.
- **Config transcript**: the same captive-shell commands saved in a file and imported/applied.

This gives beginners an embarrassingly easy path while giving power users a composable, scriptable grammar. JSON remains the canonical on-disk store; the command language becomes the human UX contract.

## Open product decisions

1. Should `alias <id> <name>` adopt/label the current run by default?
2. Should “profile” replace “alias” in docs/help as the primary noun?
3. Should bare `sigmund` open interactive mode on TTY, or should that require `sigmund shell` until the behavior is proven?
4. Should profiles support cwd/env/readiness in v1 of editing mode, or should v1 only expose command/args/console/multi/grants?
5. Should the interactive UI be line-oriented first or jump directly to a TUI?

My recommendation: line-oriented shell first, profile noun now, adopt-on-alias by default, and defer full TUI until the command model is validated.
