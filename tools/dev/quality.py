#!/usr/bin/env python3
"""Read-only CPU quality gates. Only format-fix and install intentionally mutate state."""
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


def check_format(root: Path, names: set[str], *, fix: bool = False) -> None:
    files = sorted(n for n in names if project_source(n) and (root / n).is_file())
    if not files:
        print("[format] No project source changes", flush=True)
        return
    for name in files:
        if not (root / name).resolve().is_relative_to(root):
            raise GateError("source symlink leaves repository: " + name)
    tool = clang_tool("clang-format")
    if any(Path(n).suffix in {".cu", ".cuh"} for n in files):
        # Exercise kernel-launch syntax with the selected version before allowing
        # it near CUDA files. No C++ compiler flags or CUDA SDK are assumed.
        probe = "__global__ void k(int *p){*p=1;} void launch(int*p){k<<<1,32>>>(p);}\n"
        parsed = subprocess.run([tool, "--style=file", "--assume-filename=" + str(root / "probe.cu")],
                                input=probe, capture_output=True, text=True, check=True, timeout=30)
        if not all(token in parsed.stdout for token in ("__global__", "<<<", ">>>")):
            raise GateError("selected clang-format did not preserve the CUDA kernel-launch probe")
        print("[format] LLVM 21 CUDA kernel-launch compatibility probe passed", flush=True)
    failures = []
    for name in files:
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
    if not os.access(root / ".githooks/pre-push", os.X_OK):
        raise GateError(".githooks/pre-push must be executable (chmod +x .githooks/pre-push)")
    run(["git", "config", "--local", "core.hooksPath", ".githooks"], root)
    hook_status(root)


def hook_status(root: Path) -> None:
    value = git(root, "config", "--get", "core.hooksPath").decode().strip()
    if value != ".githooks" or not os.access(root / ".githooks/pre-push", os.X_OK):
        raise GateError("pre-push hook is not active/executable; run tools/dev/install-hooks.sh")
    print("[quality] Active: core.hooksPath=.githooks; executable pre-push hook (terminal and CLion)", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("format", "format-fix", "configure", "tidy", "build", "test", "verify", "pre-push", "install", "hook-status"))
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--base", help="compare committed HEAD with this local commit/ref")
    scope.add_argument("--all", action="store_true", help="audit all tracked project sources (default)")
    scope.add_argument("--files", nargs="+", help="explicit repository-relative paths, useful while editing")
    args = parser.parse_args()
    try:
        root = Path(os.fsdecode(git(Path.cwd(), "rev-parse", "--show-toplevel")).removesuffix("\n")).resolve()
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
