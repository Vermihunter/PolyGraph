"""Regression checks for CI's nonempty-suite and exit-status guards (stdlib only)."""
import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("oracle_runner", Path(__file__).with_name("run_oracle_tests.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def run_suite(self, suite):
        with contextlib.redirect_stderr(io.StringIO()):
            return runner.run_suite(suite)

    def test_empty_suite_fails(self):
        self.assertEqual(self.run_suite(unittest.TestSuite()), 1)

    def test_failure_is_nonzero(self):
        def fail():
            raise AssertionError("intentional runner regression fixture")
        self.assertEqual(self.run_suite(unittest.TestSuite([unittest.FunctionTestCase(fail)])), 1)

    def test_skipped_suite_fails(self):
        def skip():
            raise unittest.SkipTest("intentional runner regression fixture")
        self.assertEqual(self.run_suite(unittest.TestSuite([unittest.FunctionTestCase(skip)])), 1)

    def test_missing_isolation_module_fails_even_when_other_tests_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = root / ".github/scripts/run_oracle_tests.py"
            script.parent.mkdir(parents=True)
            shutil.copyfile(Path(__file__).with_name("run_oracle_tests.py"), script)
            reference = root / "tests/reference"
            reference.mkdir(parents=True)
            (root / "tests/__init__.py").write_text("", encoding="utf-8")
            (reference / "__init__.py").write_text("", encoding="utf-8")
            (reference / "test_example.py").write_text(
                "import unittest\nclass Example(unittest.TestCase):\n"
                "    def test_passes(self): self.assertTrue(True)\n", encoding="utf-8")
            result = subprocess.run([sys.executable, "-I", "-S", str(script)],
                                    capture_output=True, text=True, timeout=10, check=False)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("isolation", result.stderr)

    def test_success_returns_zero(self):
        self.assertEqual(self.run_suite(unittest.TestSuite([unittest.FunctionTestCase(lambda: None)])), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
