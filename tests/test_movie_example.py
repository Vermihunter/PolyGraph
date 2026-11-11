"""Offline proof of the example adapter through the shared import workflow."""

import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from collections import defaultdict

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "examples/movie-graph"


class MovieExampleTest(unittest.TestCase):
    def setUp(self):
        value = os.environ.get("POLYGRAPH_MOVIE_IMPORT")
        if not value:
            self.skipTest("CTest supplies the linked example binary")
        self.binary = Path(value).resolve()
        self.temp = tempfile.TemporaryDirectory(prefix="polygraph-movie-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.example = self.root / "movie"
        shutil.copytree(EXAMPLE, self.example)
        self.output = self.root / "workspace"

    def run_import(self, success=True):
        result = subprocess.run(
            [
                sys.executable,
                str(EXAMPLE / "onboard.py"),
                "--binary",
                str(self.binary),
                "--manifest",
                str(self.example / "domain.json"),
                "--workspace-root",
                str(self.output),
            ],
            cwd=self.root,
            capture_output=True,
            text=True,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("phase=prepare", result.stderr)
        return result

    def test_normalized_types_properties_and_three_hop_oracle(self):
        result = self.run_import()
        manifest_path = Path(result["manifest"])
        manifest = json.loads(manifest_path.read_text())
        oracle = json.loads((self.example / "expected.json").read_text())
        self.assertEqual(manifest["counts"], oracle["counts"])
        self.assertEqual(result["adapter"], "movie-graph")
        self.assertEqual(result["state"], "prepared")
        with sqlite3.connect(
            (manifest_path.parent / manifest["index"]["path"]).as_uri() + "?mode=ro",
            uri=True,
        ) as db:
            nodes = {}
            properties = {}
            for node_id, key, props in db.execute(
                "SELECT id,key,properties FROM nodes"
            ):
                identity = json.loads(key)
                self.assertEqual(
                    identity[:3], ["polygraph.source.v1", "movie-graph", "vertex"]
                )
                self.assertEqual(identity[4], "int64")
                nodes[node_id] = f"{identity[3]}:{identity[5]}"
                properties[nodes[node_id]] = json.loads(props)
            self.assertEqual(len(nodes), 9)
            self.assertIs(properties["Person:1"]["active"], True)
            self.assertIsInstance(properties["Person:1"]["birth_year"], int)
            self.assertIsInstance(properties["Film:1"]["rating"], float)
            self.assertEqual(properties["Film:1"]["title"], "Orbit, One")
            edges = list(db.execute("SELECT from_id,to_id FROM edges"))
        for root in oracle["roots"]:
            with self.subTest(root=root):
                adjacency = defaultdict(set)
                for source, target in edges:
                    adjacency[nodes[source]].add(nodes[target])
                    if root["direction"] == "undirected":
                        adjacency[nodes[target]].add(nodes[source])
                frontier = {f"{root['type']}:{root['id']}"}
                seen = set()
                for expected in root["layers"]:
                    self.assertEqual(frontier, set(expected))
                    seen.update(frontier)
                    frontier = {
                        neighbor for node in frontier for neighbor in adjacency[node]
                    } - seen
        for backend in ("Neo4J", "ArangoDB", "PostgreSQL"):
            self.assertGreater(manifest["backends"][backend]["counts"]["nodes"], 0)
            self.assertGreater(manifest["backends"][backend]["counts"]["edges"], 0)
            self.assertGreater(manifest["backends"][backend]["counts"]["proxies"], 0)
        verified = subprocess.run(
            [str(self.binary), "verify", "--manifest", str(manifest_path), "--strict"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_same_command_reuses_unchanged_inputs_and_retains_old_generation_after_edit(
        self,
    ):
        first = self.run_import()
        old = Path(first["manifest"])
        original = old.read_bytes()
        timestamp = old.stat().st_mtime_ns
        second = self.run_import()
        self.assertEqual(second["manifest"], first["manifest"])
        self.assertEqual(old.stat().st_mtime_ns, timestamp)
        people = self.example / "csv/people.csv"
        with people.open("a") as stream:
            stream.write("5,Ellis Stone,1995,true\n")
        third = self.run_import()
        self.assertNotEqual(third["manifest"], first["manifest"])
        self.assertEqual(third["counts"], {"nodes": 10, "edges": 9})
        self.assertEqual(old.read_bytes(), original)
        self.assertEqual(old.stat().st_mtime_ns, timestamp)

    def test_bad_typed_values_and_missing_endpoints_are_not_coerced(self):
        people = self.example / "csv/people.csv"
        original = people.read_text()
        people.write_text(original.replace("1970,true", "1970,yes"))
        result = self.run_import(success=False)
        self.assertIn("people.csv:record 2", result.stderr)
        self.assertIn("true or false", result.stderr)
        people.write_text(original)
        credits = self.example / "csv/acted_in.csv"
        credits.write_text(credits.read_text().replace("1,1,1,", "1,1,999,"))
        self.run_import(success=False)

    def test_stock_cli_remains_builtin_and_movie_cli_validates_without_outputs(self):
        listing = subprocess.run(
            [str(self.binary), "adapters"], capture_output=True, text=True
        )
        self.assertEqual(listing.returncode, 0)
        self.assertEqual(
            listing.stdout.splitlines(), ["ldbc-snb", "movie-graph", "polygraph-jsonl"]
        )
        manifest = self.example / "domain.json"
        document = json.loads(manifest.read_text())
        document["workspace"] = {"root": str(self.output)}
        manifest.write_text(json.dumps(document))
        result = subprocess.run(
            [
                str(self.binary),
                "import",
                "--graph-manifest",
                str(manifest),
                "--dry-run",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.output.exists())
        self.assertEqual(json.loads(result.stdout)["adapter"], "movie-graph")
