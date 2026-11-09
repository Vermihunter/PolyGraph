"""Reusable adapter assertions; source-format details live in fixture declarations."""

from __future__ import annotations

from collections import Counter, defaultdict
from contextlib import closing
import copy
from dataclasses import dataclass
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile

from tools.graphio.id_codec import BackendLayout, canonical_global_id

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/adapter_contract"


def uint64(value):
    return int(value) & ((1 << 64) - 1)


def source_key(selector, kind):
    return json.dumps(
        [
            "polygraph.source.v1",
            selector["namespace"],
            kind,
            selector["type"],
            selector["idType"],
            selector["id"],
        ],
        ensure_ascii=False,
        separators=(",", ":"),
    )


@dataclass
class Snapshot:
    manifest: dict
    nodes: list[dict]
    edges: list[dict]

    @classmethod
    def read(cls, output):
        manifest = json.loads((output / "manifest.json").read_text())
        index = output / manifest["index"]["path"]
        with closing(sqlite3.connect(index.as_uri() + "?mode=ro", uri=True)) as db:
            nodes = [
                dict(
                    id=uint64(id),
                    key=key,
                    type=kind,
                    owner=owner,
                    properties=json.loads(props),
                )
                for id, key, kind, owner, props in db.execute(
                    "SELECT id,key,type,owner,properties FROM nodes"
                )
            ]
            tables = {
                row[0]
                for row in db.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                )
            }
            keys = (
                {
                    uint64(id): key
                    for key, id in db.execute("SELECT key,id FROM source_edges")
                }
                if "source_edges" in tables
                else {}
            )
            edges = [
                dict(
                    id=uint64(id),
                    key=keys.get(uint64(id)),
                    type=kind,
                    owner=owner,
                    source=uint64(source),
                    target=uint64(target),
                    properties=json.loads(props),
                )
                for id, kind, owner, source, target, props in db.execute(
                    "SELECT id,type,owner,from_id,to_id,properties FROM edges"
                )
            ]
        return cls(
            manifest,
            sorted(nodes, key=lambda row: row["id"]),
            sorted(edges, key=lambda row: row["id"]),
        )


class AdapterContractMixin:
    """Combine with unittest.TestCase and set `case` to a declarative fixture."""

    def setUp(self):
        executable = os.environ.get(self.case["binary_env"])
        if not executable:
            self.skipTest(
                "Requires "
                + self.case["binary_env"]
                + "; CTest supplies built importers"
            )
        self.binary = Path(executable).resolve()
        self.temp = tempfile.TemporaryDirectory(prefix="polygraph-adapter-contract-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.input_root = self.root / "input"
        source = ROOT / self.case["input"]
        if source.is_dir():
            shutil.copytree(source, self.input_root)
            self.input = self.input_root
        else:
            self.input_root.mkdir()
            self.input = self.input_root / source.name
            shutil.copyfile(source, self.input)
        self.placement = self.root / "placement.json"
        shutil.copyfile(ROOT / self.case["placement"], self.placement)
        self.expected = json.loads((FIXTURES / self.case["expected"]).read_text())
        node_refs = [row["ref"] for row in self.expected["nodes"]]
        edge_refs = [row["ref"] for row in self.expected["edges"]]
        self.assertEqual(
            len(node_refs), len(set(node_refs)), "Oracle node aliases must be unique"
        )
        self.assertEqual(
            len(edge_refs), len(set(edge_refs)), "Oracle edge aliases must be unique"
        )
        for row in self.expected["nodes"] + self.expected["edges"]:
            self.assertIn(
                set(row["identity"]),
                ({"uuid"}, {"source"}),
                "Oracle needs exactly one identity selector",
            )
        for traversal in self.expected["traversals"]:
            self.assertIn(traversal["direction"], ("directed", "undirected"))
            self.assertTrue(traversal["layers"], "Oracle needs an explicit root layer")
            self.assertEqual(traversal["layers"][0], [traversal["root"]])
            reached = [ref for layer in traversal["layers"] for ref in layer]
            self.assertEqual(
                len(reached),
                len(set(reached)),
                "Oracle layers must contain newly reached nodes",
            )
            self.assertTrue(
                set(reached).issubset(node_refs),
                "Oracle references an unknown node alias",
            )

    def source_snapshot(self):
        return {
            str(path.relative_to(self.root)): (
                path.read_bytes(),
                path.stat().st_mtime_ns,
            )
            for path in [self.placement, *self.input_root.rglob("*")]
            if path.is_file()
        }

    def prepare(self, name="prepared", *, adapter=None, threads=1):
        before = self.source_snapshot()
        output = self.root / name
        result = subprocess.run(
            [
                str(self.binary),
                "prepare",
                "--adapter",
                adapter or self.case["adapter"],
                "--input",
                str(self.input),
                "--placement-config",
                str(self.placement),
                "--output-dir",
                str(output),
                "--threads",
                str(threads),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(
            self.source_snapshot(), before, "An adapter changed its source/config files"
        )
        return output, result

    def prepared(self, name="prepared", *, threads=1):
        output, result = self.prepare(name, threads=threads)
        self.assertEqual(result.returncode, 0, result.stderr)
        return output, Snapshot.read(output)

    def assert_typed_equal(self, actual, expected, context):
        self.assertIs(type(actual), type(expected), context)
        if isinstance(expected, dict):
            self.assertEqual(set(actual), set(expected), context)
            for key, value in expected.items():
                self.assert_typed_equal(actual[key], value, context + "." + key)
        elif isinstance(expected, list):
            self.assertEqual(len(actual), len(expected), context)
            for index, value in enumerate(expected):
                self.assert_typed_equal(actual[index], value, context + f"[{index}]")
        else:
            self.assertEqual(actual, expected, context)

    def resolve(self, expected, rows, kind):
        identity = expected["identity"]
        if "uuid" in identity:
            matches = [row for row in rows if row["id"] == int(identity["uuid"])]
        else:
            selector = identity["source"]
            self.assertEqual(
                selector["type"], expected["type"], "Invalid oracle source type"
            )
            key = source_key(selector, kind)
            matches = [row for row in rows if row["key"] == key]
        self.assertEqual(
            len(matches), 1, f"Missing/ambiguous identity: {expected['ref']}"
        )
        return matches[0]

    def assert_nodes(self, snapshot):
        self.assertEqual(len(snapshot.nodes), len(self.expected["nodes"]))
        self.assertEqual(snapshot.manifest["counts"]["nodes"], len(snapshot.nodes))
        ids = [row["id"] for row in snapshot.nodes + snapshot.edges]
        self.assertEqual(
            len(ids), len(set(ids)), "Node/edge GlobalIds must be globally unique"
        )
        self.assertEqual(
            len({row["key"] for row in snapshot.nodes}), len(snapshot.nodes)
        )
        layout = BackendLayout(
            tuple(json.loads(self.placement.read_text())["storageBackends"])
        )
        for row in snapshot.nodes + snapshot.edges:
            self.assertEqual(canonical_global_id(row["id"]), row["id"])
            self.assertEqual(layout.decode_source(row["id"]), row["owner"])
        aliases = {}
        for expected in self.expected["nodes"]:
            actual = self.resolve(expected, snapshot.nodes, "vertex")
            self.assertEqual(actual["type"], expected["type"])
            self.assertEqual(actual["owner"], expected["owner"])
            self.assert_typed_equal(
                actual["properties"], expected["properties"], expected["ref"]
            )
            aliases[actual["id"]] = expected["ref"]
        self.assertEqual(len(aliases), len(snapshot.nodes))
        return aliases

    def assert_graph(self, snapshot):
        self.assertEqual(snapshot.manifest["source"]["adapter"], self.case["adapter"])
        self.assertEqual(snapshot.manifest["id_strategy"], self.case["id_strategy"])
        aliases = self.assert_nodes(snapshot)
        self.assertEqual(len(snapshot.edges), len(self.expected["edges"]))
        self.assertEqual(snapshot.manifest["counts"]["edges"], len(snapshot.edges))
        resolved = set()
        for expected in self.expected["edges"]:
            actual = self.resolve(expected, snapshot.edges, "edge")
            resolved.add(actual["id"])
            self.assertIn(actual["source"], aliases, "Missing source endpoint")
            self.assertIn(actual["target"], aliases, "Missing target endpoint")
            self.assertEqual(actual["type"], expected["type"])
            self.assertEqual(actual["owner"], expected["owner"])
            self.assertEqual(aliases[actual["source"]], expected["from"])
            self.assertEqual(aliases[actual["target"]], expected["to"])
            self.assert_typed_equal(
                actual["properties"], expected["properties"], expected["ref"]
            )
        self.assertEqual(len(resolved), len(snapshot.edges))
        return aliases

    def mutate(self, name):
        recipe = self.case["mutations"][name]
        path = self.input_root / recipe["file"]
        self.assertTrue(path.resolve().is_relative_to(self.input_root.resolve()))
        data = path.read_text()
        if "append_line" in recipe:
            line = data.splitlines()[recipe["append_line"]]
            data = data.rstrip("\r\n") + "\n" + line + "\n"
        elif "append" in recipe:
            data += recipe["append"]
        elif "replace" in recipe:
            old, new = recipe["replace"]
            self.assertEqual(
                data.count(old), 1, "Mutation must identify exactly one fixture value"
            )
            data = data.replace(old, new, 1)
        else:
            lines = data.splitlines()
            index = recipe["json_line"]
            value = json.loads(lines[index])
            value.update(recipe["patch"])
            lines[index] = json.dumps(value, ensure_ascii=False)
            data = "\n".join(lines) + "\n"
        path.write_text(data)
        return recipe

    def assert_rejected(self, name):
        recipe = self.mutate(name)
        output, result = self.prepare()
        self.assertNotEqual(result.returncode, 0, "Invalid input was silently accepted")
        self.assertIn(recipe["error"], result.stderr)
        self.assertFalse(
            (output / "manifest.json").exists(),
            "Failed prepare published a success manifest",
        )

    def verify_strict(self, output):
        result = subprocess.run(
            [
                str(self.binary),
                "verify",
                "--manifest",
                str(output / "manifest.json"),
                "--strict",
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_records_ids_properties_endpoints_and_strict_artifacts(self):
        output, snapshot = self.prepared()
        self.assert_graph(snapshot)
        self.verify_strict(output)
        for backend in json.loads(self.placement.read_text())["storageBackends"]:
            self.assertIn(backend, snapshot.manifest["backends"])

    def test_record_orientation_and_declared_reachability(self):
        _, snapshot = self.prepared()
        aliases = self.assert_graph(snapshot)
        for traversal in self.expected["traversals"]:
            with self.subTest(traversal=traversal):
                adjacency = defaultdict(set)
                for edge in snapshot.edges:
                    source, target = aliases[edge["source"]], aliases[edge["target"]]
                    adjacency[source].add(target)
                    if traversal["direction"] == "undirected":
                        adjacency[target].add(source)
                seen, frontier = set(), {traversal["root"]}
                for expected in traversal["layers"]:
                    self.assertEqual(frontier, set(expected))
                    seen.update(frontier)
                    frontier = {
                        target for source in frontier for target in adjacency[source]
                    } - seen

    def test_deterministic_fresh_output_and_idempotent_rerun(self):
        first_output, first = self.prepared("first", threads=1)
        second_output, second = self.prepared("second", threads=3)
        self.verify_strict(second_output)
        self.assert_graph(first)
        self.assert_graph(second)
        self.assertEqual(first.nodes, second.nodes)
        self.assertEqual(first.edges, second.edges)
        self.assertEqual(
            first.manifest["identity_sha256"], second.manifest["identity_sha256"]
        )
        self.assertEqual(first.manifest["index"], second.manifest["index"])
        for backend in first.manifest["backends"]:
            self.assertEqual(
                first.manifest["backends"][backend]["artifacts"],
                second.manifest["backends"][backend]["artifacts"],
            )
        original = (first_output / "manifest.json").read_bytes()
        _, repeated = self.prepared("first", threads=3)
        self.assertEqual(repeated.nodes, first.nodes)
        self.assertEqual(repeated.edges, first.edges)
        self.assert_graph(repeated)
        self.assertEqual((first_output / "manifest.json").read_bytes(), original)
        self.verify_strict(first_output)

    def test_duplicate_vertex_identity_is_rejected(self):
        self.assert_rejected("duplicate_node")

    def test_duplicate_relationship_policy_is_explicit(self):
        recipe = self.case["mutations"]["duplicate_edge"]
        if recipe["policy"] == "reject":
            self.assert_rejected("duplicate_edge")
            return
        self.assertEqual(recipe["policy"], "preserve")
        self.mutate("duplicate_edge")
        output, snapshot = self.prepared()
        aliases = self.assert_nodes(snapshot)
        self.assertEqual(len(snapshot.edges), len(self.expected["edges"]) + 1)
        self.assertEqual(snapshot.manifest["counts"]["edges"], len(snapshot.edges))
        semantic = lambda edge: (
            edge["type"],
            edge["owner"],
            edge["from"],
            edge["to"],
            json.dumps(edge["properties"], sort_keys=True, ensure_ascii=False),
        )
        expected = Counter(semantic(edge) for edge in self.expected["edges"])
        extra = next(
            edge for edge in self.expected["edges"] if edge["ref"] == recipe["edge"]
        )
        expected[semantic(extra)] += 1
        actual = Counter(
            semantic(
                dict(
                    edge,
                    **{"from": aliases[edge["source"]], "to": aliases[edge["target"]]},
                )
            )
            for edge in snapshot.edges
        )
        self.assertEqual(
            actual, expected, "Duplicate source rows lost parallel-edge semantics"
        )

        self.verify_strict(output)

    def test_missing_reference_does_not_publish(self):
        self.assert_rejected("missing_reference")

    def test_malformed_input_does_not_publish(self):
        self.assert_rejected("malformed")

    def test_unknown_adapter_fails_before_outputs(self):
        output, result = self.prepare(adapter="not-a-registered-adapter")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unknown adapter", result.stderr)
        self.assertFalse(output.exists())

    def test_assertions_detect_type_column_identity_and_direction_regressions(self):
        _, snapshot = self.prepared()
        self.assert_graph(snapshot)
        changed_type = copy.deepcopy(snapshot)
        changed_type.nodes[0]["type"] = "not-the-declared-type"
        missing_column = copy.deepcopy(snapshot)
        self.assertTrue(
            missing_column.nodes[0]["properties"],
            "Fixture needs properties to test column preservation",
        )
        missing_column.nodes[0]["properties"].pop(
            next(iter(missing_column.nodes[0]["properties"]))
        )
        duplicate_id = copy.deepcopy(snapshot)
        duplicate_id.nodes[1]["id"] = duplicate_id.nodes[0]["id"]
        reversed_edge = copy.deepcopy(snapshot)
        edge = next(
            row for row in reversed_edge.edges if row["source"] != row["target"]
        )
        edge["source"], edge["target"] = edge["target"], edge["source"]
        coercion = copy.deepcopy(snapshot)
        props = coercion.nodes[0]["properties"]
        key = next(iter(props))
        props[key] = 1 if type(props[key]) is bool else {"unexpected": True}
        for broken in (
            changed_type,
            missing_column,
            duplicate_id,
            reversed_edge,
            coercion,
        ):
            with self.subTest(broken=broken):
                with self.assertRaises(AssertionError):
                    self.assert_graph(broken)
