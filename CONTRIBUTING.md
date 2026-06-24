# Contributing to hold

hold is a small, security-sensitive C11 tool. The bar is: a change can be
extended later **without** a weak test letting a regression through. To that end
there is **one command that runs the same rigor everywhere**, and the security
invariants are enforced by tests that fail on regression.

## One command, every platform

| You are on… | Run | What it does |
|---|---|---|
| **macOS or Linux (native)** | `make ci` | static + dynamic `-Werror` builds, the regression suite + profile-hash vector, **ASan/UBSan**, cppcheck (if installed), and the layer-dependency lint. Anything your local toolchain lacks is reported **SKIPPED**, never silently passed. |
| **A Mac, but you want Linux parity** | `scripts/linux.sh` | runs `make ci`'s full rigor — **including cppcheck** and the Linux code paths — inside the committed CI image (`docker/Dockerfile.linux-ci`), via Apple `container`, Docker, or Podman. Your working tree is copied in, never mutated. |
| **The root / sudoers / private-store lane** | `scripts/linux.sh root` | runs the suite as root with `HOLD_REQUIRE_ROOT_TESTS=1`, exercising the privilege-delegation tests the user lane can only skip. |
| **Build the actual release artifacts** | `scripts/linux.sh release` | builds + packages the release tarballs the same way `release.yml` does — Linux gnu (native) + the full musl cross matrix (amd64/arm64/armhf/mipsel/riscv64) in the pinned container — into `dist/`, so you can download nothing and still test real artifacts. Run `scripts/release_build.sh` directly (no container) to add the **native macOS** tarball to the same `dist/`. |

`make ci` ≡ `scripts/ci.sh`; `make lint` ≡ `scripts/lint_layers.sh`;
`scripts/release_build.sh` mirrors `release.yml`'s build commands and packages
with the same `package_tarball.sh` CI uses. GitHub CI calls the same scripts, so
"green locally" means the same thing as "green in CI".

> Apple `container`: `container run` executes inside the default `container
> machine` (a persistent Linux VM), so nothing extra is needed. To give builds
> more of a big host, tune the VM once — e.g. `container machine set cpus=12
> memory=32G` — then restart it; this is host config, intentionally not baked
> into the repo. The pinned image (incl. zig) is what makes the build itself
> reproducible.

> macOS note: `make ci`'s ASan/UBSan step is the only place the `__APPLE__` code
> branches get sanitized, and cppcheck/the Linux branches only run under
> `scripts/linux.sh`. Run both before a release.

## Test conventions

- Tests live in `tests/test_hold.sh` (black-box: they run the built binary and
  assert on stdout/stderr/exit-codes/filesystem). `tests/profile_hash_vector.c`
  locks the profile-hash capability key.
- There is a real **SKIP** state. A test that needs something absent (a root
  actor, `setsid`) calls `skip "<reason>"`; it is reported as SKIP, **not** a
  vacuous PASS. Set `HOLD_REQUIRE_ROOT_TESTS=1` (CI's root lane does) to turn
  any skip into a hard failure.
- Assert the **specific** observable — exact exit code, exact file mode/owner,
  exact output — not just "exit 0". Add a negative test for every refusal path.
- Security-sensitive changes (store layout/modes, sudoers, the signal path, the
  profile hash, console sockets) must come with a test that fails if the
  invariant regresses. See the existing `test_system_store_*`,
  `test_grant_*`, `test_*_sudo_provenance`, `test_signal_*` for the patterns.

## Architecture

Source is layered under `src/` with public APIs in `include/hold/`:
`core` → `platform` → `store` → `console`/`access` → `runtime` → `cli`/`main`.
A lower layer must never include a higher layer's header; `make lint` enforces
this. All cross-module symbols are `hold_`-prefixed.

## Known cleanups (not blockers)

- `release.yml` builds (gnu/musl) do not pass `-Werror`, while every gate does —
  worth aligning so release artifacts are held to the same warning policy.
- `do_signal_action` has a `pgid <= 1` guard that is unreachable because
  `hold_valid_record` already rejects `pgid <= 1` at load; it can be dropped
  or the redundancy documented.
- `do_print_signal_command` evaluates state before the boot-id check, the
  opposite order from `do_signal_action`; mirror them for consistency.
- The pinned Zig version lives in two places — `ZIG_VERSION` in
  `docker/Dockerfile.linux-ci` and in `.github/workflows/release.yml`; they must
  be bumped together. Worth centralising into a single pinned value.
