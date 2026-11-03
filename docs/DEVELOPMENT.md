# Developer quality gates

Normal `git push` uses the **fast** CPU profile:

1. Check formatting of relevant changed project C++/CUDA files, without modifying them.
2. Reuse/configure the existing CPU build if necessary and run one incremental Ninja build.
3. Run the ten required smoke tests.

**Clang-tidy does not run in the fast profile**, regardless of changed headers,
CMake files, developer scripts, missing dependency records, or tidy cache state.
Fast verification does not require clang-tidy to be installed. The fast
`verified-push` wrapper also excludes tidy from its certificate tool fingerprint;
it does not invoke even `clang-tidy --version`.

Static analysis and comprehensive tests are opt-in developer commands. Explicitly
run the **full** profile and dispatch **CPU CI → full** before merging. Hooks do
not dispatch workflows or configure repository branch protection.

## Setup and daily commands

Ubuntu/Linux requires Python 3.10+, Git, CMake 3.27+, Ninja, a C++23 compiler,
and LLVM 21 clang-format. Manual static analysis also requires LLVM 21
clang-tidy. `CLANG_FORMAT` and `CLANG_TIDY` can name alternate LLVM 21 executables.
See [CI.md](CI.md) for CPU dependency installation.

```bash
./tools/dev/install-hooks.sh
python3 tools/dev/quality.py hook-status

# Automatic checks: changed formatting + incremental build + smoke tests.
git push

# The same fast pipeline while editing (no network or push).
python3 tools/dev/quality.py fast
python3 tools/dev/quality.py fast --base origin/master
python3 tools/dev/quality.py fast --files tests/command_line_test.cpp
python3 tools/dev/quality.py fast --all
```

Bare `fast` includes changes since the upstream, working/index edits and
untracked files. With no known upstream it visibly falls back to all tracked
files. `--base` compares committed revisions; use `--files` or bare `fast` to
include edits. `--all` audits formatting of every tracked owned source and runs
build/smoke checks; it still performs no tidy analysis.

Every stage and the total print elapsed time. Failed checks preserve a nonzero
exit status and stop subsequent stages. Missing, disabled or runtime-skipped
smoke tests fail; CTest JUnit results are checked in addition to discovery.

## Manual static analysis and full verification

```bash
# Analyze changed translation units and consumers of changed project headers.
python3 tools/dev/quality.py tidy --base origin/master
python3 tools/dev/quality.py tidy --files tests/command_line_test.cpp include/cli/CommandLine.hpp

# Fresh analysis of every project CPU translation unit (no success-cache reuse).
python3 tools/dev/quality.py tidy --all

# Full formatting audit, fresh full CPU tidy, all CPU build targets, every test,
# then 50 until-fail repetitions of the concurrency selection.
python3 tools/dev/quality.py full

# Complete tests and concurrency repetitions, without static analysis.
python3 tools/dev/quality.py test --profile full

# Separate ASan+UBSan and TSan build/test profiles, where supported.
python3 tools/dev/quality.py sanitizers
```

`verify --all` remains an alias for full verification. Explicit `--profile full`
(or the existing `POLYGRAPH_QUALITY_PROFILE=full` override) deliberately requests
full verification; leave that environment override unset for ordinary fast
pushes. `full` rejects `--files`/`--base` so full coverage cannot silently become
a targeted check. `tidy --base` uses committed differences; `--files` can inspect
current edits. Omitting a scope from `tidy` analyzes all tracked CPU sources.

The existing `tools/dev/clang-tidy.yaml` focused, warnings-as-errors policy and
`.polygraph/tidy-cache.json` infrastructure are preserved. The broader root
`.clang-tidy` remains the IDE policy. Full tidy means every CPU TU under the
focused policy, not every optional feature configuration.

Manual tidy builds once before analysis to refresh generated headers and Ninja
transitive dependencies. Changed headers select recorded consumers. Missing or
incomplete dependency information and relevant policy/configuration changes
visibly select all CPU units. Changed-file analysis may reuse completed successes
whose source, transitive dependencies, compilation commands, configuration and
toolchain are unchanged. Full analysis ignores success-cache hits. These rules
apply only when analysis is explicitly requested; none can expand a fast push
into a tidy run.

The CPU preset excludes CUDA, HTTP and some optional metrics/analyzer sources.
Manual tidy reports known optional sources outside its compilation database;
unexpected missing CPU sources fail. Fast checks and full CPU verification do
not provide GPU/HTTP build/runtime coverage. Use the relevant feature presets.

## Pre-commit and incremental build safety

Automatic pre-commit clang-format is unchanged. It formats only eligible staged
files under `include/`, `src/`, `app/` and `tests/`, then stages their exact
formatted blobs. Generated/protobuf code, vendored dependencies, build/data
files and `include/utilities/json.hpp` are excluded. `.clang-format-ignore` is
respected. No repository-wide formatting runs during normal commits.

Partially staged eligible files and unstaged formatter-policy changes are
rejected **before writes**, preserving unstaged edits. Stage the whole file or
stash unstaged edits with `git stash push --keep-index`, then restore and resolve
any conflicts. Existing index locking, mode/symlink checks, alternate-index
support and editor-save checks remain intact. Push formatting is read-only.

The hook consumes every Git pre-push input row and groups identical tips. It uses
the actual remote OID, every introduced commit's changed paths and the final
tree diff, including reverted changes and force-update removals. NUL-delimited
paths preserve spaces/newlines; renames include both names, and deleted files
have nothing to format. Ref deletions need no source checks. Unknown remote
objects and new refs visibly select all tracked files for formatting, **never
for automatic tidy**. Local tracking refs are not trusted as authoritative new-ref
bases. Distinct tips use persistent isolated checkouts/builds under
`.polygraph/revisions/<commit>/`. First builds can be expensive.

Pre-push requires the checked-out revision to be clean, including untracked
files; dirty trees fail with commit/stash guidance. Ignored build/data files are
allowed. The hook never resets the developer's checkout or bypasses Git hooks.

`build/cpu-tests` is retained. Ninja rebuilds stale targets and regenerates changed
CMake inputs. Explicit configuration runs only for missing/incompatible state or
changed presets. Existing fetched dependencies are retained and updates are
disconnected; a cold build can still download missing dependencies. The CPU
preset disables project build-time tidy and formatting. The configuration guard
also clears/rejects nonempty `CMAKE_C_CLANG_TIDY` and `CMAKE_CXX_CLANG_TIDY`, so
neither language can implicitly launch tidy during the fast build. Other manually
selected CMake presets can enable build-time tooling; the hook uses only
`cpu-tests`.

## Verify before opening SSH

For a long initial build, the existing wrapper runs verification before starting
Git's remote connection:

```bash
# These commands perform a push when invoked.
python3 tools/dev/quality.py verified-push origin
python3 tools/dev/quality.py verified-push origin HEAD:refs/heads/my-branch

# Explicit opt-in to the full preflight, including tidy and all tests.
python3 tools/dev/quality.py verified-push --profile full origin HEAD:refs/heads/my-branch
```

The default preflight runs the same formatting/build/smoke pipeline without tidy.
Temporary certificates bind the exact commit/tree, covered file scope and profile
to configuration, toolchain, dependencies, generated fixtures and built outputs.
State is checked before/after verification and by the enabled hook against the
actual server-advertised scope. Changed state or uncovered remote changes cause
a quick failure, not a rebuild while SSH is connected. Refresh tracking refs with
`git fetch --prune <remote>` and rerun preflight. No `--no-verify` or force flags
are used. Certificates are removed after invocation.

## CI and verification evidence

Existing GitHub Actions remain **workflow_dispatch only**. Choose `tidy` for
fresh comprehensive CPU analysis or `full` for formatting, comprehensive CPU tidy,
complete tests, correctness, repeated concurrency and the isolated Python oracle.
No automatic push or pull-request triggers were added. The workflow file and
analysis configuration are unchanged by this correction.

```bash
python3 -m unittest discover -s tools/dev -p 'test_*.py' -v
python3 -m unittest discover -s tools/dev -p check_pre_commit_test.py -v
```

The correction adds regression guards for no tidy invocation in cold-cache/new-ref
hooks, fast source/header/configuration scopes, fast certificate tool lookup,
C/C++ build-time tidy settings, required-stage failure propagation, and explicit
manual tidy/full behavior. Existing range/cache/formatting safety tests remain.
Tests do not perform remote pushes or commit to the developer checkout.

The reported baseline push took **425s**, including **424.60s** for 92 tidy
analyses out of 94 selected TUs. Cache reuse could not reliably meet the push
budget, so analysis was removed from the automatic profile rather than further
optimizing it. Current measurements and process-execution evidence are retained
under ignored `build/quality-no-tidy/`. They measure the real fast pipeline via
`fast --files` on six existing owned files plus `CMakeLists.txt`, with a configured,
unchanged CPU build; the shell hook itself is exercised in disposable fixtures.

| Stage | Latest six-file run |
| --- | ---: |
| clang-format | 0.38s |
| CMake configure | 0.00s |
| CPU build | 0.03s |
| smoke tests | 0.34s |
| total | 0.75s |

Three untraced six-file runs took 0.57s, 0.62s, 0.75s (median 0.62s). All ten smoke tests passed. The unchanged build performed no compilation. A traced run took 1.38s; its `execve` log contains **zero clang-tidy executions**, and generated Ninja commands contain no tidy step. All runs set `CLANG_TIDY` to a nonexistent executable. Fast preflight/certificate validation also succeeded with that setting.

All **48 gate regressions** and **18 staged-formatting regressions** passed. Executable hook fixtures verify required formatting/configuration/build/test failures return nonzero and stop later stages, and that new refs/cold caches do not invoke tidy. Pre-commit formatting, manual tidy/full routing, and manual-only CI are preserved. No remote push, commit or workflow dispatch was performed.
