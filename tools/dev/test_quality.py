"""Gate regression tests: no commits, staging, network, or actual pushes."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("quality", HERE / "quality.py")
quality = importlib.util.module_from_spec(spec)
spec.loader.exec_module(quality)


class ScopeTests(unittest.TestCase):
    def test_only_owned_sources(self):
        for name in ("src/a.cpp", "include/a.hpp", "app/path with spaces/main.cpp", "tests/a.cpp", "src/gpu/k.cu"):
            self.assertTrue(quality.project_source(name), name)
        for name in ("build/src/a.cpp", "data/a.hpp", "include/utilities/json.hpp", "src/generated/a.cpp",
                     "src/x.pb.cc", "src/vendor/a.hpp", "external/a.cpp", "src/build-debug/a.cpp"):
            self.assertFalse(quality.project_source(name), name)

    def test_nul_paths_preserve_spaces_and_newlines(self):
        self.assertEqual(quality.decode_paths(b"src/a b.cpp\0src/c\nd.cpp\0"), {"src/a b.cpp", "src/c\nd.cpp"})

    def test_config_changes_require_full_analysis(self):
        for name in (".clang-format", "src/.clang-format", "src/_clang-format", ".clang-format-ignore",
                     "src/.clang-format-ignore", "tools/dev/quality.py", "src/cmake/Core.cmake", "tests/CMakeLists.txt"):
            self.assertTrue(quality.requires_full_analysis({name}))
        self.assertFalse(quality.requires_full_analysis({"docs/CI.md", "src/a.cpp"}))

    def test_header_changes_use_dependency_graph_with_spaces(self):
        with tempfile.TemporaryDirectory(prefix="quality deps ") as temporary:
            root = Path(temporary)
            units = [(root / "src/a.cpp", root / "build/a.o"), (root / "src/b.cpp", root / "build/b.o")]
            deps = {units[0][1]: {root / "include/a b.hpp"}, units[1][1]: {root / "include/b.hpp"}}
            with patch.object(quality, "compile_units", return_value=units), patch.object(quality, "ninja_dependencies", return_value=deps):
                self.assertEqual(quality.selected_units(root, {"include/a b.hpp"}, False), [units[0][0]])
                self.assertEqual(quality.selected_units(root, {"include/new.hpp"}, False), [p for p, _ in units])
            with patch.object(quality, "compile_units", return_value=units), patch.object(quality, "ninja_dependencies", return_value={}):
                self.assertEqual(quality.selected_units(root, {"include/a b.hpp"}, False), [p for p, _ in units])

    def test_ninja_dependencies_parse_valid_records(self):
        root = Path("/tmp/project with spaces")
        output = b"src/a.o: #deps 2, deps mtime 1 (VALID)\n    /tmp/project with spaces/include/a b.hpp\n\nsrc/b.o: #deps 1, deps mtime 1 (STALE)\n    stale.hpp\n"
        result = subprocess.CompletedProcess([], 0, output, b"")
        with patch.object(quality, "run", return_value=result):
            self.assertEqual(quality.ninja_dependencies(root), {
                root / "build/cpu-tests/src/a.o": {root / "include/a b.hpp"}})

    def test_missing_tool_is_an_error(self):
        with patch.object(quality.shutil, "which", return_value=None):
            with self.assertRaises(quality.GateError):
                quality.clang_tool("clang-tidy")

    def test_revision_is_rechecked_between_stages(self):
        calls = []
        def revision(*args):
            calls.append("revision")
            if calls.count("revision") == 2:
                raise quality.GateError("HEAD changed")
        with patch.object(quality, "verify_clean_revision", side_effect=revision), \
             patch.object(quality, "check_format", side_effect=lambda *args: calls.append("format")), \
             patch.object(quality, "configure") as configure:
            with self.assertRaises(quality.GateError):
                quality.sequence(Path("/tmp/project"), set(), False, "a" * 40)
            configure.assert_not_called()
        self.assertEqual(calls, ["revision", "format", "revision"])

    def test_external_compilation_units_are_not_analyzed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build/cpu-tests"
            build.mkdir(parents=True)
            entries = [{"directory": str(build), "file": str(root / n), "output": n + ".o"}
                       for n in ("src/a.cpp", "build/_deps/external.cpp", "src/generated/a.cpp")]
            (build / "compile_commands.json").write_text(json.dumps(entries))
            self.assertEqual([p for p, _ in quality.compile_units(root)], [root / "src/a.cpp"])

    def test_new_cpu_source_missing_from_database_is_an_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "src").mkdir()
            (root / "src/unregistered.cpp").touch()
            with patch.object(quality, "compile_units", return_value=[(root / "src/a.cpp", root / "build/a.o")]):
                with self.assertRaises(quality.GateError):
                    quality.selected_units(root, {"src/unregistered.cpp"}, False)
                with self.assertRaises(quality.GateError):
                    quality.selected_units(root, {"src/unregistered.cpp", "CMakeLists.txt"}, True)

    def test_known_optional_feature_sources_are_reported_without_false_block(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in quality.OPTIONAL_CPU_FILES:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            with patch.object(quality, "compile_units", return_value=[(root / "src/a.cpp", root / "build/a.o")]):
                self.assertEqual(quality.selected_units(root, quality.OPTIONAL_CPU_FILES, False), [])

    def test_deleted_cpp_does_not_require_a_compilation_command(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(quality, "compile_units", return_value=[(root / "src/a.cpp", root / "build/a.o")]):
                self.assertEqual(quality.selected_units(root, {"src/removed.cpp"}, False), [])


# These executable fixtures intentionally replace tools, not production code.
# They exercise the real shell hook and driver orchestration, including stdin,
# exit status, paths with spaces, and stop-at-first-failure behavior.
FAKE_TOOL = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ['QUALITY_TEST_ROOT'])
kind = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
mode = os.environ.get('QUALITY_TEST_FAILURE', '')
if kind == 'git':
    if args[:2] == ['rev-parse', '--show-toplevel']: print(root)
    elif args[:2] == ['rev-parse', 'HEAD']: print('a' * 40)
    elif args[0] == 'rev-parse':
        if mode == 'missing-remote' and args[-1].startswith('b'): sys.exit(1)
        print(args[-1].split('^')[0])
    elif args[0] == 'status':
        if mode == 'dirty': sys.stdout.write(' M src/unit.cpp\0')
    elif args[0] in ('diff', 'ls-files'): sys.stdout.write('src/unit.cpp\0')
    sys.exit(0)
if '--version' in args:
    print('LLVM version 21.1.0')
    sys.exit(0)
stage = {'clang-format-21': 'format', 'clang-tidy-21': 'tidy', 'cmake': 'configure', 'ctest': 'test'}.get(kind, kind)
if kind == 'cmake' and '--build' in args: stage = 'build'
if mode == stage:
    print('injected ' + stage + ' failure', file=sys.stderr)
    sys.exit(7)
if kind == 'cmake' and '--preset' in args and '--build' not in args:
    build = root / 'build/cpu-tests'
    build.mkdir(parents=True, exist_ok=True)
    (build / 'compile_commands.json').write_text(json.dumps([{'directory': str(build), 'file': str(root / 'src/unit.cpp')}]))
if kind == 'ctest' and '--show-only=json-v1' in args:
    print(json.dumps({'tests': [] if mode == 'empty' else [{'name': 'unit.test'}]}))
'''


class HookTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="quality hook spaces ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("src", "tools/dev", ".githooks", "bin"):
            (self.root / name).mkdir(parents=True)
        shutil.copyfile(HERE / "quality.py", self.root / "tools/dev/quality.py")
        shutil.copyfile(HERE.parents[1] / ".githooks/pre-push", self.root / ".githooks/pre-push")
        (self.root / ".githooks/pre-push").chmod(0o755)
        (self.root / "src/unit.cpp").write_text("int value;\n")
        for name in ("git", "clang-format-21", "clang-tidy-21", "cmake", "ctest"):
            path = self.root / "bin" / name
            path.write_text(FAKE_TOOL)
            path.chmod(0o755)
        self.environment = {**os.environ, "QUALITY_TEST_ROOT": str(self.root),
                            "PATH": str(self.root / "bin") + os.pathsep + os.environ["PATH"]}
        self.environment.pop("CLANG_FORMAT", None)
        self.environment.pop("CLANG_TIDY", None)

    def invoke(self, failure="", local=None, remote=None):
        env = {**self.environment, "QUALITY_TEST_FAILURE": failure}
        local = local or 'a' * 40
        remote = remote or 'b' * 40
        protocol = f"refs/heads/main {local} refs/heads/main {remote}\n"
        return subprocess.run([str(self.root / ".githooks/pre-push"), 'origin', 'unused'],
                              input=protocol, capture_output=True, text=True, cwd=self.root, env=env, timeout=20)

    def test_every_failed_stage_blocks_push_and_stops(self):
        stages = ["clang-format", "CMake configure", "clang-tidy", "CPU build", "fast tests"]
        for index, failure in enumerate(("format", "configure", "tidy", "build", "test")):
            with self.subTest(failure=failure):
                result = self.invoke(failure)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("FAILED", result.stderr)
                for later in stages[index + 1:]:
                    self.assertNotIn("[quality] " + later, result.stdout)

    def test_success_runs_all_checks(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("All selected checks passed", result.stdout)

    def test_empty_test_selection_blocks_push(self):
        result = self.invoke("empty")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("selection is empty", result.stderr)

    def test_dirty_worktree_blocks_before_checks(self):
        result = self.invoke("dirty")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Commit or stash", result.stderr)
        self.assertNotIn("[quality] clang-format", result.stdout)

    def test_non_head_push_is_rejected(self):
        result = self.invoke(local='c' * 40)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not checked-out HEAD", result.stderr)

    def test_malformed_ref_input_is_rejected(self):
        result = self.invoke(local="not-an-object-id")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid Git pre-push", result.stderr)

    def test_deletion_only_push_needs_no_build(self):
        result = self.invoke(local='0' * 40)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("No non-deletion", result.stdout)

    def test_new_branch_and_missing_remote_fail_closed_to_full_scope(self):
        for failure, remote in (("", '0' * 40), ("missing-remote", 'b' * 40)):
            with self.subTest(failure=failure):
                result = self.invoke(failure, remote=remote)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("src/unit.cpp", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
