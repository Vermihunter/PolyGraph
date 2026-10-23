from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.graphio.domain import load_domain_manifest, write_domain_manifest


class DomainManifestTests(unittest.TestCase):
    def test_manifest_carries_observability_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            schema = root / "schema.json"
            data = root / "graph.jsonl"
            backends = root / "backends.json"
            observability = root / "observability.json"
            for path in (schema, data, backends, observability):
                path.write_text("{}\n", encoding="utf-8")

            manifest = write_domain_manifest(
                root / "domain" / "domain.json",
                domain_id="test",
                name="Test",
                description="Test domain",
                schema=schema,
                data=data,
                backends=backends,
                observability=observability,
                graph_version=3,
                verbose=True,
                start_uuid="42",
            )
            raw = json.loads(manifest.read_text(encoding="utf-8"))
            loaded = load_domain_manifest(manifest)

        self.assertIn("observabilityConfig", raw["paths"])
        self.assertEqual(loaded.paths.observability, observability.resolve())
        self.assertEqual(loaded.graph_version, 3)
        self.assertTrue(loaded.verbose)
        self.assertEqual(loaded.start_uuid, "42")

    def test_legacy_manifest_uses_project_observability_default(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "domain.json"
            manifest.write_text(
                json.dumps(
                    {
                        "format": "polygraph.domain",
                        "version": 1,
                        "id": "legacy",
                        "paths": {
                            "schema": "schema.json",
                            "data": "graph.jsonl",
                            "backendConfig": "backends.json",
                        },
                        "defaults": {"bfs": {"startUuid": "7"}},
                    }
                ),
                encoding="utf-8",
            )
            loaded = load_domain_manifest(manifest)

        self.assertEqual(loaded.paths.observability.name, "observability.json")
        self.assertEqual(loaded.paths.observability.parent.name, "observability")


if __name__ == "__main__":
    unittest.main()
