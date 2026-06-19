# Parameterized profiles proposal

[Future work](README.md) | [Docs index](../index.md) | [Profiles and aliases](../profiles-and-aliases.md) | [Security](../security.md)

Status: future-work proposal, not current behavior.

This proposal extends root-managed aliases with an admin-defined public CLI for a hidden root command.

The public CLI is a Sigmund wrapper, not the target command's CLI:

```text
sigmund start report --date <yyyy-mm-dd> --mode <summary|full>
sigmund start scale --count <count>
```

The profile hash identifies the hidden profile. The hash lets sudoers and Sigmund refer to the delegated interface without exposing the command behind it.

## What Is Public

The user-facing surface is small:

```text
alias
profile hash
public help
public argv shape
allowed input shapes
```

Example:

```text
alias:
  report

public help:
  sigmund start report --date <yyyy-mm-dd> --mode <summary|full>

public argv:
  --date <date>
  --mode <mode>

allowed inputs:
  date matches ^[0-9]{4}-[0-9]{2}-[0-9]{2}$
  mode is summary or full
```

The user can see that a date and mode are accepted. The user cannot see what command uses them or where they go.

## What Stays Root-Private

The root-private profile contains the real command shape:

```text
target binary
target argv template
mapping from public inputs to target argv positions
```

Example:

```text
target binary:
  /usr/local/sbin/private-report

target argv template:
  private-report --internal-date <date> --format <mode>

mapping:
  <date> receives the validated --date value
  <mode> receives the validated --mode value
```

Those mappings are not exposed through the public alias, public help, public run index, or sudoers rule.

## Chain Of Operations

The chain is:

```text
1. User runs the public wrapper:
     sigmund start report --date 2026-06-19 --mode summary

2. User-mode Sigmund resolves the public alias:
     report -> <profile-hash>

3. User-mode Sigmund builds the Sigmund argv that will cross sudo:
     sigmund --system --elevated ... report <profile-hash> --date 2026-06-19 --mode summary

4. sudoers checks that argv against the managed regex for this alias/profile.

5. If sudoers allows it, sudo starts root Sigmund.

6. Root Sigmund loads the root-private profile by hash.

7. Root Sigmund validates the admitted values.

8. Root Sigmund maps the validated values into the hidden target argv.

9. Root Sigmund starts the target command.
```

Sudoers never starts the target command. Sudoers only decides whether this constrained Sigmund invocation may cross into root.

Root Sigmund decides what the values mean and what command actually runs.

## Example Boundary

Sudoers-visible shape, conceptually:

```text
sigmund --system --elevated ... report <profile-hash> --date <date-regex> --mode <mode-enum>
```

Root-only final argv:

```text
/usr/local/sbin/private-report private-report --internal-date 2026-06-19 --format summary
```

The public `--date` and `--mode` switches are Sigmund wrapper switches. They are not target switches unless the root-private profile maps them that way.

## Security Plus Opacity

This is not security through obfuscation.

The security is:

```text
sudoers admits only the constrained Sigmund argv shape
root Sigmund loads only the profile named by the hash
root Sigmund validates admitted values before use
root Sigmund builds argv directly, without a shell
```

The opacity is:

```text
the profile hash hides the command shape
the public surface does not reveal the target binary
the public surface does not reveal internal flags
the public surface does not reveal where inputs are inserted
the public surface does not reveal why inputs are used
```

The regex and root-side validation enforce the allowed inputs. The hash and root-private profile keep the real command shape hidden.

## Design Rules

- The admin defines the public Sigmund wrapper argv.
- Named public switches such as `--date`, `--count`, and `--mode` are part of that wrapper argv.
- The wrapper argv is admitted through sudoers only if it matches the managed regex.
- The hash selects the immutable root-private profile.
- Root Sigmund validates the admitted values again.
- Root Sigmund maps those values into the hidden argv.
- The target command, target argv, mappings, paths, and meaning stay root-private.

This note stops at that shape.

