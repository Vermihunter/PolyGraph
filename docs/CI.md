# CPU CI

[`.github/workflows/cpu-ci.yml`](../.github/workflows/cpu-ci.yml) runs **only through
`workflow_dispatch`** using Ubuntu 24.04. Choose `full`, `unit`, `correctness`,
`concurrency`, or `oracle`; `full` runs every existing check. There are no push,
pull-request, or scheduled triggers. See [DEVELOPMENT.md](DEVELOPMENT.md) for the
automatic local pre-push gate. It uses the existing `cpu-tests`
configure/build/test presets; it does not redefine their feature flags.

| Required check | Work performed | Job limit |
| --- | --- | --- |
| CPU / unit | Build every target enabled by `cpu-tests`, then run the entire CTest suite | 30 minutes |
| CPU / correctness | Fresh preset build, then `ctest -L '^correctness$'` | 30 minutes |
| CPU / concurrency | Fresh preset build, then `ctest -L '^concurrency$' --repeat until-fail:50` | 30 minutes |
| Python oracle and isolation | Standard-library reference suite, explicit isolation checks and CI runner regression tests | 10 minutes |

The first three checks share one matrix definition with `fail-fast: false`.
Each independently configures and builds the source checked out for that event;
one failing check cannot cancel the others. This repeats compilation across
runners but avoids transferring binaries/CMake caches with embedded absolute
paths. Compiler object caching (`ccache`, limited to 300 MB per check) reduces
repeat work. Cache keys include the check, build configuration hash and commit,
with fallbacks for earlier commits of the same check. CMake build trees,
generated oracle fixtures, executables and datasets are never restored from cache.

The `cpu-tests` preset enables the CPU libraries, backend adapters, BFS CLI,
benchmark executable and tests. HTTP/CUDA remain disabled according to the
preset. Adapter compilation requires development libraries, not database servers.
The existing Redis test opens an ephemeral loopback fake-server socket; it does
not contact a real Redis service. No Compose services are started, and neither
the full LDBC SF0.1 snapshot nor GPU hardware is needed.

Install dependencies on Ubuntu 24.04:

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends -y \
  build-essential cmake ninja-build pkg-config ccache git ca-certificates \
  libcurl4-openssl-dev libpq-dev libspdlog-dev libgtest-dev \
  nlohmann-json3-dev python3
```

The preset requires CMake 3.27+ and C++23. CMake fetches the repository-pinned
p-ranav/argparse v3.2 if unavailable; outbound HTTPS is required on a cold build.
Installed GoogleTest/spdlog are preferred. The oracle and registered Python CTest
tests require **only Python 3.10+ and its standard library**. Do not install
`tools/requirements.txt` or plotting/benchmark packages for CI. The oracle job
uses `python3 -I -S` to exclude environment/user import paths and site packages.

Local reproduction, from a fresh checkout containing Phase 3:

```bash
cmake --preset cpu-tests -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build --preset cpu-tests
mkdir -p build/ci-logs/local

ctest --preset cpu-tests --no-tests=error --parallel 2 --timeout 60 \
  --output-log "$PWD/build/ci-logs/local/unit.log" \
  --output-junit "$PWD/build/ci-logs/local/unit.xml"
ctest --preset cpu-tests -L '^correctness$' --no-tests=error --parallel 2 --timeout 60 \
  --output-log "$PWD/build/ci-logs/local/correctness.log" \
  --output-junit "$PWD/build/ci-logs/local/correctness.xml"
ctest --preset cpu-tests -L '^concurrency$' --no-tests=error --parallel 2 --timeout 60 \
  --repeat until-fail:50 \
  --output-log "$PWD/build/ci-logs/local/concurrency.log" \
  --output-junit "$PWD/build/ci-logs/local/concurrency.xml"

python3 -I -S .github/scripts/test_oracle_runner.py
python3 -I -S .github/scripts/run_oracle_tests.py
python3 -I -S .github/scripts/run_oracle_tests.py --isolation-only
```

CTest's `--no-tests=error` makes missing registrations/labels fail rather than
silently pass. `--repeat until-fail:50` stops and fails on the first failure; it
is not a retry-until-success policy. Existing 20/30-second per-test timeout
properties remain active; `--timeout 60` provides a default for tests without a
timeout. Configure, build and CTest steps have additional wall-clock limits.

The Python runner rejects empty suites, any skipped test, failed assertions and
import errors. Its five regression tests check exit status for empty, failing,
skipped and successful selections, plus a missing isolation module even when
other discovered tests pass. The unchanged Phase 2 independence tests
inspect oracle imports and run a fresh interpreter with production imports and
production-file access explicitly blocked. They are run both in the full oracle
suite and as a separate isolation selection. See
[`tests/reference/test_independence.py`](../tests/reference/test_independence.py).

All shell pipelines use `set -euo pipefail`; `tee` cannot hide configure, build or
test failures. No test is skipped, suppressed or marked as an expected failure.
The workflow stores discovery JSON, full command output, JUnit XML, CMake
configuration details and CTest temporary diagnostics. On failure these upload
as `cpu-<suite>-diagnostics` or `python-oracle-diagnostics` artifacts retained for
14 days. Hard runner termination can prevent the final upload step.

The token has only `contents: read`; checkout does not persist credentials.
There are no secrets, write permissions, deployments, `pull_request_target`, or
privileged service containers. Official actions are pinned to full commit SHAs
with release comments. Update those pins together with their comments after
reviewing upstream releases. Superseded manual runs of the same suite/ref are cancelled.

The current committed CLI still accepts duplicate aliases. The original
`RejectsUnknownAndDuplicateOptions` test remains active and correctly makes unit
and fast local gates fail. No expected-failure marker, filter or skip hides it.
The earlier local CI implementation included an alias fix/regression proposal,
but that production change is absent from this checkout. Current quality-gate
verification and blockers are documented in [DEVELOPMENT.md](DEVELOPMENT.md).

Historical local verification from the initial CI implementation (not results
for the current committed CLI or a GitHub-hosted run):

- Full CPU build and **470/470 CTest tests passed**, also repeated from a fresh
  Git clone of the Phase 3 baseline with only these CI/CLI changes overlaid.
- BFS correctness: **323/323 passed** against the unchanged independent oracle.
- Concurrency: **25 tests × 50 = 1,250 successful executions**.
- Oracle: **88/88 passed** in `-I -S` mode; explicit isolation **2/2 passed**;
  CI runner regression tests **5/5 passed**.
- Additional local Python checks: `python3 -m pytest tests
  tools/measurement/test_csv_compare.py -q` passed **106 tests and 3,246 subtests**.
- Empty CTest label selection with `--no-tests=error` returned status **8**.
- The existing CLI regression and both added regression tests were observed
  failing before the alias fix; all **13 CLI tests** pass afterward.
- Workflow syntax and embedded Bash passed **actionlint 1.7.7 with ShellCheck
  0.10.0**. Reproduce with `actionlint .github/workflows/cpu-ci.yml` with
  `shellcheck` on PATH (or `actionlint -shellcheck=/path/to/shellcheck ...`).

Phase 3 was found on `origin/feature/testing` at `c27cb8b`, directly after the
initial `master` checkout (`e719776`). Its ten existing test files were restored
unchanged as the baseline, rather than implementing new testing phases.
Those files are `tests/CMakeLists.txt`, `tests/PHASE3.md`,
`tests/bfs_concurrency_test.cpp`, `tests/bfs_contract_test.cpp`,
`tests/executor_test_labels.cmake`, `tests/generate_executor_fixtures.py`,
`tests/scripted_graph_store_test.cpp`, `tests/support/ExecutorContract.hpp`,
`tests/support/ScriptedGraphStore.cpp`, and `tests/support/ScriptedGraphStore.hpp`. Include
that existing commit (or those files) when integrating CI. The historical
[`tests/PHASE3.md`](../tests/PHASE3.md) reports the CLI failure before this fix.

Local execution uses the available Ubuntu 22.04/GCC 11 environment and locally
cached FetchContent sources. This does not validate Ubuntu 24.04's apt transaction,
GitHub checkout/cache/artifact services, hosted runner resources or repository
branch-protection settings. No workflow run was dispatched or commit pushed.
Manual workflows are optional checks. If mandatory remote enforcement is
introduced later, enable automatic events deliberately and then configure
branch protection/rulesets with the appropriate required check names.
