from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.polygraph import (
    _compose_instance_key,
    _instance_connection_overrides,
    resolve_compose_instance,
)


class ComposeInstanceTests(unittest.TestCase):
    def test_instance_key_normalizes_scale_and_distribution(self) -> None:
        self.assertEqual(_compose_instance_key("0.1", "Schema Based"), "sf0_1/schema-based")
        self.assertEqual(_compose_instance_key("1", "schema-based"), "sf1_0/schema-based")
        self.assertEqual(_compose_instance_key("1.0", "schema-based"), "sf1_0/schema-based")
        self.assertEqual(_compose_instance_key("sf1", "schema-based"), "sf1_0/schema-based")
        self.assertEqual(_compose_instance_key("sf1_0", "hash-based"), "sf1_0/hash-based")

    def test_registry_merges_dotenv_defaults_and_instance_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compose = root / "compose.yml"
            compose.write_text("services: {}\n", encoding="utf-8")
            env = root / ".env"
            env.write_text(
                "NEO4J_HTTP_PORT=7474\n"
                "NEO4J_BOLT_PORT=7687\n"
                "NEO4J_USERNAME=neo4j\n"
                "NEO4J_PASSWORD=password\n"
                "ARANGO_PORT=8529\n"
                "ARANGO_ROOT_PASSWORD=rootpassword\n",
                encoding="utf-8",
            )
            registry = root / "environments.json"
            registry.write_text(
                json.dumps(
                    {
                        "defaults": {
                            "composeFile": str(compose),
                            "envFile": str(env),
                            "timeoutSeconds": 600,
                            "variables": {"NEO4J_HTTP_PORT": "17474"},
                        },
                        "instances": {
                            "sf0_1/schema-based": {
                                "domain": "data/domain.json",
                                "variables": {"ARANGO_PORT": "18529"},
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )

            instance = resolve_compose_instance(
                "sf0_1", "schema-based", registry_path=registry
            )

        self.assertEqual(instance.key, "sf0_1/schema-based")
        self.assertEqual(instance.project, "polygraph-sf0-1-schema-based")
        self.assertEqual(instance.neo4j_http_port, 17474)
        self.assertEqual(instance.arango_port, 18529)
        self.assertEqual(instance.timeout_seconds, 600)
        self.assertEqual(instance.domain, "data/domain.json")
        self.assertEqual(instance.variables["POLYGRAPH_SCALE_FACTOR"], "sf0_1")
        self.assertEqual(instance.variables["POLYGRAPH_DISTRIBUTION"], "schema-based")


    def test_unregistered_instance_is_synthesized_lazily(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compose = root / "compose.yml"
            compose.write_text("services: {}\n", encoding="utf-8")
            env = root / ".env"
            env.write_text(
                "NEO4J_HTTP_PORT=7474\n"
                "NEO4J_BOLT_PORT=7687\n"
                "NEO4J_USERNAME=neo4j\n"
                "NEO4J_PASSWORD=password\n"
                "ARANGO_PORT=8529\n"
                "ARANGO_ROOT_PASSWORD=rootpassword\n",
                encoding="utf-8",
            )
            registry = root / "environments.json"
            registry.write_text(json.dumps({
                "defaults": {"composeFile": str(compose), "envFile": str(env)},
                "instances": {},
            }), encoding="utf-8")
            instance = resolve_compose_instance("2", "schema-based", registry_path=registry)

        self.assertEqual(instance.key, "sf2_0/schema-based")
        self.assertEqual(instance.project, "polygraph-sf2-0-schema-based")
        self.assertEqual(instance.domain, "data/ldbc-snb/sf2_0/schema-based/domain.json")

    def test_instance_load_overrides_backend_ports_and_credentials(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            backends = root / "backends.json"
            backends.write_text(json.dumps({
                "version": 2,
                "idRouting": ["Neo4J", "ArangoDB"],
                "backends": [
                    {
                        "id": "Neo4J", "enabled": True,
                        "connection": {
                            "endpoint": "http://localhost:7474/db/neo4j/tx/commit",
                            "database": "neo4j",
                            "credentials": {"username": "old", "password": "old"},
                        },
                        "bolt_endpoint": "bolt://localhost:7687",
                    },
                    {
                        "id": "ArangoDB", "enabled": True,
                        "connection": {
                            "endpoint": "http://localhost:8529/_db/sf10/_api/cursor",
                            "database": "sf10",
                            "credentials": {"username": "root", "password": "old"},
                        },
                    },
                ],
            }), encoding="utf-8")
            compose = root / "compose.yml"
            compose.write_text("services: {}\n", encoding="utf-8")
            env = root / ".env"
            env.write_text(
                "NEO4J_HTTP_PORT=17474\n"
                "NEO4J_BOLT_PORT=17687\n"
                "NEO4J_USERNAME=neo4j\n"
                "NEO4J_PASSWORD=newpass\n"
                "ARANGO_PORT=18529\n"
                "ARANGO_ROOT_PASSWORD=arangopass\n",
                encoding="utf-8",
            )
            registry = root / "environments.json"
            registry.write_text(json.dumps({
                "defaults": {"composeFile": str(compose), "envFile": str(env)},
                "instances": {"sf10_0/schema-based": {}},
            }), encoding="utf-8")
            instance = resolve_compose_instance(
                "sf10_0", "schema-based", registry_path=registry
            )
            overrides = _instance_connection_overrides(instance, backends)

        self.assertEqual(overrides["Neo4J"].bolt_endpoint, "bolt://127.0.0.1:17687")
        self.assertIn(":17474/", overrides["Neo4J"].endpoint)
        self.assertEqual(overrides["Neo4J"].password, "newpass")
        self.assertIn(":18529/", overrides["ArangoDB"].endpoint)
        self.assertEqual(overrides["ArangoDB"].database, "sf10")
        self.assertEqual(overrides["ArangoDB"].password, "arangopass")



if __name__ == "__main__":
    unittest.main()
