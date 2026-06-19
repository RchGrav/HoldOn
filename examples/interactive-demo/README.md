# Sigmund interactive demo

Run the demo without installing anything:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/demo.sh | bash
```

The demo is contained in one small shell script. Take a look at [run-demo.sh](run-demo.sh) if you want to see how it works and what to expect; it is commented like a walkthrough.

The script is a narrated tutorial. It pulls in `sigmund` when needed, prints each command before it runs it, pauses between steps when stdin is a terminal, and explains how the same pattern applies to CI helpers, local dev servers, databases, emulators, and integration-test services.

## What happens

1. It creates one temporary directory under `${TMPDIR:-/tmp}`.
2. It creates a temporary `HOME`, Sigmund state directory, boot-id file, helper script, and sandboxed `sigmund` binary inside that directory.
3. It uses `sigmund` from `PATH` if available. If not, it downloads the matching release tarball and `SHA256SUMS`, verifies the checksum, and extracts only into the temporary directory.
4. It starts the helper process, shows the run ID and log, lists runs, dumps log output, prints the stop command, stops and prunes the run, creates an alias, starts from the alias, then stops and prunes again.
5. It refuses to create sudoers entries because the demo binary is deliberately sandboxed, not a secured root-owned install.
6. On exit or interruption, it tries to stop/prune demo runs and removes the temporary directory.

## Isolation

The demo creates a temporary directory containing:

- a temporary `HOME`;
- temporary Sigmund state;
- a generated helper application;
- a copied, downloaded, or freshly built `sigmund` binary;
- a fake boot-id file for deterministic demo state.

It does not install Sigmund, change shell profiles, or write to `/etc`. The cleanup trap stops and prunes demo runs, then removes the temporary directory.

Because the demo uses a sandboxed binary in a temporary directory, that binary is not a secured installed file. The demo therefore refuses to create a sudoers entry and explains why. If a secured root-owned Sigmund binary is installed, Sigmund's managed sudoers behavior can be demonstrated separately in a deliberately privileged test environment.

## Local development

From a source checkout, `examples/interactive-demo/run-demo.sh` runs the same demo. The script works on Linux and macOS with Bash, POSIX shell utilities, and either an existing `sigmund` binary, a downloadable release artifact, or a C compiler plus a source checkout.
