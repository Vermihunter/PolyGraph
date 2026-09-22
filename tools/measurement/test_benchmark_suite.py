from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.measurement.benchmark_suite import (
    _polygraph_event_log_root,
    _has_polygraph_event_logs,
    _resume_config_mismatches,
    load_suite_config,
)
from tools.graphio.model import NodeRecord, RelationshipRecord
from tools.measurement.graph_bench.config import BenchmarkConfig, BfsConfig, PathConfig
from tools.measurement.graph_bench.orchestrator import BfsBenchmarkOrchestrator
from tools.measurement.graph_bench.metrics import MeasurementSink, RunRecord
from tools.measurement.graph_bench.backends.native import GRAPH_NAME, Neo4jNativeClient
from tools.measurement.graph_bench.graph_data import GraphData, write_schema_with_source


class BenchmarkSuiteConfigTests(unittest.TestCase):
    def test_simple_config_loads_two_distributions_and_all_scenarios(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "suite.json"
            path.write_text(
                json.dumps(
                    {
                        "name": "test",
                        "repetitions": 2,
                        "startUuidCount": 5,
                        "build": {"enabled": False},
                        "services": {"start": False},
                        "bfs": {
                            "neighborBatchSize": 2500,
                            "maxConcurrentNeo4jRequests": 3,
                            "maxConcurrentArangoRequests": 5,
                            "backendRequestRetries": 4,
                            "backendRetryDelayMs": 75,
                        },
                        "datasets": [
                            {
                                "name": "schema-based",
                                "domain": "schema/domain.json",
                                "composeInstance": "sf0_1/schema-based",
                            },
                            {
                                "name": "hash-based",
                                "domain": "hash/domain.json",
                                "composeInstance": "sf0_1/hash-based",
                            },
                        ],
                        "scenarios": {
                            "distributed": True,
                            "implementationFull": ["neo4j", "arango", "gpu"],
                            "native": ["neo4j", "arango"],
                        },
                    }
                ),
                encoding="utf-8",
            )
            config = load_suite_config(path)

        self.assertEqual(config.repetitions, 2)
        self.assertEqual(config.start_uuid_count, 5)
        self.assertEqual([dataset.name for dataset in config.datasets], ["schema-based", "hash-based"])
        self.assertEqual(
            [dataset.compose_instance for dataset in config.datasets],
            ["sf0_1/schema-based", "sf0_1/hash-based"],
        )
        self.assertEqual(config.scenarios.implementation_full, ("Neo4J", "ArangoDB", "GPU"))
        self.assertEqual(config.scenarios.native, ("Neo4J", "ArangoDB"))
        self.assertTrue(config.scenarios.load_distributed)
        self.assertEqual(config.bfs.neighbor_batch_size, 2500)
        self.assertEqual(config.bfs.neo4j_max_concurrent_requests, 3)
        self.assertEqual(config.bfs.arango_max_concurrent_requests, 5)
        self.assertEqual(config.bfs.backend_request_retries, 4)
        self.assertEqual(config.bfs.backend_retry_delay_ms, 75)

    def test_compose_instance_must_be_defined_for_every_dataset(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "suite.json"
            path.write_text(
                json.dumps(
                    {
                        "datasets": [
                            {
                                "name": "schema",
                                "domain": "schema/domain.json",
                                "composeInstance": "sf0_1/schema-based",
                            },
                            {"name": "hash", "domain": "hash/domain.json"},
                        ]
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "every dataset"):
                load_suite_config(path)

    def test_neighbor_batch_size_must_be_positive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "suite.json"
            path.write_text(
                json.dumps(
                    {
                        "datasets": [{"name": "hash", "domain": "domain.json"}],
                        "bfs": {"neighborBatchSize": 0},
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "neighborBatchSize"):
                load_suite_config(path)

    def test_graph_db_command_forwards_neighbor_batch_size(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            orchestrator = BfsBenchmarkOrchestrator.__new__(BfsBenchmarkOrchestrator)
            orchestrator.config = BenchmarkConfig(
                paths=PathConfig(
                    schema=root / "schema.json",
                    data=root / "graph.jsonl",
                    backend_config=root / "backends.json",
                    graph_db_binary=root / "bfs",
                    loader_script=root / "load_graph.py",
                    reference_bfs_script=root / "reference_bfs.py",
                ),
                bfs=BfsConfig(
                    neighbor_batch_size=2500,
                    neo4j_max_concurrent_requests=3,
                    arango_max_concurrent_requests=5,
                    backend_request_retries=2,
                    backend_retry_delay_ms=125,
                ),
            )
            orchestrator.logs_dir = root / "logs"

            command = orchestrator._graph_db_command(
                scenario="distributed",
                schema=root / "schema.json",
                run_id="batch-test",
                repetition=1,
                data=root / "graph.jsonl",
                backend_config=root / "backends.json",
                start_uuid="42",
                system="graph_db_distributed",
            )

        rendered = [str(token) for token in command]
        option_index = rendered.index("--neighbor-batch-size")
        self.assertEqual(rendered[option_index + 1], "2500")
        self.assertEqual(
            rendered[rendered.index("--neo4j-max-concurrent-requests") + 1], "3"
        )
        self.assertEqual(
            rendered[rendered.index("--arango-max-concurrent-requests") + 1], "5"
        )
        self.assertEqual(
            rendered[rendered.index("--backend-request-retries") + 1], "2"
        )
        self.assertEqual(
            rendered[rendered.index("--backend-retry-delay-ms") + 1], "125"
        )

    def test_backend_concurrency_must_be_positive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "suite.json"
            path.write_text(
                json.dumps(
                    {
                        "datasets": [{"name": "hash", "domain": "domain.json"}],
                        "bfs": {"maxConcurrentArangoRequests": 0},
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "maxConcurrentArangoRequests"):
                load_suite_config(path)

    def test_event_log_discovery_ignores_command_and_native_logs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            dataset_output = Path(temporary)
            commands = dataset_output / "logs" / "commands.jsonl"
            commands.parent.mkdir(parents=True)
            commands.write_text('{"command": ["python3", "bfs.py"]}\n', encoding="utf-8")

            native_log = dataset_output / "logs" / "native" / "neo4j.jsonl"
            native_log.parent.mkdir(parents=True)
            native_log.write_text('{"seconds": 1.0}\n', encoding="utf-8")

            event_root = _polygraph_event_log_root(dataset_output)
            self.assertEqual(event_root, dataset_output / "logs" / "events")
            self.assertFalse(_has_polygraph_event_logs(event_root))

    def test_event_log_discovery_requires_non_empty_polygraph_event_log(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            event_root = _polygraph_event_log_root(Path(temporary))
            event_file = (
                event_root
                / "run"
                / "distributed"
                / "graph_db_distributed"
                / "42"
                / "rep001.events.jsonl"
            )
            event_file.parent.mkdir(parents=True)
            event_file.touch()
            self.assertFalse(_has_polygraph_event_logs(event_root))

            event_file.write_text(
                '{"event":"bfs","seq":1,"steady_ns":1,"thread_id":1}\n',
                encoding="utf-8",
            )
            self.assertTrue(_has_polygraph_event_logs(event_root))

    def test_existing_distributed_data_can_skip_loader(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "suite.json"
            path.write_text(
                json.dumps(
                    {
                        "name": "existing-hash",
                        "datasets": [
                            {"name": "hash-based", "domain": "hash/domain.json"}
                        ],
                        "scenarios": {
                            "distributed": True,
                            "implementationFull": [],
                            "native": [],
                            "loadDistributed": False,
                            "reloadEachRepetition": False,
                        },
                    }
                ),
                encoding="utf-8",
            )
            config = load_suite_config(path)

        self.assertTrue(config.scenarios.distributed)
        self.assertFalse(config.scenarios.load_distributed)
        self.assertFalse(config.scenarios.reload_each_repetition)
        self.assertEqual(config.scenarios.implementation_full, ())
        self.assertEqual(config.scenarios.native, ())

    def test_skip_loader_rejects_reload_each_repetition(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "suite.json"
            path.write_text(
                json.dumps(
                    {
                        "datasets": [{"name": "hash", "domain": "domain.json"}],
                        "scenarios": {
                            "loadDistributed": False,
                            "reloadEachRepetition": True,
                        },
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "loadDistributed"):
                load_suite_config(path)

    def test_full_backend_schema_uses_zero_routing_bits(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "schema.json"
            output = root / "gpu.json"
            source.write_text(
                json.dumps(
                    {
                        "idRouting": ["Neo4J", "ArangoDB", "GPU"],
                        "idRoutingBits": 2,
                        "nodeTypes": [{"name": "Person", "source": "Neo4J"}],
                        "relations": [{"name": "KNOWS", "source": "ArangoDB"}],
                    }
                ),
                encoding="utf-8",
            )
            write_schema_with_source(source, output, "GPU")
            generated = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(generated["idRouting"], ["GPU"])
        self.assertEqual(generated["storageBackends"], ["GPU"])
        self.assertEqual(generated["idRoutingBits"], 0)
        self.assertEqual(generated["nodeTypes"][0]["source"], "GPU")
        self.assertEqual(generated["relations"][0]["source"], "GPU")


class BenchmarkResumeTests(unittest.TestCase):
    def test_measurement_sink_replaces_failed_checkpoint_on_resume(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            failed = RunRecord(
                timestamp="2026-07-19T00:00:00+00:00",
                scenario="distributed",
                system="graph_db_distributed",
                phase="bfs_query",
                repetition=1,
                start_uuid="4658",
                seconds=1.0,
                status="failed",
            )
            sink = MeasurementSink(output)
            sink.add(failed)
            sink.flush_all()

            resumed = MeasurementSink(output, resume=True)
            self.assertFalse(
                resumed.successful(
                    scenario="distributed",
                    system="graph_db_distributed",
                    phase="bfs_query",
                    repetition=1,
                    start_uuid="4658",
                )
            )
            resumed.add(
                RunRecord(
                    timestamp="2026-07-19T00:01:00+00:00",
                    scenario="distributed",
                    system="graph_db_distributed",
                    phase="bfs_query",
                    repetition=1,
                    start_uuid="4658",
                    seconds=2.0,
                    status="ok",
                )
            )
            _, _, summary_path = resumed.flush_all()
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        self.assertEqual(summary["record_count"], 1)
        self.assertEqual(summary["failed_count"], 0)

    def test_resume_config_comparison_accepts_new_fields_but_rejects_changes(self) -> None:
        previous = {"bfs": {"neighbor_batch_size": 250}, "repetitions": 1}
        compatible = {
            "bfs": {
                "neighbor_batch_size": 250,
                "neo4j_max_concurrent_requests": 4,
            },
            "repetitions": 1,
        }
        changed = {"bfs": {"neighbor_batch_size": 500}, "repetitions": 1}

        self.assertEqual(_resume_config_mismatches(previous, compatible), [])
        self.assertTrue(_resume_config_mismatches(previous, changed))


class Neo4jGdsBfsTests(unittest.TestCase):

    def test_reset_deletes_relationships_before_nodes_without_detach_delete(self) -> None:
        client = Neo4jNativeClient("http://unused", "", "", batch_size=500)
        calls: list[tuple[list[dict], int]] = []
        relationship_results = iter([5, 0])
        node_results = iter([3, 0])

        def fake_tx(statements: list[dict], *, timeout: int = 900) -> dict:
            calls.append((statements, timeout))
            statement = statements[0]["statement"]
            if "gds.graph.drop" in statement:
                return {"results": [{"data": []}], "errors": []}
            if "WITH DISTINCT r" in statement:
                deleted = next(relationship_results)
            elif "DELETE n" in statement:
                deleted = next(node_results)
            else:
                raise AssertionError(f"Unexpected reset statement: {statement}")
            return {"results": [{"data": [{"row": [deleted]}]}], "errors": []}

        client.tx = fake_tx  # type: ignore[method-assign]
        try:
            client.reset()
        finally:
            client.close()

        statements = [call[0][0]["statement"] for call in calls]
        self.assertEqual(len(statements), 5)
        self.assertIn("gds.graph.drop", statements[0])
        self.assertTrue(all("DETACH DELETE" not in statement for statement in statements))
        self.assertIn("WITH DISTINCT r", statements[1])
        self.assertIn("WITH DISTINCT r", statements[2])
        self.assertIn("DELETE n", statements[3])
        self.assertIn("DELETE n", statements[4])
        self.assertEqual(calls[1][0][0]["parameters"]["limit"], 2_000)
        self.assertEqual(calls[3][0][0]["parameters"]["limit"], 5_000)

    def test_gds_projection_preserves_logical_relationship_types(self) -> None:
        client = Neo4jNativeClient("http://unused", "", "")
        calls: list[tuple[list[dict], int]] = []

        def fake_tx(statements: list[dict], *, timeout: int = 900) -> dict:
            calls.append((statements, timeout))
            return {"results": [{"data": [{"row": [GRAPH_NAME, 3, 2]}]}], "errors": []}

        client.tx = fake_tx  # type: ignore[method-assign]
        try:
            client._project_gds_graph(["LIKES", "KNOWS", "KNOWS"])
        finally:
            client.close()

        statement = calls[0][0][0]
        self.assertIn("$relationshipProjection", statement["statement"])
        self.assertEqual(
            statement["parameters"]["relationshipProjection"],
            {
                "KNOWS": {"type": "KNOWS", "orientation": "UNDIRECTED"},
                "LIKES": {"type": "LIKES", "orientation": "UNDIRECTED"},
            },
        )

    def test_gds_bfs_query_and_csv_output(self) -> None:
        graph = GraphData(
            nodes=[
                NodeRecord(10, "Person", "Neo4J", {}),
                NodeRecord(11, "Person", "Neo4J", {}),
                NodeRecord(12, "Person", "Neo4J", {}),
            ],
            relationships=[
                RelationshipRecord(20, "KNOWS", "Neo4J", 10, 11, {}),
                RelationshipRecord(21, "KNOWS", "Neo4J", 11, 12, {}),
            ],
        )
        client = Neo4jNativeClient("http://unused", "", "")
        client._graph = graph
        client._native_by_global = {10: 101, 11: 102, 12: 103}
        client._global_by_native = {101: 10, 102: 11, 103: 12}
        calls: list[tuple[list[dict], int]] = []

        def fake_tx(statements: list[dict], *, timeout: int = 900) -> dict:
            calls.append((statements, timeout))
            return {
                "results": [{
                    "columns": ["sourceNode", "nodeIds"],
                    "data": [{"row": [101, [101, 102, 103]]}],
                }],
                "errors": [],
            }

        client.tx = fake_tx  # type: ignore[method-assign]
        try:
            with tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "bfs.csv"
                count = client.bfs("10", "KNOWS", 2, output)
                csv_text = output.read_text(encoding="utf-8")
        finally:
            client.close()

        self.assertEqual(count, 3)
        self.assertEqual(csv_text, "vertex_id;distance\n10;0\n11;1\n12;2\n")
        self.assertEqual(len(calls), 1)
        statement = calls[0][0][0]
        self.assertIn("CALL gds.bfs.stream(", statement["statement"])
        self.assertIn(
            "sourceNode: toInteger(toString(config.sourceNode))",
            statement["statement"],
        )
        self.assertIn(
            "maxDepth: toInteger(toString(config.maxDepth))",
            statement["statement"],
        )
        self.assertEqual(statement["parameters"]["graphName"], GRAPH_NAME)
        self.assertEqual(
            statement["parameters"]["configuration"],
            {
                "sourceNode": 101,
                "maxDepth": 2,
                "logProgress": False,
                "relationshipTypes": ["KNOWS"],
            },
        )


if __name__ == "__main__":
    unittest.main()
