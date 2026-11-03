#!/usr/bin/env python3
"""Fast format/build/smoke hooks; opt-in tidy, full and sanitizer verification."""
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
import hashlib
import time
import fcntl
import xml.etree.ElementTree as ET
from contextlib import contextmanager

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


def revision_files(root: Path, tip: str, base: str | None) -> set[str]:
    if base is None:
        return decode_paths(git(root, "ls-tree", "-r", "--name-only", "-z", tip))
    # Include paths touched in every introduced commit, even changes reverted at
    # the tip, and both sides of renames. Final diff also covers force-push removals.
    names = decode_paths(git(root, "diff", "--no-renames", "--name-only", "-z", base, tip, "--"))
    commits = git(root, "rev-list", tip, "^" + base).decode().splitlines()
    if commits:
        names.update(decode_paths(git_input(root, ["diff-tree", "--stdin", "--root", "-r", "-m",
                                                   "--no-commit-id", "--no-renames", "--name-only", "-z"],
                                             ("\n".join(commits) + "\n").encode())))
    return names


def push_scopes(root: Path, protocol: str, remote_name: str = "origin") -> dict[str, set[str]]:
    scopes: dict[str, set[str]] = {}
    for line in protocol.splitlines():
        fields = line.split()
        if len(fields) != 4 or not all(re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", fields[i]) for i in (1, 3)):
            raise GateError("invalid Git pre-push ref input")
        _, local_oid, _, remote_oid = fields
        if set(local_oid) == {"0"}:
            continue
        tip = git(root, "rev-parse", "--verify", local_oid + "^{commit}").decode().strip()
        base = None
        if set(remote_oid) != {"0"}:
            result = run(["git", "rev-parse", "--verify", remote_oid + "^{commit}"], root, capture=True, check=False)
            if result.returncode == 0:
                base = result.stdout.decode().strip()
            else:
                print("[scope] Remote object unavailable; fallback to all files at " + tip, flush=True)
        else:
            # Tracking refs can retain history deleted/force-pushed on the server.
            # Git supplies no authoritative base for a new ref: never use those
            # hints to exclude files that may actually be new to this remote.
            print("[scope] New ref has no authoritative base; fallback to all files at " + tip, flush=True)
        scopes.setdefault(tip, set()).update(revision_files(root, tip, base))
    return scopes


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
    run(["cmake", "--preset", "cpu-tests", "-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON",
         "-DPOLYGRAPH_ENABLE_GPU=OFF", "-DPOLYGRAPH_ENABLE_CLANG_FORMAT=OFF",
         "-DPOLYGRAPH_ENABLE_CLANG_TIDY=OFF", "-DCMAKE_CXX_CLANG_TIDY=", "-DCMAKE_C_CLANG_TIDY=",
         "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON"], root)
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
        # Older CMake databases expose the object only in the command.
        output = entry.get("output")
        if not output:
            arguments = entry.get("arguments") or shlex.split(entry.get("command", ""))
            if "-o" in arguments:
                output = arguments[arguments.index("-o") + 1]
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
    resolved = {}
    def resolve(name: str) -> Path:
        if name not in resolved:
            resolved[name] = (build / name).resolve()
        return resolved[name]
    current = None
    for line in os.fsdecode(result.stdout).splitlines():
        match = re.match(r"^(.*): #deps .* \((VALID|STALE)\)$", line)
        if match:
            current = resolve(match[1]) if match[2] == "VALID" else None
            if current is not None:
                records[current] = set()
        elif current is not None and line.startswith("    "):
            records[current].add(resolve(line[4:]))
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


def content_key(values: list[str], paths: set[Path], memo: dict | None = None) -> str:
    digest = hashlib.sha256()
    for value in values:
        digest.update(os.fsencode(value) + b"\0")
    for path in sorted(paths):
        digest.update(os.fsencode(path) + b"\0")
        if path.is_file():
            status = path.stat()
            identity = (path, status.st_ino, status.st_size, status.st_mtime_ns, status.st_ctime_ns)
            cached = memo.get(identity) if memo is not None else None
            if cached is None:
                contents = hashlib.sha256()
                with path.open("rb") as stream:
                    while chunk := stream.read(1024 * 1024):
                        contents.update(chunk)
                cached = contents.digest()
                if memo is not None:
                    memo[identity] = cached
            digest.update(cached)
        else:
            digest.update(b"MISSING")
        digest.update(b"\0")
    return digest.hexdigest()


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(data, stream, sort_keys=True)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def read_json(path: Path) -> dict:
    try:
        data = json.loads(path.read_text())
        return data if isinstance(data, dict) else {}
    except (FileNotFoundError, ValueError):
        return {}  # A missing/corrupt cache is a miss, never a success.


def configuration_paths(root: Path) -> set[Path]:
    names = decode_paths(git(root, "ls-files", "-z"))
    return {root / n for n in names if requires_full_analysis({n})} | {Path(__file__).resolve(), root / "CMakeUserPresets.json"}


def tool_implementation(root: Path, executables: set[Path]) -> set[Path]:
    paths = set(executables)
    for executable in executables:
        result = run(["ldd", str(executable)], root, capture=True, check=False)
        diagnostic = os.fsdecode(result.stdout + result.stderr)
        if result.returncode and not any(message in diagnostic for message in ("not a dynamic executable", "statically linked")):
            raise GateError("cannot fingerprint shared implementation of " + str(executable) + ": " + diagnostic)
        if "not found" in diagnostic:
            raise GateError("tool shared library missing: " + diagnostic)
        for line in diagnostic.splitlines():
            match = re.search(r"(/.*) \(0x[0-9a-f]+\)", line)
            if match:
                paths.add(Path(match[1]).resolve())
        # LLVM's resource headers are read by tidy but are absent from GCC's
        # dependency files. Include them and their inventory in tool identity.
        resources = executable.parent.parent / "lib/clang"
        if resources.is_dir():
            paths.update(p for p in resources.rglob("*") if p.is_file())
    return paths


def tool_state(root: Path, *, include_tidy: bool = True) -> str:
    paths = {Path(sys.executable).resolve()}
    versions = []
    names = ["clang-format", "cmake", "ninja", "ctest"]
    if include_tidy:
        names.append("clang-tidy")
    for name in names:
        tool = clang_tool(name) if name.startswith("clang-") else shutil.which(name)
        if not tool:
            raise GateError("missing tool: " + name)
        paths.add(Path(tool).resolve())
        versions.append(os.fsdecode(run([tool, "--version"], root, capture=True).stdout))
    # Compiler paths in the DB, not whichever c++ happens to be on PATH.
    for entry in json.loads((root / "build/cpu-tests/compile_commands.json").read_text()):
        args = entry.get("arguments") or shlex.split(entry["command"])
        executable = shutil.which(args[0])
        if not executable:
            raise GateError("missing compilation tool: " + args[0])
        paths.add(Path(executable).resolve())
    # GCC's implementation lives in cc1plus, beyond its small driver binary.
    for executable in list(paths):
        if re.search(r"(c\+\+|g\+\+)(-[0-9]+)?$", executable.name):
            result = run([str(executable), "-print-prog-name=cc1plus"], root, capture=True)
            implementation = Path(os.fsdecode(result.stdout).strip())
            if implementation.is_file():
                paths.add(implementation.resolve())
    paths = tool_implementation(root, paths)
    environment = {k: v for k, v in os.environ.items()
                   if not k.startswith(("GIT_", "POLYGRAPH_VERIFIED_")) and k not in {"_", "SHLVL", "PWD", "OLDPWD", "POLYGRAPH_QUALITY_PROFILE"}}
    return content_key(versions + [json.dumps(environment, sort_keys=True)], paths)


def analysis_state(root: Path, *, include_tidy: bool = True) -> str:
    build_dir = root / "build/cpu-tests"
    # File-name inventory detects newly added headers that can shadow an old
    # include. Contents are hashed per TU below, including external/system headers.
    inventory = []
    include_roots = {root / n for n in SOURCE_ROOTS} | {build_dir}
    for entry in json.loads((build_dir / "compile_commands.json").read_text()):
        arguments = entry.get("arguments") or shlex.split(entry.get("command", ""))
        for index, argument in enumerate(arguments):
            if argument in {"-I", "-isystem", "-iquote", "-idirafter"} and index + 1 < len(arguments):
                include_roots.add((Path(entry["directory"]) / arguments[index + 1]).resolve())
            elif argument.startswith("-I") and len(argument) > 2:
                include_roots.add((Path(entry["directory"]) / argument[2:]).resolve())
    for directory in include_roots:
        if directory.exists():
            inventory.extend(str(p) for p in directory.rglob("*") if p.suffix in EXTENSIONS and p.is_file())
    return content_key([tool_state(root, include_tidy=include_tidy), *sorted(inventory)], configuration_paths(root) | {
        build_dir / "CMakeCache.txt", build_dir / "compile_commands.json", build_dir / "build.ninja"})


def check_tidy(root: Path, names: set[str], *, full: bool = False) -> None:
    for name in sorted(names):
        if project_source(name) and Path(name).suffix in {".cu", ".cuh"}:
            print(f"[tidy] CUDA source requires a CUDA compilation database; CPU gate formats only: {name}", flush=True)
    units = selected_units(root, names, full)
    if not units:
        print("[tidy] No affected CPU translation units", flush=True)
        return
    tool = clang_tool("clang-tidy")
    dependencies = ninja_dependencies(root)
    outputs: dict[Path, set[Path]] = {}
    complete = {}
    for source, output in compile_units(root):
        outputs.setdefault(source, set()).update(dependencies.get(output, set()))
        complete[source] = complete.get(source, True) and output in dependencies
    cache_file = root / ".polygraph/tidy-cache.json"
    cache = read_json(cache_file)
    state = analysis_state(root)
    hits = 0
    memo = {}
    for index, source in enumerate(units, 1):
        key = content_key([state], outputs[source] | {source}, memo) if complete[source] else None
        relative = source.relative_to(root).as_posix()
        if not full and key is not None and cache.get(relative) == key:
            hits += 1
            continue
        print(f"[tidy] {index}/{len(units)} {relative}" + (" (uncacheable: missing Ninja dependencies)" if key is None else ""), flush=True)
        # Remove an older success before starting: interruption/failure cannot
        # leave a valid record for an unvalidated input.
        cache.pop(relative, None)
        write_json(cache_file, cache)
        run([tool, "-p", str(root / "build/cpu-tests"),
             "--config-file=" + str(root / "tools/dev/clang-tidy.yaml"),
             "--header-filter=^" + re.escape(str(root)) + r"/(include|src|app|tests)/",
             "--exclude-header-filter=" + re.escape(str(root / "include/utilities/json.hpp")) +
             r"|/(generated|third_party|third-party|vendor|external|_deps|build|data|datasets)/",
             str(source)], root)
        if key is not None:
            if content_key([state], outputs[source] | {source}) != key:
                raise GateError("analysis inputs changed during clang-tidy: " + relative)
            cache[relative] = key
            write_json(cache_file, cache)
    print(f"[tidy] {len(units)} selected; {hits} cache hits; {len(units) - hits} analyzed", flush=True)


def ensure_configured(root: Path) -> None:
    directory = root / "build/cpu-tests"
    stamp = root / ".polygraph/configure.json"
    key = content_key([], {root / "CMakePresets.json", root / "CMakeUserPresets.json"})
    cache = directory / "CMakeCache.txt"
    safe = cache.is_file() and all(re.search(pattern, cache.read_text()) for pattern in (
        r"POLYGRAPH_ENABLE_GPU:BOOL=OFF", r"POLYGRAPH_ENABLE_CLANG_FORMAT:BOOL=OFF",
        r"POLYGRAPH_ENABLE_CLANG_TIDY:BOOL=OFF", r"CMAKE_EXPORT_COMPILE_COMMANDS:[^=]+=ON",
        r"CMAKE_GENERATOR:INTERNAL=Ninja", r"POLYGRAPH_BUILD_TESTS:BOOL=ON"))
    if safe and re.search(r"^CMAKE_C(?:XX)?_CLANG_TIDY:[^=]+=.+$", cache.read_text(), re.MULTILINE):
        safe = False
    if not safe or not (directory / "build.ninja").is_file() or not (directory / "compile_commands.json").is_file() or read_json(stamp).get("key") != key:
        print("[configure] Initial/mismatched CPU configuration; configuring in place", flush=True)
        configure(root)
        write_json(stamp, {"key": key})
    else:
        print("[configure] Reusing CPU build; Ninja handles changed CMake inputs", flush=True)


def build(root: Path) -> None:
    run(["cmake", "--build", "--preset", "cpu-tests"], root)


SMOKE_TESTS = (
    "unit.DenseIdMapperTest.SortsAndDeduplicatesIdsBeforeAssigningDenseIndices",
    "unit.DenseIdMapperTest.RejectsMissingGlobalAndOutOfRangeDenseIds",
    "unit.CommandLineTest.ParsesAliasesFlagsAndDefaults",
    "unit.CommandLineTest.RejectsMissingRequiredOption",
    "unit.BfsExecutorTest.DirectionOptimizedSwitchesFromSparsePushToBitmapPull",
    "unit.BfsExecutorTest.ClassicPolicyNeverUsesPullExpansion",
    "contract.BfsPolicyContractTest.ForcedPullHonorsRelationshipAndDestinationFilters",
    "contract.BfsPolicyContractTest.ProvesPushPullPushForSparseAndBitmapMembership",
    "contract.Oracle/BfsContractTest.MatchesIndependentOracleAcrossExecutionModes/diamond_outgoing_all_unlimited",
    "contract.Oracle/BfsContractTest.MatchesIndependentOracleAcrossExecutionModes/cycle_both_all_unlimited",
)


def validate_smoke(tests: list[dict]) -> None:
    found = {test["name"]: test for test in tests}
    missing = set(SMOKE_TESTS) - found.keys()
    if missing:
        raise GateError("required smoke tests missing (selection is empty/incomplete): " + shlex.join(sorted(missing)))
    for name in SMOKE_TESTS:
        properties = {p["name"]: p["value"] for p in found[name].get("properties", [])}
        if properties.get("DISABLED"):
            raise GateError("required smoke test disabled: " + name)


def run_test_suite(root: Path, args: list[str], expected: set[str] | None = None) -> None:
    # CTest exits zero for runtime skips (including GTEST_SKIP). Mandatory gates
    # must verify executed results, not merely a nonempty discovery list.
    directory = root / ".polygraph"
    directory.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ctest-", dir=directory) as temporary:
        report = Path(temporary) / "results.xml"
        run([*args, "--output-junit", str(report)], root)
        if not report.is_file():
            raise GateError("CTest did not produce mandatory execution results")
        cases = ET.parse(report).getroot().findall(".//testcase")
        found = {case.get("name") for case in cases}
        if not cases or expected is not None and not expected <= found:
            raise GateError("CTest execution results missing expected tests")
        invalid = [case.get("name", "unnamed") for case in cases
                   if any(case.find(tag) is not None for tag in ("skipped", "failure", "error"))]
        if invalid:
            raise GateError("required tests skipped or failed at runtime: " + shlex.join(invalid))


def fast_tests(root: Path) -> None:
    selection = "^(" + "|".join(re.escape(n) for n in SMOKE_TESTS) + ")$"
    listing = run(["ctest", "--preset", "cpu-tests", "-R", selection, "--show-only=json-v1"], root, capture=True)
    validate_smoke(json.loads(listing.stdout).get("tests", []))
    print(f"[tests] {len(SMOKE_TESTS)} smoke tests; full unit/correctness/concurrency suite is explicit", flush=True)
    run_test_suite(root, ["ctest", "--preset", "cpu-tests", "-R", selection, "--no-tests=error",
         "--output-on-failure", "--timeout", "20", "--parallel", "2"], set(SMOKE_TESTS))


def full_tests(root: Path) -> None:
    for label in ("unit", "correctness", "concurrency"):
        listing = run(["ctest", "--preset", "cpu-tests", "-L", "^" + label + "$", "--show-only=json-v1"], root, capture=True)
        tests = json.loads(listing.stdout).get("tests", [])
        if not tests or any(t["name"].endswith("_NOT_BUILT") or any(p["name"] == "DISABLED" and p["value"] for p in t.get("properties", [])) for t in tests):
            raise GateError("full test selection missing, disabled or not built: " + label)
    run_test_suite(root, ["ctest", "--preset", "cpu-tests", "--no-tests=error", "--output-on-failure", "--parallel", "2"])
    run_test_suite(root, ["ctest", "--preset", "cpu-tests", "-L", "^concurrency$", "--no-tests=error",
         "--output-on-failure", "--parallel", "2", "--repeat", "until-fail:50"])


@contextmanager
def timed(title: str):
    start = time.monotonic()
    print("[quality] " + title, flush=True)
    try:
        yield
    finally:
        print(f"[time] {title}: {time.monotonic() - start:.2f}s", flush=True)


@contextmanager
def gate_lock(root: Path):
    directory = root / ".polygraph"
    directory.mkdir(exist_ok=True)
    with (directory / "quality.lock").open("w") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise GateError("another quality gate is using this CPU build; retry when it completes") from error
        yield


def sequence(root: Path, names: set[str], full: bool, expected: str | None = None, *, certify: bool = False) -> str | None:
    # The automatic fast path contains only format/configure/build/smoke.
    # Tidy is opt-in through the full profile or the standalone tidy command.
    format_names = changed_files(root, None) if full or any(Path(n).name in {".clang-format", "_clang-format", ".clang-format-ignore"} for n in names) else names
    certified_state = None
    stages = [
        ("clang-format", lambda: check_format(root, format_names)),
        ("CMake configure", lambda: ensure_configured(root)),
        ("CPU build", lambda: build(root)),
    ]
    if full:
        stages.append(("clang-tidy", lambda: check_tidy(root, names, full=True)))
    stages.append(("full tests" if full else "smoke tests", lambda: full_tests(root) if full else fast_tests(root)))
    for title, action in stages:
        if expected:
            verify_clean_revision(root, expected)
        with timed(title):
            action()
        if certify and title == "CPU build":
            certified_state = verification_state(root, include_tidy=full)
    if expected:
        verify_clean_revision(root, expected)
    if certify and verification_state(root, include_tidy=full) != certified_state:
        raise GateError("verification inputs/build outputs changed during verification; retry")
    print("[quality] All selected checks passed", flush=True)
    return certified_state


def sanitizers(root: Path) -> None:
    # No sanitizer gate existed in this checkout. Keep instrumentation in
    # separate preset directories so everyday CPU objects are never rebuilt.
    for preset in ("cpu-asan", "cpu-tsan"):
        for title, args in (
            ("configure", ["cmake", "--preset", preset, "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON"]),
            ("build", ["cmake", "--build", "--preset", preset]),
            ("tests", ["ctest", "--preset", preset, "--no-tests=error", "--output-on-failure", "--parallel", "2"]),
        ):
            with timed(preset + " " + title):
                if title == "tests":
                    run_test_suite(root, args)
                else:
                    run(args, root)


def verification_state(root: Path, *, include_tidy: bool = True) -> str:
    directory = root / "build/cpu-tests"
    dependencies = ninja_dependencies(root)
    units = compile_units(root)
    if any(output not in dependencies for _, output in units):
        raise GateError("cannot certify verification: incomplete Ninja dependency records")
    paths = {root / n for n in decode_paths(git(root, "ls-files", "-z"))}
    for output, deps in dependencies.items():
        paths.add(output)
        paths.update(deps)
    # Include linked libraries/executables, generated test discovery and fixtures.
    for path in directory.rglob("*"):
        if path.is_file() and (path.suffix in {".a", ".so", ".cmake", ".json"} or os.access(path, os.X_OK)):
            paths.add(path)
    return content_key([analysis_state(root, include_tidy=include_tidy)], paths)


def make_certificate(root: Path, tip: str, names: set[str], profile: str) -> dict:
    return {"tip": tip, "tree": git(root, "rev-parse", tip + "^{tree}").decode().strip(),
            "files": sorted(names), "profile": profile, "state": verification_state(root, include_tidy=profile == "full")}


def certificate_valid(root: Path, record: dict, tip: str, names: set[str], profile: str) -> bool:
    if (record.get("tip") != tip or record.get("profile") != profile or
            not names <= set(record.get("files", []))):
        return False
    return record == make_certificate(root, tip, set(record["files"]), profile)


def revision_checkout(root: Path, tip: str) -> Path:
    if git(root, "rev-parse", "HEAD").decode().strip() == tip:
        verify_clean_revision(root, tip)
        return root
    # Different tips get isolated persistent checkouts/builds; never reset the
    # developer's index/worktree or reuse objects built with another source root.
    target = root / ".polygraph/revisions" / tip
    if not (target / ".git").exists():
        target.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--shared", "--no-checkout", "--", str(root), str(target)], root)
        run(["git", "-c", "core.hooksPath=/dev/null", "checkout", "--detach", tip], target)
        with (target / ".git/info/exclude").open("a") as stream:
            stream.write("\n/.polygraph/\n")
    verify_clean_revision(target, tip)
    print("[scope] Isolated build for additional pushed tip " + tip[:12] + ": " + str(target), flush=True)
    return target


def verify_push(root: Path, protocol: str, remote: str, profile: str) -> None:
    scopes = push_scopes(root, protocol, remote)
    if not scopes:
        print("[quality] No non-deletion refs to verify", flush=True)
    certificate_dir = os.environ.get("POLYGRAPH_VERIFIED_DIRECTORY")
    for tip, names in scopes.items():
        checkout = revision_checkout(root, tip)
        with gate_lock(checkout):
            if certificate_dir:
                record = read_json(Path(certificate_dir) / (tip + ".json"))
                if not certificate_valid(checkout, record, tip, names, profile):
                    raise GateError("verified-push certificate is stale or remote scope changed; no in-connection rebuild. Refresh remote tracking refs and rerun verified-push")
                verify_clean_revision(checkout, tip)
                print("[quality] Exact revision/scope/toolchain/dependencies verified before connection: " + tip[:12], flush=True)
            else:
                sequence(checkout, names, profile == "full", tip)


def verified_push(root: Path, remote: str, refspecs: list[str], profile: str) -> None:
    hook_status(root)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", remote) or remote.startswith("-"):
        raise GateError("verified-push requires a configured remote name")
    git(root, "remote", "get-url", "--push", remote)  # Local lookup; no connection.
    if not refspecs:
        branch = git(root, "symbolic-ref", "--quiet", "HEAD").decode().strip()
        refspecs = [branch + ":" + branch]
    lines, exact = [], []
    for refspec in refspecs:
        if refspec.startswith(("-", "+")) or "*" in refspec or refspec.count(":") != 1:
            raise GateError("use explicit non-force source:refs/heads/destination refspecs")
        source, destination = refspec.split(":")
        if not source or not destination.startswith("refs/heads/"):
            raise GateError("verified-push supports branch updates; use normal git push for deletion/tag/force options")
        git(root, "check-ref-format", destination)
        tip = git(root, "rev-parse", "--verify", "--end-of-options", source + "^{commit}").decode().strip()
        tracking = "refs/remotes/" + remote + "/" + destination.removeprefix("refs/heads/")
        result = run(["git", "rev-parse", "--verify", tracking], root, capture=True, check=False)
        base = result.stdout.decode().strip() if result.returncode == 0 else "0" * len(tip)
        lines.append(f"{source} {tip} {destination} {base}\n")
        exact.append(tip + ":" + destination)
    scopes = push_scopes(root, "".join(lines), remote)
    (root / ".polygraph").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="verified-push-", dir=root / ".polygraph") as temporary:
        for tip, names in scopes.items():
            checkout = revision_checkout(root, tip)
            with gate_lock(checkout):
                state = sequence(checkout, names, profile == "full", tip, certify=True)
                record = make_certificate(checkout, tip, names, profile)
                if record["state"] != state:
                    raise GateError("verification state changed before certificate creation; retry")
                verify_clean_revision(checkout, tip)
                write_json(Path(temporary) / (tip + ".json"), record)
        # Hooks remain enabled; the hook checks the ACTUAL server advertisement
        # against our covered scope/state. A mismatch fails quickly and asks for
        # a new preflight; it never silently accepts a stale tracking range.
        env = {**os.environ, "POLYGRAPH_VERIFIED_DIRECTORY": temporary, "POLYGRAPH_QUALITY_PROFILE": profile}
        result = subprocess.run(["git", "-c", "core.hooksPath=.githooks", "push", "--", remote, *exact], cwd=root, env=env)
        if result.returncode:
            raise GateError(f"git push failed (exit {result.returncode}); see Git diagnostics")


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
    start = time.monotonic()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("fast", "full", "sanitizers", "verified-push", "format", "format-fix", "configure", "tidy", "build", "test", "verify", "pre-commit", "format-staged", "pre-push", "install", "hook-status"))
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--base", help="compare committed HEAD with this local commit/ref")
    scope.add_argument("--all", action="store_true", help="all tracked project sources")
    scope.add_argument("--files", nargs="+", help="explicit repository-relative paths")
    parser.add_argument("--profile", choices=("fast", "full"), default=os.environ.get("POLYGRAPH_QUALITY_PROFILE", "fast"))
    parser.add_argument("remote", nargs="?")
    parser.add_argument("refspecs", nargs="*")
    args = parser.parse_args()
    try:
        root = Path(os.fsdecode(git(Path.cwd(), "rev-parse", "--show-toplevel")).removesuffix("\n")).resolve()
        if args.profile not in {"fast", "full"}:
            raise GateError("invalid verification profile: " + args.profile)
        if args.command in {"pre-commit", "format-staged"}:
            format_staged(root)
            return 0
        if args.command == "pre-push":
            if args.profile not in {"fast", "full"}:
                raise GateError("invalid push profile")
            verify_push(root, sys.stdin.read(), args.remote or "origin", args.profile)
            return 0
        if args.command == "verified-push":
            if not args.remote:
                raise GateError("usage: quality.py verified-push [--profile full] REMOTE [source:refs/heads/destination ...]")
            verified_push(root, args.remote, args.refspecs, args.profile)
            return 0
        if args.command in {"install", "hook-status"}:
            {"install": install, "hook-status": hook_status}[args.command](root)
            return 0
        with gate_lock(root):
            if args.command in {"configure", "build", "test", "sanitizers"}:
                {"configure": configure, "build": build, "test": full_tests if args.profile == "full" else fast_tests, "sanitizers": sanitizers}[args.command](root)
                return 0
            if (args.command == "full" or args.profile == "full") and (args.files or args.base):
                raise GateError("full always verifies the entire CPU profile; --files/--base are not allowed")
            if args.command == "fast" and not (args.files or args.base or args.all):
                result = run(["git", "rev-parse", "--verify", "@{upstream}"], root, capture=True, check=False)
                if result.returncode:
                    print("[scope] No upstream; fallback to all tracked files (use --base for an explicit range)", flush=True)
                    names = changed_files(root, None)
                else:
                    names = revision_files(root, "HEAD", result.stdout.decode().strip())
                names.update(decode_paths(git(root, "diff", "HEAD", "--name-only", "--no-renames", "-z", "--")))
                names.update(decode_paths(git(root, "ls-files", "--others", "--exclude-standard", "-z")))
            else:
                names = set(args.files) if args.files is not None else changed_files(root, args.base)
            for name in names:
                if Path(name).is_absolute() or ".." in Path(name).parts:
                    raise GateError("file paths must stay within the repository: " + name)
            full = args.profile == "full" or args.command == "full" or args.command in {"verify", "tidy"} and args.files is None and args.base is None
            if args.command in {"format", "format-fix"}:
                check_format(root, names, fix=args.command == "format-fix")
            elif args.command == "tidy":
                ensure_configured(root)
                build(root)
                check_tidy(root, names, full=full)
            else:
                sequence(root, names, full)
        return 0
    except (GateError, OSError, ValueError, ET.ParseError, subprocess.SubprocessError) as error:
        print(f"[quality] FAILED: {error}", file=sys.stderr, flush=True)
        return 1
    finally:
        print(f"[time] total: {time.monotonic() - start:.2f}s", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
