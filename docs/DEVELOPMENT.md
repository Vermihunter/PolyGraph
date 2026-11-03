# Developer quality gates

GitHub tests are **manual only**. Open Actions → CPU CI → Run workflow, choose a
branch and `full`, `unit`, `correctness`, `concurrency`, or `oracle`. `full` keeps
all existing build/test/oracle checks; concurrency remains a 50-repeat manual
stress check. Nothing runs automatically on push, pull request, or a schedule.
See [CI.md](CI.md) for dependencies, diagnostics, caching and reproduction.

Local pushes use the version-controlled [pre-push hook](../.githooks/pre-push).
Both terminal Git and CLion's Git push invoke it after installation. CLion must
use this Git checkout and have the same Python/CMake/LLVM tools available in its
process PATH; restart the IDE after changing PATH. This is a local safeguard,
not mandatory remote enforcement.

## Setup

Use Linux, Git, Python 3.10+, CMake 3.27+, Ninja, a C++23 compiler, and the CPU
libraries listed in [CI.md](CI.md). Install **clang-format 21 and clang-tidy 21**
from your distribution or the [official LLVM apt repository](https://apt.llvm.org/)
if your distribution supplies an older version. The script defaults to the
versioned executable names. `CLANG_FORMAT` and `CLANG_TIDY` may select alternative
executable paths, but both must report major version 21. Paths containing spaces
are supported (quote such paths on the command line). No CUDA SDK is needed for the CPU gate.

```bash
clang-format-21 --version
clang-tidy-21 --version
./tools/dev/install-hooks.sh
# Equivalent activation:
git config --local core.hooksPath .githooks
python3 tools/dev/quality.py hook-status
```

The installer refuses to overwrite another effective `core.hooksPath`; integrate
any existing hooks first. It changes only local Git configuration, not the
index, sources, commits or remotes. The hook and installer must retain their
executable file modes. Installation is per clone; Git does not activate hooks
merely by checking them out. Use `git config --show-origin --get core.hooksPath`
to diagnose another configuration overriding it.

## Individual developer commands

Run from the repository root (or invoke the driver by its absolute path from
another directory inside the same checkout):

```bash
# Read-only formatting audit of tracked, project-owned sources:
python3 tools/dev/quality.py format --all
# Narrow scope while editing; filenames are repository-relative:
python3 tools/dev/quality.py format --files src/cli/CommandLine.cpp
# Explicit, opt-in modification; never called by the hook:
python3 tools/dev/quality.py format-fix --files src/cli/CommandLine.cpp include/cli/CommandLine.hpp
# Or format every tracked project source deliberately:
python3 tools/dev/quality.py format-fix --all

# Configure CPU tests and export the actual compilation database:
python3 tools/dev/quality.py configure
# Audit all CPU translation units with the focused gate profile:
python3 tools/dev/quality.py tidy --all
# Incremental analysis: sources changed between a local commit/ref and HEAD:
python3 tools/dev/quality.py tidy --base origin/master
# Explicit source/header selection for uncommitted editing work:
python3 tools/dev/quality.py tidy --files include/cli/CommandLine.hpp

# Incremental build and fast tests:
python3 tools/dev/quality.py build
python3 tools/dev/quality.py test
# Entire sequence, stopping at the first failure:
python3 tools/dev/quality.py verify --all
# Or verify the committed range relative to a locally available base:
python3 tools/dev/quality.py verify --base origin/master

# Regression tests for the gate itself (no pushes or commits are made):
python3 -m unittest discover -s tools/dev -p 'test_*.py' -v
```

The manual commands inspect the working files, allowing checks during editing.
`--base` only selects paths from the **committed** base-to-HEAD diff; use
`--files` or `--all` to include uncommitted edits. The pre-push entry point does
not expose these manual scope overrides. It always derives scope from Git's
actual pre-push stdin and verifies a clean checked-out revision.

## Push behavior and revision safety

The hook reads every advertised update (`local ref`, `local object ID`, `remote
ref`, `remote object ID`) before starting tools. For every non-deletion update,
its peeled commit must equal checked-out `HEAD`. A push of another branch's tip,
a tag pointing elsewhere, a non-commit object, or multiple distinct tips is
rejected: check out and push each revision separately. Annotated tags pointing
to HEAD are supported. Deletion-only/no-update pushes introduce no source and
need no build.

Tracked working files and the index must be clean; non-ignored untracked files
must be committed or stashed too. Ignored build trees and datasets are allowed.
This prevents testing unrelated uncommitted changes instead of the pushed
revision. HEAD and cleanliness are rechecked between stages and at the end; if
an editor or another Git operation changes them during verification, the push
fails. Avoid editing files while a push gate runs. This is not an adversarial
filesystem lock; ignored dependencies/toolchain configuration are trusted local
inputs, not part of a hermetic build.

For known remote objects, the union of remote-tip-to-local-tip changed paths is
checked. No fetch/network mutation is performed. A new branch or unavailable
remote object causes a conservative full tracked-source audit. Changes to build
configuration, Clang configuration, or gate code also force full source/CPU
analysis. The hook checks the final pushed tip, not every historical commit in
the outgoing range. A successful run is not cached across pushes.

The order is:

1. **Format validation**, with the root `.clang-format`, LLVM 21 `--dry-run
   --Werror`, and no source writes. Failures list all affected files and an
   explicit repair command. Sources/headers under `include`, `src`, `app` and
   `tests` are included; vendored `include/utilities/json.hpp`, generated
   protobuf files, generated/vendor/external/build/data directories are excluded.
   The selected formatter must preserve a CUDA kernel-launch syntax probe before
   `.cu`/`.cuh` formatting is allowed. CUDA files are not passed to a C++ compiler.
2. **CMake configure**, reusing `cpu-tests` and `build/cpu-tests`. It explicitly
   enables `CMAKE_EXPORT_COMPILE_COMMANDS`, disables CUDA and the CMake automatic
   formatter/tidy integrations, and clears global `CMAKE_CXX_CLANG_TIDY` so a
   push cannot trigger the repository's build-time source formatter. Existing
   dependency locations in the build cache are retained; discovery errors fail.
3. **Clang-tidy**, using that actual `compile_commands.json`. The small
   [gate profile](../tools/dev/clang-tidy.yaml) leaves the broad root CLion
   `.clang-tidy` unchanged. It enables focused analyzer/use-after-move/memory/RAII
   checks and treats enabled diagnostics as errors. It never uses `--fix`.
   External/generated translation units and header diagnostics are excluded.
   Changed headers select consumers using valid Ninja dependency records. An
   incomplete graph, fresh build, or header with no known consumer falls back
   to all CPU translation units. A changed CPU source absent from the compilation
   database fails; it is not silently ignored.
4. **Incremental build**, via `cmake --build --preset cpu-tests`.
5. **Fast tests**, the union of `unit` and deterministic `correctness` labels,
   executed once. Both required selections are independently checked for
   nonemptiness. Tests labeled integration/database/external/ldbc/slow/stress are
   excluded; failure, missing executables or an empty selection blocks the push.
   Deterministic concurrency tests run once; the 50-repeat stress run is manual.

Every tool failure exits nonzero. Subprocesses cannot consume the push protocol
from stdin and have a 15-minute upper bound; version/probe commands have a
30-second bound. CTest's default per-test bound is 60 seconds, with existing
20/30/60-second test properties retained. Compiler errors and the known CLI
alias test are never suppressed.

CPU static analysis does not cover optional HTTP/server, GPU, algorithm-analysis,
or metrics translation
units that are absent from this preset. Changed optional units/CUDA files print
that limitation explicitly. A changed header without a CPU consumer causes the
conservative CPU scan, but it cannot establish GPU/HTTP correctness. Use a
suitable compilation database and toolchain for those optional targets. Formatting
all owned files is independent of whether a CPU target builds them.

## Bypass and later remote enforcement

`git push --no-verify` bypasses this local hook; users can also change local Git
configuration or hook files. Use that escape hatch deliberately and record why.
CLion can likewise be configured to skip hooks. No local hook is tamper-proof.
To require checks centrally later, add automatic CI events deliberately and
configure branch protection/rulesets requiring those checks, or add a managed
server-side gate. Manual-only workflows alone do not enforce every push/PR.

## Verification and existing blockers

Local verification on 2026-11-03 (Ubuntu 22.04, LLVM 21; not a hosted run):

| Command/check | Result |
| --- | --- |
| `python3 tools/dev/quality.py format --all` | Exit 1: five existing app-file formatting failures; CUDA probe passed; sources unchanged |
| `python3 tools/dev/quality.py verify --all` | Exit 1 at format validation; no later stage was run |
| `python3 tools/dev/quality.py configure` | Passed; populated the actual CPU compilation database |
| `python3 tools/dev/quality.py tidy --all` | Passed all 94 CPU translation units with the focused profile |
| `python3 tools/dev/quality.py build` | Passed every enabled CPU target |
| `python3 tools/dev/quality.py test` | Exit 1, 468/469 passed; only the existing CLI alias test failed outside the socket-restricted sandbox |
| `ctest --preset cpu-tests -L '^correctness$' --no-tests=error --parallel 2 --timeout 60` | 323/323 passed |
| `python3 -I -S .github/scripts/run_oracle_tests.py` | 88/88 passed |
| Same oracle runner with `--isolation-only` | 2/2 passed |
| `python3 -m unittest discover -s tools/dev -p 'test_*.py' -v` | 19/19 passed; includes per-stage shell-hook failure injections without pushing |
| Actual LLVM 21 null-dereference fixture with the gate profile | Exit 1 with `clang-analyzer-core.NullDereference` promoted to error |
| Actual `.githooks/pre-push` with HEAD/new-branch stdin | Exit 1 on the dirty implementation working tree, before any check |
| Actual hook with deletion-only stdin | Exit 0, no source checks/build |
| `./tools/dev/install-hooks.sh` and `python3 tools/dev/quality.py hook-status` | Passed; this clone now has `core.hooksPath=.githooks` |
| `actionlint -shellcheck=/path/to/shellcheck .github/workflows/cpu-ci.yml` | Passed (actionlint 1.7.7, ShellCheck 0.10.0) |
| `shellcheck .githooks/pre-push tools/dev/install-hooks.sh` | Passed |

The first sandboxed fast-test attempt also failed the existing loopback Redis
fake at socket creation. Running with loopback sockets available isolated the
sole genuine test failure to the CLI regression. No real database was used.
Logs are retained under ignored `build/quality-verification/`. No GitHub manual
run was dispatched. No existing C++/CUDA source was changed, and no files were
staged, committed or pushed.
No source is auto-formatted or automatically repaired to make a push green.
The current full-format audit fails on five existing files:
`app/bfs/main.cpp`, `app/bfs_benchmark/BfsBenchmark.cpp`,
`app/bfs_benchmark/BfsBenchmark.hpp`, `app/bfs_benchmark/main.cpp`, and
`app/http_server/main.cpp`. New-branch/full audits will block until those are
explicitly formatted by a developer. Incremental pushes only audit their scope.

The current checkout also still contains the known
`CommandLineTest.RejectsUnknownAndDuplicateOptions` failure: duplicate aliases
are accepted. A full or fast test run therefore fails and prevents a push.
This gate implementation does not modify that production behavior or skip its
test. The prior CI implementation report in `CI.md` referred to a local alias
fix that was not included in the current committed source.
