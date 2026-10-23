from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.workflows.checkpoint import CheckpointMismatchError, WorkflowCheckpoint
from tools.workflows.common import resumable_storage_load


class WorkflowCheckpointTests(unittest.TestCase):
    def test_completed_stage_survives_reopen(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "run.json"
            first = WorkflowCheckpoint.open(
                path,
                workflow="test",
                identity={"sf": "sf1_0"},
            )
            first.mark_done("prepare", rows=123)

            second = WorkflowCheckpoint.open(
                path,
                workflow="test",
                identity={"sf": "sf1_0"},
            )

            self.assertTrue(second.is_done("prepare"))
            self.assertEqual(second.stage("prepare")["details"]["rows"], 123)

    def test_identity_mismatch_requires_restart(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "run.json"
            WorkflowCheckpoint.open(path, workflow="test", identity={"sf": "sf1_0"})

            with self.assertRaises(CheckpointMismatchError):
                WorkflowCheckpoint.open(path, workflow="test", identity={"sf": "sf10_0"})

            restarted = WorkflowCheckpoint.open(
                path,
                workflow="test",
                identity={"sf": "sf10_0"},
                restart=True,
            )
            self.assertEqual(restarted.completed_stages(), [])

    def test_checkpoint_write_is_valid_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "run.json"
            checkpoint = WorkflowCheckpoint.open(path, workflow="test", identity={"x": 1})
            checkpoint.mark_failed("load", RuntimeError("boom"))
            payload = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(payload["stages"]["load"]["status"], "failed")
            self.assertIn("RuntimeError: boom", payload["stages"]["load"]["error"])


class ResumableStorageLoadTests(unittest.TestCase):
    def test_failed_late_backend_resumes_without_reloading_completed_backends(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "domain.json"
            graph = root / "graph.jsonl"
            manifest.write_text("{}\n", encoding="utf-8")
            graph.write_text('{"kind":"meta"}\n', encoding="utf-8")
            checkpoint_path = root / "storage-load.json"

            failure = subprocess.CalledProcessError(1, ["polygraph", "load"])
            with (
                patch(
                    "tools.workflows.common.storage_load_checkpoint_path",
                    return_value=checkpoint_path,
                ),
                patch("tools.workflows.common.write_storage_marker"),
                patch("tools.workflows.common.polygraph", side_effect=[None, None, failure]) as load,
            ):
                with self.assertRaises(subprocess.CalledProcessError):
                    resumable_storage_load(
                        "1",
                        "schema-based",
                        project="polygraph-sf1-0-schema-based",
                        manifest=manifest,
                        postgres_indexes=False,
                        batch_size=20_000,
                        workers=4,
                    )
                self.assertEqual(load.call_count, 3)

            state = json.loads(checkpoint_path.read_text(encoding="utf-8"))
            self.assertEqual(state["stages"]["backend:Neo4J"]["status"], "done")
            self.assertEqual(state["stages"]["backend:ArangoDB"]["status"], "done")
            self.assertEqual(state["stages"]["backend:PostgreSQL"]["status"], "failed")

            with (
                patch(
                    "tools.workflows.common.storage_load_checkpoint_path",
                    return_value=checkpoint_path,
                ),
                patch("tools.workflows.common.write_storage_marker") as marker,
                patch("tools.workflows.common.polygraph") as load,
            ):
                resumable_storage_load(
                    "1",
                    "schema-based",
                    project="polygraph-sf1-0-schema-based",
                    manifest=manifest,
                    postgres_indexes=False,
                    batch_size=20_000,
                    workers=4,
                )

            self.assertEqual(load.call_count, 1)
            command = load.call_args.args
            self.assertIn("PostgreSQL", command)
            self.assertNotIn("Neo4J", command)
            self.assertNotIn("ArangoDB", command)
            marker.assert_called_once()


if __name__ == "__main__":
    unittest.main()
