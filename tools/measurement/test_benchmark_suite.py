from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.measurement.benchmark_suite import load_suite_config
from tools.measurement.graph_bench.graph_data import write_schema_with_source


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
                        "datasets": [
                            {"name": "schema-based", "domain": "schema/domain.json"},
                            {"name": "hash-based", "domain": "hash/domain.json"},
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
        self.assertEqual(config.scenarios.implementation_full, ("Neo4J", "ArangoDB", "GPU"))
        self.assertEqual(config.scenarios.native, ("Neo4J", "ArangoDB"))

    def test_full_backend_schema_uses_zero_routing_bits(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "schema.json"
            output = root / "gpu.json"
            source.write_text(
                json.dumps(
                    {
                        "uuidRouting": ["Neo4J", "ArangoDB", "GPU"],
                        "uuidRoutingBits": 2,
                        "nodeTypes": [{"name": "Person", "source": "Neo4J"}],
                        "relations": [{"name": "KNOWS", "source": "ArangoDB"}],
                    }
                ),
                encoding="utf-8",
            )
            write_schema_with_source(source, output, "GPU")
            generated = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(generated["uuidRouting"], ["GPU"])
        self.assertEqual(generated["storageBackends"], ["GPU"])
        self.assertEqual(generated["uuidRoutingBits"], 0)
        self.assertEqual(generated["nodeTypes"][0]["source"], "GPU")
        self.assertEqual(generated["relations"][0]["source"], "GPU")


if __name__ == "__main__":
    unittest.main()
