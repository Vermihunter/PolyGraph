import json
from pathlib import Path
import tempfile
import unittest

from tools.graphio.workspace import (
    GraphWorkspace,
    claim_output,
    claim_index,
    claim_generated_files,
)


class WorkspaceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_stable_namespaced_paths_for_two_graphs(self):
        first = GraphWorkspace.from_manifest(self.root / "a.json", {"id": "first"})
        second = GraphWorkspace.from_manifest(self.root / "b.json", {"id": "second"})
        self.assertEqual(first.root, self.root / "artifacts/first")
        self.assertNotEqual(first.root, second.root)
        self.assertEqual(first.path("ingestion"), first.root / "ingestion")
        self.assertEqual(
            first, GraphWorkspace.from_manifest(self.root / "a.json", {"id": "first"})
        )
        explicit = GraphWorkspace.from_manifest(
            self.root / "a.json",
            {"id": "first", "workspace": {"id": "stable", "root": "generated"}},
        )
        self.assertEqual(explicit.root, self.root / "generated/stable")

    def test_rejects_unsafe_ids(self):
        for name in ("../escape", ".", "..", "/absolute", "a/b", "", "a b", None, 42):
            with self.subTest(name=name), self.assertRaises(ValueError):
                GraphWorkspace.from_manifest(self.root / "a.json", {"id": name})

    def test_output_collision_and_rerun_preserve_other_graph(self):
        source_a = self.root / "a.jsonl"
        source_a.write_text("source-a")
        source_b = self.root / "b.jsonl"
        source_b.write_text("source-b")
        a, b = self.root / "a", self.root / "b"
        claim_output(a, source_a)
        claim_output(b, source_b)
        (b / "keep.bin").write_bytes(b"other graph")
        before = {p.name: p.read_bytes() for p in b.iterdir()}
        claim_output(a, source_a)
        with self.assertRaises(ValueError):
            claim_output(a, source_b)
        with self.assertRaises(ValueError):
            claim_output(a / "nested", source_b)
        self.assertEqual(before, {p.name: p.read_bytes() for p in b.iterdir()})
        self.assertEqual(source_a.read_text(), "source-a")

    def test_index_collision_does_not_delete_existing_database(self):
        index = self.root / "planning.sqlite3"
        claim_index(index, self.root / "a.jsonl")
        index.write_bytes(b"existing database")
        with self.assertRaises(ValueError):
            claim_index(index, self.root / "b.jsonl")
        self.assertEqual(index.read_bytes(), b"existing database")
        claim_index(index, self.root / "a.jsonl")

    def test_never_adopts_unknown_outputs_or_overwrites_input(self):
        source = self.root / "graph.jsonl"
        source.write_text("original")
        with self.assertRaises(ValueError):
            claim_output(self.root, source)
        unknown = self.root / "unknown"
        unknown.mkdir()
        (unknown / "keep").write_text("keep")
        with self.assertRaises(ValueError):
            claim_output(unknown, source)
        alias = self.root / "alias"
        alias.symlink_to(unknown, target_is_directory=True)
        with self.assertRaises(ValueError):
            claim_output(alias, source)
        with self.assertRaises(ValueError):
            claim_index(source, source)
        self.assertEqual(source.read_text(), "original")

    def test_duplicate_workspace_id_rejects_different_config_across_artifact_kinds(
        self,
    ):
        first = GraphWorkspace.from_manifest(self.root / "a.json", {"id": "same"})
        second = GraphWorkspace.from_manifest(self.root / "b.json", {"id": "same"})
        first.claim(first.path("caches"))
        with self.assertRaisesRegex(ValueError, "collision"):
            second.claim(second.path("gpu-storage"))
        self.assertFalse(second.path("gpu-storage").exists())

    def test_rejects_malformed_workspace_settings(self):
        for settings in (
            None,
            [],
            {"root": ""},
            {"root": None},
            {"root": "bad\0path"},
            {"typo": "x"},
        ):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                GraphWorkspace.from_manifest(
                    self.root / "a.json", {"id": "graph", "workspace": settings}
                )

    def test_owned_rerun_rejects_symlink_files_and_subdirectories(self):
        source = self.root / "graph.jsonl"
        source.write_text("immutable")
        output = self.root / "out"
        claim_output(output, source)
        target = output / "cache-plan.json"
        target.symlink_to(source)
        with self.assertRaisesRegex(ValueError, "symlink"):
            claim_output(output, source)
        target.unlink()
        target.symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            claim_output(output, source)
        self.assertEqual(source.read_text(), "immutable")

    def test_low_level_planner_respects_manifest_ownership(self):
        source = self.root / "b.jsonl"
        source.write_text("immutable")
        workspace = GraphWorkspace.from_manifest(
            self.root / "b.json", {"id": "b", "paths": {"data": str(source)}}
        )
        output = workspace.path("cache")
        workspace.claim(output)
        claim_output(output, source)
        for path in (output, workspace.path("gpu-storage")):
            with (
                self.subTest(path=path),
                self.assertRaisesRegex(ValueError, "collision"),
            ):
                claim_output(path, self.root / "a.jsonl")

    def test_legacy_preparation_does_not_overwrite_other_sources(self):
        directory = self.root / "prepared"
        first = self.root / "raw-a"
        first.mkdir()
        second = self.root / "raw-b"
        second.mkdir()
        names = ("domain.json", "graph.jsonl")
        claim_generated_files(directory, first, names)
        (directory / "graph.jsonl").write_text("first graph")
        claim_generated_files(directory, first, names)
        with self.assertRaisesRegex(ValueError, "collision"):
            claim_generated_files(directory, second, names)
        self.assertEqual((directory / "graph.jsonl").read_text(), "first graph")
        with self.assertRaisesRegex(ValueError, "source"):
            claim_generated_files(first, first, names)
