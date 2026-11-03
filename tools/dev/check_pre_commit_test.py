"""Real Git index + LLVM formatter regressions; never create a commit or push.

Fixtures reuse an existing commit through read-only alternates, and only stage
inside disposable repositories. The developer checkout's index is untouched.
"""
import os
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[1]
spec = importlib.util.spec_from_file_location("precommit_quality", HERE / "quality.py")
quality = importlib.util.module_from_spec(spec)
spec.loader.exec_module(quality)


class PreCommitTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pre commit paths with spaces ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        subprocess.run(["git", "init", "-q", str(self.root)], env=self.env, check=True)
        common = subprocess.check_output(["git", "rev-parse", "--git-common-dir"], cwd=REPOSITORY).decode().strip()
        objects = (REPOSITORY / common / "objects").resolve()
        (self.root / ".git/objects/info/alternates").write_text(str(objects) + "\n")
        head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPOSITORY).strip()
        self.git("update-ref", "HEAD", head.decode())  # Existing object only; no commit creation.
        self.git("read-tree", "HEAD")
        self.git("checkout-index", "-f", "--", ".clang-format")
        for name in ("tools/dev", ".githooks"):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(HERE / "quality.py", self.root / "tools/dev/quality.py")
        shutil.copyfile(REPOSITORY / ".githooks/pre-commit", self.root / ".githooks/pre-commit")
        (self.root / ".githooks/pre-commit").chmod(0o755)

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root, env=self.env, stderr=subprocess.STDOUT)

    def stage(self, name, content, mode=0o644):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(mode)
        self.git("add", "--", name)
        return path

    def blob(self, name):
        return self.git("show", ":" + name)

    def index_bytes(self):
        return (self.root / ".git/index").read_bytes()

    def hook(self):
        return subprocess.run([str(self.root / ".githooks/pre-commit")], cwd=self.root,
                              env=self.env, capture_output=True, text=True, timeout=30)

    def expected(self, path, content):
        return subprocess.check_output([os.environ.get("CLANG_FORMAT", "clang-format-21"), "--style=file", "--assume-filename=" + str(path)], input=content)

    def test_modified_tracked_cpp_is_formatted_and_staged(self):
        name = "src/cli/CommandLine.cpp"
        raw = b"int main(){return 0;}\n"
        path = self.stage(name, raw)
        result = self.hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(path.read_bytes(), self.expected(path, raw))
        self.assertNotEqual(path.read_bytes(), raw)
        self.assertEqual(self.blob(name), path.read_bytes())
        self.assertEqual(self.git("diff", "--name-only", "--", name), b"")

    def test_already_formatted_file_has_no_index_or_worktree_rewrite(self):
        name = "src/already.cpp"
        path = self.root / name
        content = self.expected(path, b"int f(){return 2;}\n")
        path = self.stage(name, content)
        before = (self.index_bytes(), path.stat().st_mtime_ns)
        result = self.hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.index_bytes(), path.stat().st_mtime_ns), before)

    def test_unrelated_unstaged_cpp_and_non_source_staged_file_are_untouched(self):
        raw = b"int f(){return 1;}\n"
        self.stage("src/staged.cpp", raw)
        unrelated = self.root / "src/not-staged.cpp"
        unrelated.write_bytes(raw)
        self.stage("docs/notes.txt", b"int untouched(){return 2;}\n")
        self.hook_ok()
        self.assertEqual(unrelated.read_bytes(), raw)
        self.assertEqual(self.blob("docs/notes.txt"), b"int untouched(){return 2;}\n")

    def hook_ok(self):
        result = self.hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_partial_staging_fails_before_changing_any_file_or_index(self):
        self.stage("src/first.cpp", b"int first(){return 1;}\n")
        path = self.stage("src/partial.cpp", b"int partial(){return 1;}\n")
        path.write_bytes(path.read_bytes() + b"// unstaged edit\n")
        before = (self.index_bytes(), path.read_bytes(), (self.root / "src/first.cpp").read_bytes())
        result = self.hook()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("partially staged", result.stderr)
        self.assertEqual((self.index_bytes(), path.read_bytes(), (self.root / "src/first.cpp").read_bytes()), before)

    def test_spaces_and_executable_mode_are_preserved(self):
        name = "src/space name.cpp"
        path = self.stage(name, b"int executable(){return 3;}\n", 0o755)
        self.hook_ok()
        self.assertEqual(path.stat().st_mode & 0o777, 0o755)
        self.assertTrue(self.git("ls-files", "--stage", "--", name).startswith(b"100755 "))
        self.assertEqual(self.blob(name), path.read_bytes())

    def test_generated_vendor_build_and_dataset_files_are_excluded(self):
        raw = b"int untouched(){return 1;}\n"
        names = ("src/generated/file.cpp", "src/vendor/file.hpp", "src/build-debug/file.cpp",
                 "data/file.cpp", "include/utilities/json.hpp", "src/messages.pb.cc")
        for name in names:
            self.stage(name, raw)
        before = self.index_bytes()
        self.hook_ok()
        self.assertEqual(self.index_bytes(), before)
        for name in names:
            self.assertEqual(self.blob(name), raw)
            self.assertEqual((self.root / name).read_bytes(), raw)

    def test_cuda_launch_syntax_is_formatted_without_compiling(self):
        name = "src/gpu/launch.cu"
        raw = b"__global__ void k(int*p){*p=1;} void go(int*p){k<<<1,32>>>(p);}\n"
        path = self.stage(name, raw)
        self.hook_ok()
        self.assertEqual(path.read_bytes(), self.expected(path, raw))
        self.assertEqual(self.blob(name), path.read_bytes())
        self.assertIn(b"<<<", path.read_bytes())

    def test_unstaged_formatter_configuration_is_rejected(self):
        self.stage("src/test.cpp", b"int f(){return 0;}\n")
        config = self.root / ".clang-format"
        config.write_bytes(config.read_bytes() + b"\n# unstaged policy\n")
        before = (self.index_bytes(), (self.root / "src/test.cpp").read_bytes())
        result = self.hook()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("configuration", result.stderr)
        self.assertEqual((self.index_bytes(), (self.root / "src/test.cpp").read_bytes()), before)

    def test_active_alternate_index_is_respected(self):
        alternate = self.root / ".git/alternate index"
        alternate.write_bytes(self.index_bytes())
        before = self.index_bytes()
        self.env["GIT_INDEX_FILE"] = str(alternate)
        path = self.stage("src/alternate.cpp", b"int f(){return 7;}\n")
        self.hook_ok()
        self.assertEqual(self.blob("src/alternate.cpp"), path.read_bytes())
        self.assertEqual(self.index_bytes(), before)

    def test_split_index_is_supported(self):
        self.git("update-index", "--split-index")
        path = self.stage("src/split.cpp", b"int f(){return 8;}\n")
        self.hook_ok()
        self.assertEqual(self.blob("src/split.cpp"), path.read_bytes())

    def test_staged_symlink_cannot_modify_its_target(self):
        target = self.root / "outside.txt"
        target.write_bytes(b"int target(){return 8;}\n")
        link = self.root / "src/link.cpp"
        link.parent.mkdir(parents=True)
        link.symlink_to(target)
        self.git("add", "--", "src/link.cpp")
        before = self.index_bytes()
        result = self.hook()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.index_bytes(), before)
        self.assertEqual(target.read_bytes(), b"int target(){return 8;}\n")

    def test_staged_format_configuration_is_used_and_retained(self):
        config = self.root / ".clang-format"
        self.stage(".clang-format", config.read_bytes().replace(b"IndentWidth: 4", b"IndentWidth: 2"))
        path = self.stage("src/test.cpp", b"int f(){return 1;}\n")
        before_config = self.blob(".clang-format")
        self.hook_ok()
        self.assertEqual(self.blob(".clang-format"), before_config)
        self.assertEqual(self.blob("src/test.cpp"), self.expected(path, b"int f(){return 1;}\n"))

    def test_formatter_failure_does_not_mutate_previously_prepared_files(self):
        first = self.stage("src/first.cpp", b"int first(){return 1;}\n")
        second = self.stage("src/second.cpp", b"int second(){return 2;}\n")
        fake = self.root / "fail-formatter"
        fake.write_text("#!/usr/bin/env python3\nimport sys\n"
                        "if '--version' in sys.argv: print('clang-format version 21.1.8'); sys.exit(0)\n"
                        "if '--list-ignored' in sys.argv: sys.exit(0)\n"
                        "if any('second.cpp' in a for a in sys.argv): sys.exit(7)\n"
                        "sys.stdout.buffer.write(sys.stdin.buffer.read() + b'// prepared only\\n')\n")
        fake.chmod(0o755)
        self.env["CLANG_FORMAT"] = str(fake)
        before = (self.index_bytes(), first.read_bytes(), second.read_bytes())
        result = self.hook()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.index_bytes(), first.read_bytes(), second.read_bytes()), before)

    def test_clang_format_ignore_never_truncates_or_stages_an_ignored_source(self):
        self.stage(".clang-format-ignore", b"src/ignored.cpp\n")
        raw = b"int ignored(){return 7;}\n"
        path = self.stage("src/ignored.cpp", raw)
        before = self.index_bytes()
        self.hook_ok()
        self.assertEqual(path.read_bytes(), raw)
        self.assertEqual(self.blob("src/ignored.cpp"), raw)
        self.assertEqual(self.index_bytes(), before)

    def test_configuration_appearing_during_formatting_blocks_without_mutation(self):
        path = self.stage("src/test.cpp", b"int f(){return 1;}\n")
        before = (self.index_bytes(), path.read_bytes())
        original_tool = quality.clang_tool
        def tool(kind):
            (self.root / "src/.clang-format").write_text("BasedOnStyle: LLVM\nIndentWidth: 12\n")
            return original_tool(kind)
        with patch.dict(os.environ, self.env, clear=True), patch.object(quality, "clang_tool", side_effect=tool):
            with self.assertRaisesRegex(quality.GateError, "configuration appeared"):
                quality.format_staged(self.root)
        self.assertEqual((self.index_bytes(), path.read_bytes()), before)

    def test_save_during_temporary_file_preparation_is_preserved(self):
        path = self.stage("src/test.cpp", b"int f(){return 1;}\n")
        before = self.index_bytes()
        original_temporary = quality.tempfile.mkstemp
        def temporary(*args, **kwargs):
            result = original_temporary(*args, **kwargs)
            if kwargs.get("prefix") == ".quality-format-":
                path.write_bytes(path.read_bytes() + b"// concurrent unstaged save\n")
            return result
        with patch.dict(os.environ, self.env, clear=True), patch.object(quality.tempfile, "mkstemp", side_effect=temporary):
            with self.assertRaisesRegex(quality.GateError, "working file changed"):
                quality.format_staged(self.root)
        self.assertEqual(self.index_bytes(), before)
        self.assertIn(b"// concurrent unstaged save", path.read_bytes())
        self.assertFalse((self.root / ".git/index.lock").exists())

    def test_unexpected_empty_formatter_output_fails_without_mutation(self):
        path = self.stage("src/test.cpp", b"int f(){return 1;}\n")
        fake = self.root / "empty-formatter"
        fake.write_text("#!/usr/bin/env python3\nimport sys\n"
                        "if '--version' in sys.argv: print('clang-format version 21.1.8')\n")
        fake.chmod(0o755)
        self.env["CLANG_FORMAT"] = str(fake)
        before = (self.index_bytes(), path.read_bytes())
        result = self.hook()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("empty source", result.stderr)
        self.assertEqual((self.index_bytes(), path.read_bytes()), before)

    def test_index_lock_prevents_mutation(self):
        path = self.stage("src/test.cpp", b"int f(){return 1;}\n")
        before = (self.index_bytes(), path.read_bytes())
        (self.root / ".git/index.lock").touch()
        result = self.hook()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.index_bytes(), path.read_bytes()), before)
        self.assertTrue((self.root / ".git/index.lock").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
