# Developer quality gates

Normal pushes use the **fast** CPU profile: changed-file formatting checks,
content-cached clang-tidy, one incremental Ninja build, and ten required smoke
tests. It is a development feedback gate, not complete regression coverage.
Run the **full** profile and explicitly dispatch **CPU CI → full** for every
change before merging. Local hooks do not dispatch workflows or enforce GitHub
branch protection; configure required CI checks in repository settings.

## Setup and commands

Ubuntu/Linux needs Python 3.10+, Git, CMake 3.27+, Ninja, a C++23 compiler, and
LLVM 21 `clang-format`/`clang-tidy`. See [CI.md](CI.md) for CPU dependencies.
`CLANG_FORMAT` and `CLANG_TIDY` may name LLVM 21 executables. The existing root
`.clang-format` is retained. `tools/dev/clang-tidy.yaml` is the existing focused,
warnings-as-errors analyzer policy; the broader root `.clang-tidy` remains the
IDE policy. “Full tidy” means every CPU translation unit under the focused policy.

```bash
./tools/dev/install-hooks.sh
python3 tools/dev/quality.py hook-status

# Default git pre-push profile; Git provides the actual outgoing refs on stdin.
git push

# While editing: upstream/HEAD changes, index/worktree edits and untracked files.
python3 tools/dev/quality.py fast
python3 tools/dev/quality.py fast --base origin/master
python3 tools/dev/quality.py fast --files tests/command_line_test.cpp
python3 tools/dev/quality.py fast --files include/cli/CommandLine.hpp

# Explicitly inspect every tracked file, still allowing valid tidy cache hits.
python3 tools/dev/quality.py fast --all

# Full format audit, fresh full CPU tidy, incremental complete CPU build,
# every registered test, then 50 until-fail repetitions of concurrency tests.
python3 tools/dev/quality.py full
POLYGRAPH_QUALITY_PROFILE=full git push

# Separate ASan+UBSan and TSan configurations/builds, then complete CTest suites.
python3 tools/dev/quality.py sanitizers

# Individual commands; verify --all is a backwards-compatible full profile.
python3 tools/dev/quality.py configure
python3 tools/dev/quality.py build
python3 tools/dev/quality.py test             # smoke tests
python3 tools/dev/quality.py tidy --all       # full tidy, never accepts cache hits
python3 tools/dev/quality.py tidy --files tests/command_line_test.cpp
python3 tools/dev/quality.py format --all
python3 tools/dev/quality.py format-fix --files tests/command_line_test.cpp
python3 tools/dev/quality.py verify --all
```

`full` rejects `--files`/`--base`; a requested full profile cannot silently become
a targeted check. `fast --base` compares committed revisions; use `--files` or
bare `fast` to include edits. Without a known upstream, bare `fast` reports its
all-files fallback. Normal `pre-push` requires a clean index/worktree (including
untracked files), so it verifies the pushed revision rather than different local
edits. Ignored build/data files are permitted. A dirty checkout fails before
checks with commit/stash guidance. Every stage prints its duration, including
failures, and the driver prints total elapsed time.

## Pre-commit formatting

The existing transactional staged formatter is reused. It formats only staged,
project-owned C++/CUDA files in `include/`, `src/`, `app/`, and `tests/`, then stages
exact formatted blobs. Generated/protobuf code, vendored dependencies, build/data
files, and `include/utilities/json.hpp` are excluded. `.clang-format-ignore` is
respected. There is no repository-wide formatting during commit.

**Partially staged eligible files are rejected before any write**, as are
unstaged formatter-policy changes. Unstaged edits are preserved. Stage the whole
file or temporarily stash unstaged edits with `git stash push --keep-index`, then
restore them and resolve conflicts afterward. Index locks, executable modes,
symlink checks, alternate indexes and editor-save checks remain intact. Push
formatting is always read-only.

## Pushed ranges and analysis

The hook consumes **every** pre-push input row, groups identical commit tips, and
uses the remote OID supplied by Git. It includes paths touched by **every
introduced commit**, even reverted changes, plus the final tree diff for force
updates. Rename detection is disabled for path discovery so both old and new
names are retained; deletions have no file to format/analyze. Paths are NUL
separated, including spaces and newlines. Ref deletions need no source checks.

A new ref has no authoritative base. Local tracking refs may be stale after
remote deletion or force-push, so new refs conservatively select all tracked
files. An unavailable remote OID uses the same visible fallback, without fetching
while the connection is open. Cached tidy results still apply. Multiple distinct
tips use persistent, isolated local clones/builds under
`.polygraph/revisions/<commit>/`; they never reset the developer's checkout or
share build trees with different absolute source paths. Their first builds may
be expensive. Tags that peel to commits follow the same verification rules;
objects that cannot peel to commits fail clearly.

CMake's existing `build/cpu-tests` is reused. Only missing/incompatible
configuration or changed presets trigger explicit configuration. Ninja regenerates
for changed CMake inputs and compiles only stale targets. Existing dependencies
are retained; FetchContent updates are disconnected. A cold build can still need
to download missing pinned dependencies. Build-time formatting/tidy remain off,
so compilation does not duplicate the analysis stage. No build tree is deleted.

The build runs **once before tidy**, refreshing generated inputs and Ninja's
transitive dependency records. Changed sources select their CPU compilation
commands; changed project headers add their consumers. Missing/incomplete header
records or headers without known consumers visibly select all CPU units. Object
paths are read from `output` or parsed from `-o` in the database. Duplicate sources
are analyzed once. Changes to CMake, developer policy/scripts or analysis policy
also select all units. A formatting-policy change audits all project sources.

Each successful TU analysis is keyed by its source and transitive dependency
contents (including generated, external and system headers), compilation database,
CMake cache/build rules, developer/configuration files, tool versions/executable
contents, linked implementation libraries and LLVM resource headers, relevant process environment and project/build header file inventory.
Only successful completed analyses with complete Ninja records are cached;
failures/interruption cannot add a success. Corrupt/missing cache data is a miss.
Changing an input invalidates its result even if its timestamp is restored.
Cache state is ignored local data in `.polygraph/`; removing that directory is
safe but makes the next analysis cold. Concurrent gates sharing a build fail
clearly instead of racing.

The CPU preset excludes CUDA, HTTP server, metrics and some optional analysis
sources. Changes there produce explicit coverage diagnostics; unexpected missing
CPU sources fail. The fast and full CPU profiles do **not** provide GPU/HTTP
build or runtime validation; use the appropriate feature presets separately.

## Smoke, full, and sanitizer coverage

The ten smoke tests are named explicitly in `SMOKE_TESTS` in the driver: dense-ID
mapping and rejection, CLI defaults and required arguments, BFS classic and
push/pull policy transitions, filtered pull, and independent-oracle diamond and
cycle cases. Discovery must find **every** required enabled test; missing,
disabled or unbuilt tests fail. Execution JUnit results must also contain every
required test with no runtime skips, failures or errors (CTest alone treats skips
as success). CTest uses `--no-tests=error` and propagates errors.
The selection deliberately omits Redis socket tests, the remaining unit/oracle
cases, and repeated concurrency stress. A bug outside the smoke selection can
pass `fast`; the mandatory pre-merge manual full workflow closes that coverage
gap. There are no expected-failure markers or silent retries.

`full` analyzes every CPU TU afresh, requires nonempty enabled unit/correctness/
concurrency registrations, runs all CTest tests, and repeats concurrency 50 times.
GitHub Actions' manual `full` selection includes full-repository formatting,
fresh full CPU tidy, all CPU tests, correctness, repeated concurrency and the
isolated Python oracle. No success cache can weaken explicit full tidy.

This checkout had no existing sanitizer gate/presets. `cpu-asan` (ASan+UBSan) and
`cpu-tsan` (TSan) now provide separate Ninja directories. Both require a supported
GCC/Clang Linux runtime. TSan cannot be combined with ASan; address-space
restrictions, incompatible prebuilt libraries or unavailable runtime support
cause a reported failure, not a skipped success. These profiles are optional
manual verification and are not part of ordinary pushes.

## Verify before opening SSH

Git's native pre-push hook runs after remote contact, which can leave an SSH
connection idle during a cold build/analysis. For long checks use:

```bash
python3 tools/dev/quality.py verified-push origin
python3 tools/dev/quality.py verified-push origin HEAD:refs/heads/my-branch
python3 tools/dev/quality.py verified-push --profile full origin HEAD:refs/heads/my-branch
```

This intentionally pushes when invoked. With no refspec, the current branch is
pushed to the same branch name. Explicit non-force `source:refs/heads/destination`
refspecs and multiple refs are supported. Normal `git push` remains available for
other options, tags, deletions and force-with-lease. The wrapper does not weaken
Git's ref-update/fast-forward rules, add force flags or use `--no-verify`.

The wrapper resolves immutable source OIDs and performs the gate **before**
starting `git push`. Its temporary success certificate binds the exact commit,
tree, validated file scope and profile to configuration, environment, tools,
transitive dependencies, generated test discovery, fixtures and built outputs.
State is checked before analysis/tests and again afterward. The enabled pre-push
hook recomputes state and checks the **actual** server-advertised scope. If the
remote moved so the preflight scope no longer covers it, or any relevant input
changed, the hook fails promptly; it does not rebuild while SSH is connected.
Refresh tracking refs (`git fetch --prune <remote>`) and rerun preflight. Fetching
is explicit, so ordinary hooks never add network traffic. Certificates live only
for that invocation and are removed afterward. Tidy caching alone never skips
builds or tests in a normal push.

## Regression checks and measurements

```bash
python3 -m unittest discover -s tools/dev -p 'test_*.py' -v
python3 -m unittest discover -s tools/dev -p check_pre_commit_test.py -v
```

All **43 gate regressions** and **18 LLVM-backed staged-formatting regressions**
passed. Full-repository formatting checked all 254 owned files; Actionlint and
ShellCheck passed. Real certificate checks accepted unchanged state and rejected
an added generated build input. A real certified gate plus a further state
recheck passed in 11.24 seconds; the remote push itself was simulated only in
regression fixtures.

Tests use disposable repositories and synthetic commit objects. They exercise
multiple commits and tips, new branches/stale tracking refs, missing remote
objects, renames/deletions, filenames with spaces/newlines, header dependency
selection, cache input changes, smoke discovery, failure propagation, and
verified-push preflight/certificate reuse. They never push to a remote. CTest's
`python.developer_quality` runs the gate regressions in full CPU CI; the separate
formatting job runs LLVM-backed staged-formatting regressions.

Measured locally on 2026-11-03 with the existing CPU build, logs under ignored
`build/quality-incremental/` (not GitHub-hosted measurements):

| Scenario | Local elapsed | Result |
| --- | ---: | --- |
| Old configure / idle build | 0.73s / 0.13s | Unnecessary configure; build already incremental |
| Old full tidy, 94 TUs | 439.78s | Passed |
| Old “fast” tests, 468 tests | 21.37s | 467 passed; Redis fake-server socket unavailable in sandbox |
| New gate, no source changes (`fast --base HEAD`) | 0.39s | Passed, build and ten smoke tests executed |
| Actual modified `.cpp`, cold / warm tidy | 6.96s / 0.98s | One TU; measured during initial implementation |
| Final driver, single `.cpp` scope, cold / warm tidy | 5.88s / 1.93s | Includes shared-library/resource fingerprinting and strict smoke results |
| Actual modified `CommandLine.hpp`, cold / warm | 58.81s / 1.06s | Eight affected TUs, 13 build steps initially |
| Changed CMake configuration | 417.40s | Ninja regenerated; all 94 TUs selected; smoke passed |
| Complete tests plus 25 concurrency tests × 50 | 42.70s | All 468 tests passed with loopback sockets enabled |
| Broken formatting | 0.11s | Rejected before build |
| Broken compilation | 12.45s | Rejected at build; no tidy/tests |
| Broken smoke assertion | 9.90s | Rejected at smoke tests |

The measured old stage sum is about **462 seconds** (not an observed 15-minute
end-to-end push). A one-source cold check is about **66× faster** here. The
5–20-second goal is met for that ordinary case; broad header fanout, changed
configuration, an empty cache or initial builds can exceed it. Necessary work is
reported, never silently omitted. Temporary probe edits were restored and the
CPU build rebuilt afterward. No commit, remote push or workflow dispatch was
performed during this implementation. Both sanitizer presets configured
successfully using existing dependency sources;
a minimal ASan+UBSan executable ran successfully. TSan exited 66 with
`unexpected memory mapping` in this environment. Complete sanitizer builds/suites
and hosted full CI have not been executed here and require supported runners.
