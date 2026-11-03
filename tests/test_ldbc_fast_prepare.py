from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.graphio import open_graph_reader


def _placement(path: Path) -> None:
    path.write_text(
        json.dumps(
            {
                "storageBackends": ["Neo4J"],
                "defaultNodeSource": "Neo4J",
                "defaultRelationshipSource": "Neo4J",
            }
        ),
        encoding="utf-8",
    )


def test_fast_prepare_avoids_graph_jsonl_and_preserves_records(tmp_path: Path) -> None:
    fixture = ROOT / "tests/fixtures/ldbc_snb/social_network"
    placement = tmp_path / "placement.json"
    _placement(placement)

    normal_graph = tmp_path / "normal.jsonl"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/ldbc_snb/convert.py"),
            "--input-dir", str(fixture),
            "--output", str(normal_graph),
            "--schema-output", str(tmp_path / "normal-schema.json"),
            "--placement-config", str(placement),
            "--storage-backends", "Neo4J",
        ],
        check=True,
    )

    fast = tmp_path / "fast"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/ldbc_snb/fast_prepare.py"),
            "--input", str(fixture),
            "--sf", "0.1",
            "--domain-id", "ldbc-test-fast",
            "--output-dir", str(fast),
            "--placement-config", str(placement),
            "--storage-backends", "Neo4J",
            "--root-count", "2",
            "--no-render-placement-graph",
        ],
        check=True,
    )

    assert not (fast / "graph.jsonl").exists()
    descriptor = fast / "graph.ldbc.json"
    assert descriptor.is_file()
    assert (fast / "caches/08-no-cache/residency.bin").read_bytes() == b""

    normal = open_graph_reader(normal_graph).load()
    raw = open_graph_reader(descriptor).load()
    assert raw.nodes == normal.nodes
    assert raw.relationships == normal.relationships

    plan = json.loads((fast / "caches/08-no-cache/cache-plan.json").read_text())
    assert Path(plan["graph"]).resolve() == descriptor.resolve()
    assert plan["nodes"] == len(normal.nodes)
    assert plan["relationships"] == len(normal.relationships)
    assert plan["artifacts"]["residency"]["entries"] == 0

    domain = json.loads((fast / "domain.json").read_text())
    assert domain["paths"]["data"] == "graph.ldbc.json"
    assert domain["paths"]["cachePlan"] == "caches/08-no-cache/cache-plan.json"
