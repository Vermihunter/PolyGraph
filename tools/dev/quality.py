#!/usr/bin/env python3
"""CPU verification and staged-file formatting hooks. Only formatting/install commands mutate state."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import stat
import tempfile

SOURCE_ROOTS = {"include", "src", "app", "tests"}
EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp", ".tpp", ".cu", ".cuh"}
TRANSLATION_UNITS = {".c", ".cc", ".cpp", ".cxx"}
EXCLUDED_PARTS = {"generated", "third_party", "third-party", "vendor", "external", "_deps", "build", "data", "datasets"}
FULL_ANALYSIS = {".clang-format", ".clang-tidy", "CMakeLists.txt", "CMakePresets.json"}
OPTIONAL_CPU_FILES = {
    "src/analysis/AlgorithmAnalyzer.cpp",
    "src/observability/metrics/MetricsService.cpp",
    "src/observability/metrics/collectors/GlibcMetricsCollector.cpp",
    "src/observability/metrics/collectors/GpuMetricsCollector.cpp",
    "src/observability/metrics/collectors/JemallocMetricsCollector.cpp",
    "src/observability/metrics/collectors/ProcessMetricsCollector.cpp",
}
OPTIONAL_CPU_PREFIXES = ("src/gpu/", "include/gpu/", "src/server/", "include/server/", "app/http_server/")


class GateError(RuntimeError):
    pass


def run(args: list[str], root: Path, *, capture: bool = False, check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(args, cwd=root, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None, timeout=900)
    if check and result.returncode:
        detail = result.stderr.decode(errors="replace") if capture else "see tool diagnostics above"
        raise GateError(f"{shlex.join(args)} failed (exit {result.returncode}): {detail}")
    return result


def git(root: Path, *args: str) -> bytes:
    return run(["git", *args], root, capture=True).stdout


def project_source(name: str) -> bool:
    path = Path(name)
    return (not path.is_absolute() and path.parts and path.parts[0] in SOURCE_ROOTS
            and path.suffix in EXTENSIONS and not (set(path.parts) & EXCLUDED_PARTS)
            and not any(p.startswith(("build-", "cmake-build-")) for p in path.parts)
            and not path.name.endswith((".pb.cc", ".pb.h", ".grpc.pb.cc", ".grpc.pb.h"))
            and name != "include/utilities/json.hpp")


def decode_paths(data: bytes) -> set[str]:
    return {os.fsdecode(part) for part in data.split(b"\0") if part}


def changed_files(root: Path, base: str | None) -> set[str]:
    if base is None:
        return decode_paths(git(root, "ls-files", "-z"))
    # Resolve first: a ref beginning with '-' must never become a Git option.
    oid = git(root, "rev-parse", "--verify", "--end-of-options", base + "^{commit}").decode().strip()
    return decode_paths(git(root, "diff", "--name-only", "--diff-filter=ACMRTD", "-z", oid, "HEAD", "--"))


def requires_full_analysis(names: set[str]) -> bool:
    return any(n in FULL_ANALYSIS or Path(n).name in {".clang-format", "_clang-format", ".clang-format-ignore", ".clang-tidy"}
               or n.startswith(("cmake/", "src/cmake/", "tools/dev/"))
               or n.endswith("/CMakeLists.txt") for n in names)


def verify_clean_revision(root: Path, expected: str) -> None:
    if git(root, "rev-parse", "HEAD").decode().strip() != expected:
        raise GateError("HEAD changed during verification; check out the tip being pushed and retry")
    if git(root, "status", "--porcelain=v1", "-z", "--untracked-files=normal", "--ignore-submodules=none"):
        raise GateError("push blocked: tracked/index changes or untracked files exist. Commit or stash them "
                        "(including untracked files) before pushing; ignored build/data files are allowed")


def push_scope(root: Path, protocol: str) -> tuple[str | None, set[str]]:
    head = git(root, "rev-parse", "HEAD").decode().strip()
    changed: set[str] = set()
    has_tip = False
    full = False
    for line in protocol.splitlines():
        fields = line.split()
        if len(fields) != 4 or not all(re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", fields[i]) for i in (1, 3)):
            raise GateError("invalid Git pre-push ref input")
        _, local_oid, _, remote_oid = fields
        if set(local_oid) == {"0"}:  # Deletion only: no source revision is introduced.
            continue
        tip = git(root, "rev-parse", "--verify", local_oid + "^{commit}").decode().strip()
        if tip != head:
            raise GateError("push blocked: a pushed tip is not checked-out HEAD. Check out that revision and "
                            "push it separately; multi-tip pushes to different commits are unsupported")
        has_tip = True
        remote = run(["git", "rev-parse", "--verify", remote_oid + "^{commit}"], root, capture=True, check=False)
        if set(remote_oid) == {"0"} or remote.returncode:
            full = True
        else:
            changed.update(changed_files(root, remote_oid))
    if not has_tip:
        return None, set()
    verify_clean_revision(root, head)
    return head, changed_files(root, None) if full else changed


def clang_tool(kind: str) -> str:
    tool = os.environ.get(kind.upper().replace("-", "_"), kind + "-21")
    found = shutil.which(tool)
    if not found:
        raise GateError(f"missing {tool}; install LLVM 21 tools or set {kind.upper().replace('-', '_')} to its executable")
    result = subprocess.run([found, "--version"], capture_output=True, text=True, check=True, timeout=30)
    if not re.search(r"version 21\.", result.stdout, re.IGNORECASE):
        raise GateError(f"{kind} 21 is required for reproducible formatting/C++23 parsing: {result.stdout.strip()}")
    return found


def cuda_format_probe(root: Path, tool: str, files: list[str]) -> None:
    if any(Path(n).suffix in {".cu", ".cuh"} for n in files):
        # Exercise kernel-launch syntax with the selected version before allowing
        # it near CUDA files. No C++ compiler flags or CUDA SDK are assumed.
        probe = "__global__ void k(int *p){*p=1;} void launch(int*p){k<<<1,32>>>(p);}\n"
        parsed = subprocess.run([tool, "--style=file", "--assume-filename=" + str(root / "probe.cu")],
                                input=probe, capture_output=True, text=True, check=True, timeout=30)
        if not all(token in parsed.stdout for token in ("__global__", "<<<", ">>>")):
            raise GateError("selected clang-format did not preserve the CUDA kernel-launch probe")
        print("[format] LLVM 21 CUDA kernel-launch compatibility probe passed", flush=True)


def formatter_ignores(root: Path, tool: str, name: str) -> bool:
    # For ignored stdin paths clang-format returns success with *empty output*.
    # Query ignore status explicitly; never interpret that output as source.
    result = run([tool, "--list-ignored", str(root / name)], root, capture=True)
    if result.stdout:
        print("[format] Excluded by .clang-format-ignore: " + name, flush=True)
        return True
    return False


def check_format(root: Path, names: set[str], *, fix: bool = False) -> None:
    files = sorted(n for n in names if project_source(n) and (root / n).is_file())
    if not files:
        print("[format] No project source changes", flush=True)
        return
    for name in files:
        if not (root / name).resolve().is_relative_to(root):
            raise GateError("source symlink leaves repository: " + name)
    tool = clang_tool("clang-format")
    cuda_format_probe(root, tool, files)
    failures = []
    for name in files:
        if formatter_ignores(root, tool, name):
            continue
        args = [tool, "--style=file", "-i"] if fix else [tool, "--style=file", "--dry-run", "--Werror"]
        result = run([*args, str(root / name)], root, capture=not fix, check=False)
        if result.returncode:
            failures.append(name)
            if fix:
                raise GateError(f"formatting failed: {name}")
    if failures:
        print("[format] Files needing formatting:\n" + "\n".join("  " + n for n in failures), file=sys.stderr)
        print("Fix explicitly: python3 tools/dev/quality.py format-fix --files " + shlex.join(failures), file=sys.stderr)
        raise GateError(f"{len(failures)} file(s) fail clang-format; no files were modified")
    print(f"[format] {'Formatted' if fix else 'Validated'} {len(files)} file(s)", flush=True)


def git_input(root: Path, args: list[str], data: bytes, env: dict[str, str] | None = None) -> bytes:
    result = subprocess.run(["git", *args], cwd=root, input=data, capture_output=True, env=env, timeout=30)
    if result.returncode:
        raise GateError(f"git {shlex.join(args)} failed: {os.fsdecode(result.stderr)}")
    return result.stdout


def index_entries(root: Path) -> dict[str, tuple[str, str]]:
    entries = {}
    for record in git(root, "ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        metadata, name = record.split(b"\t", 1)
        mode, oid, stage = metadata.decode("ascii").split()
        if stage != "0":
            raise GateError("resolve unmerged index entries before committing")
        entries[os.fsdecode(name)] = (mode, oid)
    return entries


def candidate_bytes(root: Path, name: str) -> tuple[bytes, int]:
    path = root / name
    if path.is_symlink() or not path.resolve().is_relative_to(root) or not path.is_file():
        raise GateError("staged source must be a regular file inside the checkout: " + name)
    return path.read_bytes(), stat.S_IMODE(path.stat().st_mode)


def format_staged(root: Path) -> None:
    # Diff the active index, including GIT_INDEX_FILE used by partial/pathspec
    # commits. Added/modified/renamed files only; deleted and unrelated files stay.
    names = sorted(n for n in decode_paths(git(root, "diff", "--cached", "--name-only", "-z",
                                               "--diff-filter=ACMRT", "--")) if project_source(n))
    if not names:
        print("[pre-commit] No eligible staged C++/CUDA files", flush=True)
        return
    entries = index_entries(root)
    original = {}
    for name in names:
        if name not in entries or entries[name][0] not in {"100644", "100755"}:
            raise GateError("staged source must be a regular index entry: " + name)
        data, mode = candidate_bytes(root, name)
        if data != git(root, "cat-file", "blob", entries[name][1]):
            raise GateError("partially staged source (or EOL/clean-filter conversion): " + name +
                            ". Nothing formatted or staged. Stage the whole file, or stash its unstaged "
                            "edits with --keep-index before retrying; restore them afterward and resolve conflicts")
        original[name] = (data, mode)
    # clang-format reads working-tree policy files. Do not let unstaged or
    # untracked policy silently control what is committed.
    policies = {".clang-format"}
    for name in names:
        for parent in (root / name).parents:
            if not parent.is_relative_to(root):
                break
            for filename in (".clang-format", "_clang-format", ".clang-format-ignore"):
                path = parent / filename
                relative = path.relative_to(root).as_posix()
                policies.add(relative)
    absent_policies = set()
    for name in sorted(policies):
        path = root / name
        if name != ".clang-format" and not path.exists() and not path.is_symlink() and name not in entries:
            absent_policies.add(name)
            continue
        if name not in entries or entries[name][0] not in {"100644", "100755"}:
            raise GateError("formatter configuration must be staged/tracked before committing: " + name)
        data, mode = candidate_bytes(root, name)
        if data != git(root, "cat-file", "blob", entries[name][1]):
            raise GateError("formatter configuration has unstaged changes: " + name + "; stage or stash them first")
        original[name] = (data, mode)

    def verify_inputs() -> None:
        for name, snapshot in original.items():
            if candidate_bytes(root, name) != snapshot:
                raise GateError("working file or formatter configuration changed during formatting: " + name)
        for name in absent_policies:
            path = root / name
            if path.exists() or path.is_symlink():
                raise GateError("formatter configuration appeared during formatting: " + name + "; stage or stash it first")

    tool = clang_tool("clang-format")
    cuda_format_probe(root, tool, names)
    formatted = {}
    for name in names:
        if formatter_ignores(root, tool, name):
            continue
        result = subprocess.run([tool, "--style=file", "--assume-filename=" + str(root / name)],
                                cwd=root, input=original[name][0], capture_output=True, timeout=30)
        if result.returncode:
            raise GateError("formatter failed for " + name + ": " + os.fsdecode(result.stderr))
        if original[name][0].strip() and not result.stdout.strip():
            raise GateError("formatter unexpectedly produced empty source for " + name + "; nothing staged")
        if result.stdout != original[name][0]:
            formatted[name] = result.stdout
    verify_inputs()
    if not formatted:
        print(f"[pre-commit] {len(names)} staged file(s) already formatted; index/worktree unchanged", flush=True)
        return
    index = Path(os.fsdecode(git(root, "rev-parse", "--git-path", "index")).removesuffix("\n"))
    if not index.is_absolute():
        index = root / index
    snapshot = index.read_bytes()
    # Own the real index lock, prepare updates on a private index in the same
    # directory (also supports split indexes), then atomically replace the index.
    # Never `git add` a working file: stage exactly our formatted blob and mode.
    lock = index.with_name(index.name + ".lock")
    try:
        descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError as error:
        raise GateError("index is locked by another Git operation; wait for it to finish and retry") from error
    os.close(descriptor)
    temporary_index = None
    try:
        if index.read_bytes() != snapshot or index_entries(root) != entries:
            raise GateError("index changed during formatting; retry the commit")
        verify_inputs()
        fd, temporary = tempfile.mkstemp(prefix="quality-index-", dir=index.parent)
        temporary_index = Path(temporary)
        with os.fdopen(fd, "wb") as stream:
            stream.write(snapshot)
        os.chmod(temporary_index, stat.S_IMODE(index.stat().st_mode))
        env = {**os.environ, "GIT_INDEX_FILE": str(temporary_index)}
        updates = b""
        for name, data in formatted.items():
            oid = git_input(root, ["hash-object", "-w", "--stdin"], data).strip()
            updates += entries[name][0].encode() + b" " + oid + b"\t" + os.fsencode(name) + b"\0"
        git_input(root, ["update-index", "-z", "--index-info"], updates, env)
        for name, data in formatted.items():
            if candidate_bytes(root, name) != original[name]:
                raise GateError("working file changed; formatted files may remain unstaged, review git diff: " + name)
            path = root / name
            fd, temporary_file = tempfile.mkstemp(prefix=".quality-format-", dir=path.parent)
            try:
                with os.fdopen(fd, "wb") as stream:
                    stream.write(data)
                    os.fchmod(stream.fileno(), original[name][1])
                # Preparing/writing the temporary file can take time. Recheck
                # after that work too, so an editor save is not overwritten.
                if candidate_bytes(root, name) != original[name]:
                    raise GateError("working file changed while preparing formatting; retry without editing: " + name)
                os.replace(temporary_file, path)
            finally:
                Path(temporary_file).unlink(missing_ok=True)
        os.replace(temporary_index, index)
        print("[pre-commit] Formatted and staged only:\n" + "\n".join("  " + n for n in formatted), flush=True)
    finally:
        if temporary_index is not None:
            temporary_index.unlink(missing_ok=True)
            temporary_index.with_name(temporary_index.name + ".lock").unlink(missing_ok=True)
        lock.unlink()


def configure(root: Path) -> None:
    run(["cmake", "--preset", "cpu-tests", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
         "-DPOLYGRAPH_ENABLE_GPU=OFF", "-DPOLYGRAPH_ENABLE_CLANG_FORMAT=OFF",
         "-DPOLYGRAPH_ENABLE_CLANG_TIDY=OFF", "-DCMAKE_CXX_CLANG_TIDY="], root)
    path = root / "build/cpu-tests/compile_commands.json"
    if not path.is_file() or not json.loads(path.read_text()):
        raise GateError("CMake produced no compilation database: " + str(path))


def compile_units(root: Path) -> list[tuple[Path, Path]]:
    path = root / "build/cpu-tests/compile_commands.json"
    if not path.is_file():
        raise GateError("missing compile_commands.json; run the configure check first")
    units = []
    for entry in json.loads(path.read_text()):
        directory = Path(entry["directory"])
        source = (directory / entry["file"]).resolve()
        if not source.is_relative_to(root) or not project_source(source.relative_to(root).as_posix()):
            continue
        if source.suffix not in TRANSLATION_UNITS:
            continue
        # CMake's output field lets Ninja dependency records map exactly to TUs.
        output = entry.get("output")
        if output:
            units.append((source, (directory / output).resolve()))
        else:
            # No dependency mapping: selecting a changed header falls back to all.
            units.append((source, Path("/no-dependency-output")))
    if not units:
        raise GateError("compilation database contains no project-owned CPU translation units")
    return units


def ninja_dependencies(root: Path) -> dict[Path, set[Path]]:
    build = root / "build/cpu-tests"
    result = run(["ninja", "-C", str(build), "-t", "deps"], root, capture=True, check=False)
    if result.returncode:
        return {}
    records: dict[Path, set[Path]] = {}
    current = None
    for line in os.fsdecode(result.stdout).splitlines():
        match = re.match(r"^(.*): #deps .* \((VALID|STALE)\)$", line)
        if match:
            current = (build / match[1]).resolve() if match[2] == "VALID" else None
            if current is not None:
                records[current] = set()
        elif current is not None and line.startswith("    "):
            records[current].add((build / line[4:]).resolve())
        elif line:
            current = None
    return records


def selected_units(root: Path, names: set[str], full: bool) -> list[Path]:
    units = compile_units(root)
    changed_sources = {root / n for n in names if project_source(n) and Path(n).suffix in TRANSLATION_UNITS}
    headers = {root / n for n in names if project_source(n) and Path(n).suffix not in TRANSLATION_UNITS | {".cu", ".cuh"}}
    known = {source for source, _ in units}
    deleted = {p for p in changed_sources if not p.is_file()}
    for path in sorted(deleted):
        print(f"[tidy] Removed source (no translation unit to analyze): {path.relative_to(root)}", flush=True)
    changed_sources -= deleted
    missing = changed_sources - known
    unsupported = {p for p in missing if p.relative_to(root).as_posix().startswith(OPTIONAL_CPU_PREFIXES)
                   or p.relative_to(root).as_posix() in OPTIONAL_CPU_FILES}
    for path in sorted(unsupported):
        print(f"[tidy] Outside CPU preset (not analyzed): {path.relative_to(root)}", flush=True)
    if missing - unsupported:
        raise GateError("changed source missing from CPU compilation database: " + shlex.join(str(p) for p in sorted(missing - unsupported)))
    if full or requires_full_analysis(names):
        return sorted(known)
    selected = changed_sources & known
    if headers:
        dependencies = ninja_dependencies(root)
        # Missing/incomplete records or a header with no recorded consumer: all
        # CPU TUs is the conservative fallback (including a freshly added include).
        if any(output not in dependencies for _, output in units) or any(
                not any(header in deps for deps in dependencies.values()) for header in headers):
            print("[tidy] Header dependencies incomplete; analyzing all CPU translation units", flush=True)
            return sorted(known)
        selected.update(source for source, output in units if dependencies[output] & headers)
    return sorted(selected)


def check_tidy(root: Path, names: set[str], *, full: bool = False) -> None:
    for name in sorted(names):
        if project_source(name) and Path(name).suffix in {".cu", ".cuh"}:
            print(f"[tidy] CUDA source requires a CUDA compilation database; CPU gate formats only: {name}", flush=True)
    units = selected_units(root, names, full)
    if not units:
        print("[tidy] No affected CPU translation units", flush=True)
        return
    tool = clang_tool("clang-tidy")
    for index, source in enumerate(units, 1):
        print(f"[tidy] {index}/{len(units)} {source.relative_to(root)}", flush=True)
        run([tool, "-p", str(root / "build/cpu-tests"),
             "--config-file=" + str(root / "tools/dev/clang-tidy.yaml"),
             "--header-filter=^" + re.escape(str(root)) + r"/(include|src|app|tests)/",
             "--exclude-header-filter=" + re.escape(str(root / "include/utilities/json.hpp")) +
             r"|/(generated|third_party|third-party|vendor|external|_deps|build|data|datasets)/",
             str(source)], root)


def build(root: Path) -> None:
    run(["cmake", "--build", "--preset", "cpu-tests"], root)


def fast_tests(root: Path) -> None:
    # Validate both labels independently, then run their union once. Deterministic
    # concurrency tests run once; integration/service/dataset/stress labels do not.
    excluded = "^(integration|database|external|ldbc|slow|stress)$"
    for label in ("unit", "correctness"):
        listing = run(["ctest", "--preset", "cpu-tests", "-L", "^" + label + "$", "-LE", excluded,
                       "--show-only=json-v1"], root, capture=True)
        tests = json.loads(listing.stdout).get("tests", [])
        if not tests:
            raise GateError(f"required fast test selection is empty: {label}")
        for test in tests:
            properties = {p["name"]: p["value"] for p in test.get("properties", [])}
            if properties.get("DISABLED") or test["name"].endswith("_NOT_BUILT"):
                raise GateError(f"required test is disabled or not built: {test['name']}")
    run(["ctest", "--preset", "cpu-tests", "-L", "^(unit|correctness)$", "-LE", excluded,
         "--no-tests=error", "--output-on-failure", "--timeout", "60", "--parallel", "2"], root)


def sequence(root: Path, names: set[str], full: bool, expected: str | None = None) -> None:
    for title, action in [
        ("clang-format", lambda: check_format(root, names)),
        ("CMake configure", lambda: configure(root)),
        ("clang-tidy", lambda: check_tidy(root, names, full=full)),
        ("CPU build", lambda: build(root)),
        ("fast tests", lambda: fast_tests(root)),
    ]:
        if expected:
            verify_clean_revision(root, expected)
        print(f"[quality] {title}", flush=True)
        action()
    if expected:
        verify_clean_revision(root, expected)
    print("[quality] All selected checks passed", flush=True)


def install(root: Path) -> None:
    existing = run(["git", "config", "--get", "core.hooksPath"], root, capture=True, check=False)
    if existing.returncode == 0 and existing.stdout.decode().strip() != ".githooks":
        raise GateError("core.hooksPath already points elsewhere; integrate existing hooks before overriding it")
    for name in ("pre-commit", "pre-push"):
        if not os.access(root / ".githooks" / name, os.X_OK):
            raise GateError(f".githooks/{name} must be executable (chmod +x .githooks/{name})")
    run(["git", "config", "--local", "core.hooksPath", ".githooks"], root)
    hook_status(root)


def hook_status(root: Path) -> None:
    value = git(root, "config", "--get", "core.hooksPath").decode().strip()
    if value != ".githooks" or not all(os.access(root / ".githooks" / n, os.X_OK) for n in ("pre-commit", "pre-push")):
        raise GateError("pre-commit/pre-push hooks are not active/executable; run tools/dev/install-hooks.sh")
    print("[quality] Active: core.hooksPath=.githooks; executable pre-commit and pre-push hooks (terminal and CLion)", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("format", "format-fix", "configure", "tidy", "build", "test", "verify", "pre-commit", "format-staged", "pre-push", "install", "hook-status"))
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--base", help="compare committed HEAD with this local commit/ref")
    scope.add_argument("--all", action="store_true", help="audit all tracked project sources (default)")
    scope.add_argument("--files", nargs="+", help="explicit repository-relative paths, useful while editing")
    args = parser.parse_args()
    try:
        root = Path(os.fsdecode(git(Path.cwd(), "rev-parse", "--show-toplevel")).removesuffix("\n")).resolve()
        if args.command in {"pre-commit", "format-staged"}:
            format_staged(root)
            return 0
        if args.command == "pre-push":
            expected, names = push_scope(root, sys.stdin.read())
            if expected is None:
                print("[quality] No non-deletion refs to verify", flush=True)
            else:
                full = requires_full_analysis(names)
                if full:
                    names = changed_files(root, None)
                sequence(root, names, full, expected)
            return 0
        if args.command in {"configure", "build", "test", "install", "hook-status"}:
            {"configure": configure, "build": build, "test": fast_tests,
             "install": install, "hook-status": hook_status}[args.command](root)
            return 0
        names = set(args.files) if args.files is not None else changed_files(root, args.base)
        for name in names:
            if args.files is not None and not (root / name).is_file():
                raise GateError("explicit file does not exist: " + name)
            if Path(name).is_absolute() or ".." in Path(name).parts:
                raise GateError("file paths must stay within the repository: " + name)
        full = args.files is None and args.base is None or requires_full_analysis(names)
        if args.command in {"format", "format-fix"}:
            check_format(root, names, fix=args.command == "format-fix")
        elif args.command == "tidy":
            check_tidy(root, names, full=full)
        else:
            sequence(root, names, full)
        return 0
    except (GateError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"[quality] FAILED: {error}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
