"""Enforce the oracle's dependency boundary, including in a fresh interpreter."""

from __future__ import annotations

import ast
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class IndependenceTests(unittest.TestCase):
    def test_all_oracle_imports_are_stdlib_or_reference_modules(self) -> None:
        package = Path(__file__).parent
        for path in sorted(package.glob("*.py")):
            with self.subTest(module=path.name):
                tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
                for node in ast.walk(tree):
                    if isinstance(node, ast.Import):
                        modules = [alias.name for alias in node.names]
                    elif isinstance(node, ast.ImportFrom) and not node.level:
                        modules = [node.module or ""]
                    else:
                        continue
                    for module in modules:
                        self.assertTrue(module.startswith("tests.reference.")
                                        or module.split(".")[0] in sys.stdlib_module_names,
                                        f"Forbidden oracle dependency in {path.name}: {module}")

    def test_pipeline_runs_with_production_imports_and_file_access_blocked(self) -> None:
        repository = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as temporary:
            raw = Path(temporary)
            (raw / "static").mkdir()
            (raw / "dynamic").mkdir()
            (raw / "dynamic/person_0_0.csv").write_text("id\n1\n2\n", encoding="utf-8")
            (raw / "dynamic/person_knows_person_0_0.csv").write_text(
                "Person.id|Person.id\n1|2\n", encoding="utf-8"
            )
            code = r'''
import importlib.abc
from pathlib import Path
import sys
root, raw = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
class BlockProduction(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname.split('.')[0] in {'tools', 'polygraph', 'src', 'include'}:
            raise ImportError('Forbidden production import: ' + fullname)
sys.meta_path.insert(0, BlockProduction())
def guard_files(event, args):
    if event == 'open' and isinstance(args[0], (str, bytes)):
        opened = Path(args[0]).resolve()
        for directory in ('tools', 'src', 'include', 'config'):
            if opened.is_relative_to(root / directory):
                raise RuntimeError('Forbidden production file read: ' + str(opened))
sys.addaudithook(guard_files)
sys.path.insert(0, str(root))
try:
    __import__('tools.graphio')
except ImportError:
    pass
else:
    raise AssertionError('Production import blocker was not active')
from tests.reference.bfs import bfs, project
from tests.reference.ldbc_csv import load_csv_graph
from tests.reference.compare import compare_results, read_result, write_result
from tests.reference.dataset import create_lock, verify_lock
from tests.reference.run import run_queries
graph = verify_lock(raw, create_lock(raw, require_complete=False))
projection = project(graph)
result = bfs(projection, ('Person', 2))
assert result.distance == {('Person', 2): 0, ('Person', 1): 1}
assert compare_results(result, result, projection, ('Person', 2)).same
print('isolated oracle passed')
'''
            completed = subprocess.run([sys.executable, "-I", "-c", code, str(repository), str(raw)],
                                       capture_output=True, text=True, timeout=20, check=False)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn("isolated oracle passed", completed.stdout)


if __name__ == "__main__":
    unittest.main()
