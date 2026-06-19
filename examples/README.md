# Sigmund examples

These scripts are runnable examples for interactive learning, CI, and automation patterns.

## `interactive-demo/run-demo.sh`

A narrated sandbox demo that pulls in `sigmund` when needed, starts a helper process, captures logs, creates an alias, restarts from that alias, and cleans up. It uses temporary state and does not install Sigmund, edit shell profiles, or write to `/etc`.

The script is written to be readable: [interactive-demo/run-demo.sh](interactive-demo/run-demo.sh).

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/demo.sh | bash
```

## `uv-webserver-alias.sh`

Installs or locates `uv` and `sigmund`, starts a Python HTTP server through Sigmund, creates a durable alias from the recorded run, stops the first run, then starts the alias from a different directory to prove the alias recipe is independent of the script's current working directory.

```sh
examples/uv-webserver-alias.sh
```

Useful knobs:

```sh
SIGMUND_DEMO_PORT=8765 \
SIGMUND_DEMO_ALIAS=uv-web-demo \
SIGMUND_DEMO_ROOT="$PWD/.sigmund-demo" \
examples/uv-webserver-alias.sh
```

The script writes installer handoff state under `SIGMUND_DEMO_ROOT` and uses absolute `SIGMUND_BIN` and `UV_BIN` paths internally. It does not rely on a shell profile being reloaded.
