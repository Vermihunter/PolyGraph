"""Service-free checks for live-test decisions and data assertions, not live-import substitutes."""

import copy
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.ingestion.live_graph import (
    check_prerequisites,
    compare_records,
    normalize_neo_node,
    normalize_arango_node,
)
from tools.ingestion.docker_environment import WorkflowError


class LiveGraphAssertionsTest(unittest.TestCase):
    def test_mismatched_attributes_types_and_endpoints_are_detected(self):
        expected = [
            {
                "id": 1,
                "type": "Film",
                "source": "ArangoDB",
                "properties": {"title": "Orbit", "active": True},
                "from": 2,
                "to": 3,
            }
        ]
        compare_records(expected, copy.deepcopy(expected), "backend")
        for field, value in [
            ("type", "Person"),
            ("from", 99),
            ("to", 2),
            ("properties", {"title": "Orbit", "active": 1}),
        ]:
            actual = copy.deepcopy(expected)
            actual[0][field] = value
            with self.subTest(field=field), self.assertRaises(WorkflowError):
                compare_records(expected, actual, "backend")
        with self.assertRaises(WorkflowError):
            compare_records(expected, [], "backend")
        with self.assertRaises(WorkflowError):
            compare_records(expected, expected + expected, "backend")

    def test_native_zero_payload_and_proxy_ids_are_not_ldbc_ids(self):
        real = normalize_neo_node(
            ["Person", "POLYGRAPHVertex"],
            {
                "localId": 0,
                "type": "Person",
                "source": "Neo4J",
                "is_proxy": False,
                "active": True,
            },
        )
        self.assertEqual(real["id"], 1 << 48)
        self.assertEqual(real["properties"], {"active": True})
        proxy = normalize_neo_node(
            ["Proxy", "POLYGRAPHVertex"],
            {
                "uuid": -(1 << 63) + 7,
                "type": "Proxy",
                "source": "Neo4J",
                "is_proxy": True,
                "remote-type": "Studio",
                "remote-source": "PostgreSQL",
            },
        )
        self.assertEqual(proxy["id"], (1 << 63) + 7)
        self.assertEqual(proxy["type"], "Studio")
        self.assertTrue(proxy["is_proxy"])
        arango = normalize_arango_node(
            {
                "_id": "Film/562949953421317",
                "_key": "562949953421317",
                "_rev": "ignored",
                "uuid": 562949953421317,
                "type": "Film",
                "source": "ArangoDB",
                "rating": 8.5,
            }
        )
        self.assertEqual(arango["properties"], {"rating": 8.5})
        self.assertEqual(arango["type"], "Film")

    def test_missing_prerequisites_report_skip_reasons_without_docker_mutations(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "missing"
            calls = []

            def runner(argv, **kwargs):
                calls.append(argv)
                return subprocess.CompletedProcess(argv, 1, "", "unavailable")

            reasons, details = check_prerequisites(
                missing,
                missing,
                "movie-graph",
                {"neo": "neo", "arango": "arango", "pg": "pg"},
                runner=runner,
            )
            self.assertTrue(reasons)
            self.assertFalse(details.get("ready", False))
            self.assertTrue(all(argv[1] in ("info", "image") for argv in calls))

    def test_preexisting_resources_cannot_disappear_or_change(self):
        from tools.ingestion.live_graph import assert_preserved

        before = {
            "containers": {"existing": "fingerprint"},
            "volumes": {"data": "hash"},
        }
        assert_preserved(before, copy.deepcopy(before))
        for category in before:
            after = copy.deepcopy(before)
            after[category].clear()
            with self.subTest(category=category), self.assertRaises(WorkflowError):
                assert_preserved(before, after)

    def test_skip_is_reported_without_provisioning(self):
        from types import SimpleNamespace
        from unittest.mock import patch
        import json
        from tools.ingestion.live_graph import run_movie_fixture

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run"
            args = SimpleNamespace(
                output_dir=output,
                unique_output=False,
                binary=Path("missing"),
                bfs=Path("missing"),
                adapter="movie-graph",
                neo_image="neo",
                arango_image="arango",
                pg_image="pg",
            )
            with (
                patch(
                    "tools.ingestion.live_graph.check_prerequisites",
                    return_value=(["missing executable"], {"ready": False}),
                ),
                patch("tools.ingestion.live_graph.DockerEnvironment") as environment,
            ):
                self.assertEqual(run_movie_fixture(args), 77)
                environment.assert_not_called()
            report = json.loads((output / "integration-result.json").read_text())
            self.assertEqual(report["state"], "skipped")
            self.assertFalse(report["cleanup"]["attempted"])

    def test_cli_requires_explicit_opt_in(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run"
            result = subprocess.run(
                [
                    "python3",
                    "tools/ingestion/disposable_live.py",
                    "--adapter",
                    "movie-graph",
                    "--binary",
                    "missing",
                    "--bfs",
                    "missing",
                    "--output-dir",
                    str(output),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("--run is required", result.stderr)
            self.assertFalse(output.exists())

    def test_arango_endpoint_handles_are_transport_metadata(self):
        from tools.ingestion.live_graph import properties

        self.assertEqual(
            properties({"_from": "Person/1", "_to": "Film/2", "role": "Lead"}),
            {"role": "Lead"},
        )

    def test_resource_snapshot_ignores_mount_enumeration_order(self):
        import json
        from unittest.mock import patch
        from tools.ingestion.live_graph import resource_snapshot

        mounts = [{"Destination": "/a"}, {"Destination": "/b"}]
        row = {
            "Id": "existing",
            "Image": "image",
            "Mounts": mounts,
            "State": {"Running": False, "StartedAt": "old", "FinishedAt": "old"},
        }

        def read(args):
            if args[0] == "ps":
                return "existing"
            if args[0] == "inspect":
                return json.dumps([row])
            return ""

        with patch("tools.ingestion.live_graph._docker_read", side_effect=read):
            before = resource_snapshot()
            row["Mounts"] = list(reversed(mounts))
            self.assertEqual(before, resource_snapshot())

    def test_cancellation_reports_failure_and_cleans_owned_environment(self):
        import json
        from types import SimpleNamespace
        from unittest.mock import patch, MagicMock
        from tools.ingestion.live_graph import run_movie_fixture
        from tools.ingestion.docker_environment import WorkflowInterrupted

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run"
            args = SimpleNamespace(
                output_dir=output,
                unique_output=False,
                binary=Path("importer"),
                bfs=Path("bfs"),
                adapter="movie-graph",
                neo_image="neo",
                arango_image="arango",
                pg_image="pg",
                graph_manifest=None,
                oracle=None,
                workflow="native",
            )
            environment = MagicMock()
            environment.artifacts = output / "artifacts"
            environment.target_path = output / "targets.json"
            environment.runtime_config_path = output / "runtime.secret.json"
            environment.state = {"status": "removed", "containers": {}}
            import signal

            previous_handler = signal.getsignal(signal.SIGTERM)
            environment.cleanup.side_effect = lambda: signal.raise_signal(
                signal.SIGTERM
            )
            snapshot = {"containers": {}, "volumes": {}}
            with (
                patch(
                    "tools.ingestion.live_graph.check_prerequisites",
                    return_value=([], {"ready": True}),
                ),
                patch(
                    "tools.ingestion.live_graph.resource_snapshot",
                    return_value=snapshot,
                ),
                patch(
                    "tools.ingestion.live_graph.DockerEnvironment",
                    return_value=environment,
                ),
                patch(
                    "tools.ingestion.live_graph.stage",
                    side_effect=WorkflowInterrupted("cancelled"),
                ),
            ):
                self.assertEqual(run_movie_fixture(args), 1)
            environment.capture_logs.assert_called_once()
            environment.cleanup.assert_called_once()
            self.assertEqual(signal.getsignal(signal.SIGTERM), previous_handler)
            report = json.loads((output / "integration-result.json").read_text())
            self.assertEqual(report["state"], "failed")
            self.assertEqual(report["error"], "cancelled")
            self.assertTrue(report["cleanup"]["owned_containers_removed"])

    def test_first_signal_during_cleanup_is_shielded(self):
        import json
        from types import SimpleNamespace
        from unittest.mock import patch, MagicMock
        from tools.ingestion.live_graph import run_movie_fixture
        from tools.ingestion.docker_environment import WorkflowInterrupted

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run"
            args = SimpleNamespace(
                output_dir=output,
                unique_output=False,
                binary=Path("importer"),
                bfs=Path("bfs"),
                adapter="movie-graph",
                neo_image="neo",
                arango_image="arango",
                pg_image="pg",
                graph_manifest=None,
                oracle=None,
                workflow="native",
            )
            environment = MagicMock()
            environment.artifacts = output / "artifacts"
            environment.target_path = output / "targets.json"
            environment.runtime_config_path = output / "runtime.secret.json"
            environment.state = {"status": "removed", "containers": {}}
            import signal

            previous_handler = signal.getsignal(signal.SIGTERM)
            environment.cleanup.side_effect = lambda: signal.raise_signal(
                signal.SIGTERM
            )
            snapshot = {"containers": {}, "volumes": {}}
            with (
                patch(
                    "tools.ingestion.live_graph.check_prerequisites",
                    return_value=([], {"ready": True}),
                ),
                patch(
                    "tools.ingestion.live_graph.resource_snapshot",
                    return_value=snapshot,
                ),
                patch(
                    "tools.ingestion.live_graph.DockerEnvironment",
                    return_value=environment,
                ),
                patch(
                    "tools.ingestion.live_graph.stage",
                    side_effect=WorkflowError("cancelled"),
                ),
            ):
                self.assertEqual(run_movie_fixture(args), 1)
            environment.capture_logs.assert_called_once()
            environment.cleanup.assert_called_once()
            self.assertEqual(signal.getsignal(signal.SIGTERM), previous_handler)
            report = json.loads((output / "integration-result.json").read_text())
            self.assertEqual(report["state"], "failed")
            self.assertEqual(report["error"], "cancelled")
            self.assertTrue(report["cleanup"]["owned_containers_removed"])
