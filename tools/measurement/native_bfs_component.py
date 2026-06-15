#!/usr/bin/env python3
"""
Native backend BFS component for the university graph, with performance measurement.

This script is intentionally separate from the federated graph_db executor.
It replicates the entire JSON graph into each backend and then runs the
strongest backend-local BFS path:

  - Neo4j: APOC path expander over the whole replicated graph through HTTP.
  - ArangoDB: AQL named-graph traversal with order: "bfs".
  - GPU: calls the native_gpu_bfs C++ executable, which builds a full cuGraph
    projection and calls cugraph::bfs.

The important addition is measurement. Every expensive phase records wall-clock
runtime and item counts:

  - JSON parsing
  - Neo4j reset / node load / constraint creation / relationship load / BFS
  - ArangoDB reset / collection creation / node load / relationship load / BFS
  - GPU subprocess runtime
  - CSV writing

Example:

  python3 tools/native_bfs/native_bfs_component.py run-all \
      --schema storage_layer/sample-university-domain.graph-shape.json \
      --data tools/generators/large-university.json \
      --start-uuid 11a96cb7-c043-4aa7-aa35-2297e9be7f06 \
      --max-depth 7 \
      --output-dir native_bfs_results \
      --metrics-json native_bfs_results/metrics.json \
      --metrics-csv native_bfs_results/metrics.csv
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shlex
import subprocess
import sys
import time
from collections import defaultdict
from contextlib import contextmanager
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, DefaultDict, Dict, Iterable, Iterator, List, Optional, Sequence, Tuple

import requests


PROXY_NODE_TYPE = "Proxy"
GRAPH_NAME = "UnifiedGraph"
NEO4J_TX_ENDPOINT = "http://localhost:7474/db/neo4j/tx/commit"
ARANGO_BASE_URL = "http://localhost:8529"
ARANGO_DATABASE = "university"
GRAPH_NODE_LABEL = "__GraphNode"


# -----------------------------------------------------------------------------
# Measurement helpers
# -----------------------------------------------------------------------------


@dataclass
class Metric:
    name: str
    backend: str
    phase: str
    seconds: float
    count: Optional[int] = None
    details: Dict[str, Any] = field(default_factory=dict)


class PerfRecorder:
    def __init__(self) -> None:
        self.started_at = time.perf_counter()
        self.metrics: List[Metric] = []

    @contextmanager
    def measure(
            self,
            name: str,
            *,
            backend: str = "general",
            phase: str = "general",
            count: Optional[int] = None,
            **details: Any,
    ) -> Iterator[None]:
        start = time.perf_counter()
        try:
            yield
        finally:
            elapsed = time.perf_counter() - start
            self.metrics.append(
                Metric(
                    name=name,
                    backend=backend,
                    phase=phase,
                    seconds=elapsed,
                    count=count,
                    details={k: v for k, v in details.items() if v is not None},
                )
            )

    def record(
            self,
            name: str,
            seconds: float,
            *,
            backend: str = "general",
            phase: str = "general",
            count: Optional[int] = None,
            **details: Any,
    ) -> None:
        self.metrics.append(
            Metric(
                name=name,
                backend=backend,
                phase=phase,
                seconds=seconds,
                count=count,
                details={k: v for k, v in details.items() if v is not None},
            )
        )

    @property
    def total_wall_seconds(self) -> float:
        return time.perf_counter() - self.started_at

    def print_summary(self, stream: Any = sys.stderr) -> None:
        print("\n========== Native BFS performance summary ==========")
        print(f"Total command wall time: {self.total_wall_seconds:.3f} s", file=stream)

        grouped: Dict[Tuple[str, str, str], List[Metric]] = defaultdict(list)
        for metric in self.metrics:
            grouped[(metric.backend, metric.phase, metric.name)].append(metric)

        rows = []
        for (backend, phase, name), items in grouped.items():
            total = sum(m.seconds for m in items)
            calls = len(items)
            count = sum(m.count for m in items if m.count is not None)
            max_seconds = max(m.seconds for m in items)
            avg_seconds = total / calls if calls else 0.0
            rows.append((total, backend, phase, name, calls, count, avg_seconds, max_seconds))

        rows.sort(reverse=True, key=lambda x: x[0])

        print(
            f"{'total s':>10}  {'calls':>5}  {'items':>10}  {'avg ms':>10}  {'max ms':>10}  {'backend':<8}  {'phase':<18}  name",
            file=stream,
        )
        print("-" * 120, file=stream)
        for total, backend, phase, name, calls, count, avg_seconds, max_seconds in rows:
            items = str(count) if count else ""
            print(
                f"{total:10.3f}  {calls:5d}  {items:>10}  {avg_seconds * 1000:10.3f}  "
                f"{max_seconds * 1000:10.3f}  {backend:<8}  {phase:<18}  {name}",
                file=stream,
            )

        print("====================================================\n", file=stream)

    def write_json(self, path: str | Path) -> None:
        payload = {
            "total_wall_seconds": self.total_wall_seconds,
            "metrics": [asdict(m) for m in self.metrics],
        }
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2)

    def write_csv(self, path: str | Path) -> None:
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(["backend", "phase", "name", "seconds", "count", "details_json"])
            for metric in self.metrics:
                writer.writerow([
                    metric.backend,
                    metric.phase,
                    metric.name,
                    f"{metric.seconds:.9f}",
                    "" if metric.count is None else metric.count,
                    json.dumps(metric.details, sort_keys=True),
                ])


# -----------------------------------------------------------------------------
# Generic helpers
# -----------------------------------------------------------------------------


def sanitize_name(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not cleaned:
        cleaned = "X"
    if cleaned[0].isdigit():
        cleaned = f"X_{cleaned}"
    return cleaned


def cypher_ident(name: str) -> str:
    return f"`{sanitize_name(name)}`"


def entity_label() -> str:
    return cypher_ident(GRAPH_NODE_LABEL)


def chunks(items: Sequence[Dict[str, Any]], size: int) -> Iterable[Sequence[Dict[str, Any]]]:
    for i in range(0, len(items), size):
        yield items[i:i + size]


def load_json(path: str | Path) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


@dataclass(frozen=True)
class NodeRecord:
    uuid: str
    type: str
    source: str
    properties: Dict[str, Any]


@dataclass(frozen=True)
class RelationshipRecord:
    uuid: str
    type: str
    source: str
    from_uuid: str
    to_uuid: str
    properties: Dict[str, Any]


def parse_data(data_path: str | Path) -> Tuple[List[NodeRecord], List[RelationshipRecord]]:
    raw = load_json(data_path)
    nodes = [
        NodeRecord(
            uuid=n["uuid"],
            type=n["type"],
            source=n.get("source", ""),
            properties=n.get("properties", {}),
        )
        for n in raw.get("nodes", [])
    ]
    rels = [
        RelationshipRecord(
            uuid=r["uuid"],
            type=r["type"],
            source=r.get("source", ""),
            from_uuid=r["from"],
            to_uuid=r["to"],
            properties=r.get("properties", {}),
        )
        for r in raw.get("relationships", [])
    ]
    return nodes, rels


def relationship_types_from_schema(schema_path: str | Path) -> List[str]:
    schema = load_json(schema_path)
    return [sanitize_name(r["name"]) for r in schema.get("relations", [])]


def node_type_by_uuid(nodes: Sequence[NodeRecord]) -> Dict[str, str]:
    return {n.uuid: n.type for n in nodes}


def arango_doc_id_for_uuid(nodes: Sequence[NodeRecord], uuid: str) -> str:
    by_uuid = node_type_by_uuid(nodes)
    if uuid not in by_uuid:
        raise ValueError(f"Start uuid not found in JSON nodes: {uuid}")
    return f"{sanitize_name(by_uuid[uuid])}/{uuid}"


def write_csv(path: str | Path, rows: Iterable[Tuple[str, int]], perf: Optional[PerfRecorder] = None) -> None:
    cm = perf.measure("write CSV", backend="local", phase="output", output=str(path)) if perf else nullcontext()
    with cm:
        with open(path, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f, delimiter=";")
            writer.writerow(["vertex_id", "distance"])
            for vertex_id, distance in rows:
                writer.writerow([vertex_id, distance])


@contextmanager
def nullcontext() -> Iterator[None]:
    yield


# -----------------------------------------------------------------------------
# Neo4j native component
# -----------------------------------------------------------------------------


class Neo4jNativeComponent:
    def __init__(
            self,
            endpoint: str,
            username: str,
            password: str,
            batch_size: int = 10000,
            perf: Optional[PerfRecorder] = None,
    ):
        self.endpoint = endpoint
        self.username = username
        self.password = password
        self.batch_size = batch_size
        self.perf = perf or PerfRecorder()
        self.session = requests.Session()
        if username:
            self.session.auth = (username, password)
        self.session.headers.update({"Content-Type": "application/json"})

    def close(self) -> None:
        self.session.close()

    def tx(
            self,
            statements: List[Dict[str, Any]],
            *,
            metric_name: str = "Neo4j HTTP transaction",
            phase: str = "http",
            count: Optional[int] = None,
            **details: Any,
    ) -> Dict[str, Any]:
        with self.perf.measure(
                metric_name,
                backend="neo4j",
                phase=phase,
                count=count,
                statements=len(statements),
                **details,
        ):
            response = self.session.post(
                self.endpoint,
                json={"statements": statements},
                timeout=600,
            )

        if response.status_code != 200:
            raise RuntimeError(f"Neo4j HTTP {response.status_code}: {response.text}")

        with self.perf.measure(
                "Neo4j response JSON parse",
                backend="neo4j",
                phase="json_parse",
                **details,
        ):
            payload = response.json()

        if payload.get("errors"):
            raise RuntimeError(
                f"Neo4j returned errors: {json.dumps(payload['errors'], indent=2)}"
            )

        return payload

    def reset(self) -> None:
        self.tx(
            [{"statement": "MATCH (n) DETACH DELETE n"}],
            metric_name="Neo4j reset database",
            phase="reset",
        )

    def initialize_constraints(self) -> None:
        self.tx(
            [{
                "statement": f"""
                CREATE CONSTRAINT graphnode_uuid_unique IF NOT EXISTS
                FOR (n:{entity_label()})
                REQUIRE n.uuid IS UNIQUE
                """
            }],
            metric_name="Neo4j create graph-node UUID constraint",
            phase="schema",
        )

    def load_all(self, nodes: Sequence[NodeRecord], rels: Sequence[RelationshipRecord]) -> None:
        with self.perf.measure("Neo4j total load", backend="neo4j", phase="load", count=len(nodes) + len(rels)):
            self.reset()

            nodes_by_type: DefaultDict[str, List[Dict[str, Any]]] = defaultdict(list)
            with self.perf.measure("Neo4j prepare node batches", backend="neo4j", phase="prepare", count=len(nodes)):
                for node in nodes:
                    props = dict(node.properties)
                    props["uuid"] = node.uuid
                    props["type"] = node.type
                    props["source"] = node.source
                    nodes_by_type[node.type].append({"props": props})

            for node_type, rows in sorted(nodes_by_type.items()):
                label = cypher_ident(node_type)
                statement = f"""
                UNWIND $rows AS row
                CREATE (n:{entity_label()}:{label})
                SET n = row.props
                """

                for batch_index, batch in enumerate(chunks(rows, self.batch_size)):
                    self.tx(
                        [{"statement": statement, "parameters": {"rows": list(batch)}}],
                        metric_name="Neo4j create node batch",
                        phase="load_nodes",
                        count=len(batch),
                        node_type=node_type,
                        batch_index=batch_index,
                    )

            # Build the global UUID lookup after node creation. This avoids index
            # maintenance during the initial node inserts while still making the
            # relationship MATCH lookups fast.
            self.initialize_constraints()

            rels_by_type: DefaultDict[str, List[Dict[str, Any]]] = defaultdict(list)
            with self.perf.measure("Neo4j prepare relationship batches", backend="neo4j", phase="prepare", count=len(rels)):
                for rel in rels:
                    props = dict(rel.properties)
                    props["uuid"] = rel.uuid
                    props["type"] = rel.type
                    props["source"] = rel.source
                    rels_by_type[rel.type].append({
                        "from_uuid": rel.from_uuid,
                        "to_uuid": rel.to_uuid,
                        "props": props,
                    })

            for rel_type, rows in sorted(rels_by_type.items()):
                rtype = cypher_ident(rel_type)
                statement = f"""
                UNWIND $rows AS row
                MATCH (a:{entity_label()} {{uuid: row.from_uuid}})
                MATCH (b:{entity_label()} {{uuid: row.to_uuid}})
                CREATE (a)-[r:{rtype}]->(b)
                SET r = row.props
                """

                for batch_index, batch in enumerate(chunks(rows, self.batch_size)):
                    self.tx(
                        [{"statement": statement, "parameters": {"rows": list(batch)}}],
                        metric_name="Neo4j create relationship batch",
                        phase="load_relationships",
                        count=len(batch),
                        rel_type=rel_type,
                        batch_index=batch_index,
                    )

    def bfs(self, start_uuid: str, relationship_filter: str, max_depth: int, output: str | Path) -> None:
        with self.perf.measure("Neo4j total BFS command", backend="neo4j", phase="bfs"):
            statement = f"""
            MATCH (start:{entity_label()} {{uuid: $startUuid}})
            CALL apoc.path.expandConfig(start, {{
              minLevel: 0,
              maxLevel: $maxDepth,
              relationshipFilter: $relationshipFilter,
              bfs: true,
              uniqueness: 'NODE_GLOBAL'
            }}) YIELD path
            WITH last(nodes(path)) AS node, length(path) AS distance
            RETURN node.uuid AS vertex_id, distance AS distance
            """

            payload = self.tx(
                [{
                    "statement": statement,
                    "parameters": {
                        "startUuid": start_uuid,
                        "maxDepth": max_depth,
                        "relationshipFilter": relationship_filter,
                    },
                }],
                metric_name="Neo4j native APOC BFS HTTP query",
                phase="bfs_query",
            )

            with self.perf.measure("Neo4j convert BFS rows", backend="neo4j", phase="bfs_result"):
                rows = payload["results"][0]["data"]
                result = [(row["row"][0], row["row"][1]) for row in rows]

            with self.perf.measure("Neo4j sort BFS result", backend="neo4j", phase="bfs_result", count=len(result)):
                result.sort(key=lambda x: (x[1], x[0]))

            write_csv(output, result, perf=self.perf)

    @staticmethod
    def curl_command(
            start_uuid: str,
            relationship_filter: str,
            max_depth: int,
            endpoint: str = NEO4J_TX_ENDPOINT,
            username: str = "neo4j",
            password: str = "password",
    ) -> str:
        statement = f"""
MATCH (start:{entity_label()} {{uuid: $startUuid}})
CALL apoc.path.expandConfig(start, {{
  minLevel: 0,
  maxLevel: $maxDepth,
  relationshipFilter: $relationshipFilter,
  bfs: true,
  uniqueness: 'NODE_GLOBAL'
}}) YIELD path
WITH last(nodes(path)) AS node, length(path) AS distance
RETURN node.uuid AS vertex_id, distance AS distance
""".strip()
        payload = {
            "statements": [{
                "statement": statement,
                "parameters": {
                    "startUuid": start_uuid,
                    "maxDepth": max_depth,
                    "relationshipFilter": relationship_filter,
                },
            }]
        }
        json_payload = shlex.quote(json.dumps(payload))
        auth = shlex.quote(f"{username}:{password}")
        endpoint_arg = shlex.quote(endpoint)
        return (
            f"curl -s -u {auth} "
            f"-H 'Content-Type: application/json' "
            f"{endpoint_arg} "
            f"-d {json_payload}"
        )


# -----------------------------------------------------------------------------
# Arango native component
# -----------------------------------------------------------------------------


class ArangoNativeComponent:
    def __init__(
            self,
            base_url: str,
            username: str,
            password: str,
            database: str,
            batch_size: int = 10000,
            perf: Optional[PerfRecorder] = None,
    ):
        self.base_url = base_url.rstrip("/")
        self.username = username
        self.password = password
        self.database = database
        self.batch_size = batch_size
        self.perf = perf or PerfRecorder()
        self.session = requests.Session()
        self.session.auth = (username, password)
        self.session.headers.update({"Content-Type": "application/json"})

    def close(self) -> None:
        self.session.close()

    def system_api(self, path: str) -> str:
        return f"{self.base_url}{path}"

    def db_api(self, path: str) -> str:
        return f"{self.base_url}/_db/{self.database}{path}"

    def request(
            self,
            method: str,
            url: str,
            *,
            metric_name: str,
            phase: str,
            count: Optional[int] = None,
            **kwargs: Any,
    ) -> requests.Response:
        with self.perf.measure(metric_name, backend="arango", phase=phase, count=count, url=url):
            return self.session.request(method, url, timeout=kwargs.pop("timeout", 600), **kwargs)

    def check(self, response: requests.Response, ok: Tuple[int, ...], context: str) -> Dict[str, Any]:
        if response.status_code not in ok:
            raise RuntimeError(f"{context} failed: HTTP {response.status_code}: {response.text}")
        if not response.text:
            return {}
        with self.perf.measure("Arango response JSON parse", backend="arango", phase="json_parse", context=context):
            return response.json()

    def reset_database(self) -> None:
        resp = self.request(
            "DELETE",
            self.system_api(f"/_api/database/{self.database}"),
            metric_name="Arango delete database",
            phase="reset",
            timeout=120,
        )
        if resp.status_code not in (200, 202, 404):
            raise RuntimeError(f"Delete Arango database failed: HTTP {resp.status_code}: {resp.text}")

        resp = self.request(
            "POST",
            self.system_api("/_api/database"),
            metric_name="Arango create database",
            phase="reset",
            json={"name": self.database},
            timeout=120,
        )
        self.check(resp, (201,), "Create Arango database")

    def ensure_collection(self, name: str, edge: bool = False) -> None:
        payload: Dict[str, Any] = {"name": sanitize_name(name)}
        if edge:
            payload["type"] = 3
        resp = self.request(
            "POST",
            self.db_api("/_api/collection"),
            metric_name="Arango ensure collection",
            phase="schema",
            json=payload,
            timeout=120,
        )
        if resp.status_code not in (200, 201, 409):
            raise RuntimeError(f"Create Arango collection {name} failed: HTTP {resp.status_code}: {resp.text}")

    def load_all(self, nodes: Sequence[NodeRecord], rels: Sequence[RelationshipRecord]) -> None:
        with self.perf.measure("Arango total load", backend="arango", phase="load", count=len(nodes) + len(rels)):
            self.reset_database()
            vertex_collections = sorted({sanitize_name(n.type) for n in nodes})
            edge_collections = sorted({sanitize_name(r.type) for r in rels})

            for collection in vertex_collections:
                self.ensure_collection(collection, edge=False)
            for collection in edge_collections:
                self.ensure_collection(collection, edge=True)

            nodes_by_type: DefaultDict[str, List[Dict[str, Any]]] = defaultdict(list)
            with self.perf.measure("Arango prepare node documents", backend="arango", phase="prepare", count=len(nodes)):
                for node in nodes:
                    doc = dict(node.properties)
                    doc.update({"_key": node.uuid, "uuid": node.uuid, "type": node.type, "source": node.source})
                    nodes_by_type[node.type].append(doc)

            for node_type, docs in sorted(nodes_by_type.items()):
                collection = sanitize_name(node_type)
                for batch_index, batch in enumerate(chunks(docs, self.batch_size)):
                    resp = self.request(
                        "POST",
                        self.db_api(f"/_api/document/{collection}"),
                        metric_name="Arango insert node batch",
                        phase="load_nodes",
                        count=len(batch),
                        params={"overwriteMode": "replace"},
                        json=list(batch),
                        timeout=600,
                    )
                    self.check(resp, (201, 202), f"Insert Arango nodes into {collection}")

            type_by_uuid = node_type_by_uuid(nodes)
            rels_by_type: DefaultDict[str, List[Dict[str, Any]]] = defaultdict(list)
            with self.perf.measure("Arango prepare edge documents", backend="arango", phase="prepare", count=len(rels)):
                for rel in rels:
                    from_type = type_by_uuid[rel.from_uuid]
                    to_type = type_by_uuid[rel.to_uuid]
                    doc = dict(rel.properties)
                    doc.update({
                        "_key": rel.uuid,
                        "_from": f"{sanitize_name(from_type)}/{rel.from_uuid}",
                        "_to": f"{sanitize_name(to_type)}/{rel.to_uuid}",
                        "uuid": rel.uuid,
                        "type": rel.type,
                        "source": rel.source,
                    })
                    rels_by_type[rel.type].append(doc)

            for rel_type, docs in sorted(rels_by_type.items()):
                collection = sanitize_name(rel_type)
                for batch_index, batch in enumerate(chunks(docs, self.batch_size)):
                    resp = self.request(
                        "POST",
                        self.db_api(f"/_api/document/{collection}"),
                        metric_name="Arango insert edge batch",
                        phase="load_relationships",
                        count=len(batch),
                        params={"overwriteMode": "replace"},
                        json=list(batch),
                        timeout=600,
                    )
                    self.check(resp, (201, 202), f"Insert Arango edges into {collection}")

            edge_definitions = [
                {"collection": edge_col, "from": vertex_collections, "to": vertex_collections}
                for edge_col in edge_collections
            ]
            resp = self.request(
                "POST",
                self.db_api("/_api/gharial"),
                metric_name="Arango create named graph",
                phase="schema",
                json={
                    "name": GRAPH_NAME,
                    "edgeDefinitions": edge_definitions,
                    "orphanCollections": vertex_collections,
                },
                timeout=600,
            )
            self.check(resp, (201, 202, 409), "Create Arango named graph")

    def cursor_query(self, query: str, bind_vars: Dict[str, Any], *, metric_name: str, phase: str) -> List[Any]:
        resp = self.request(
            "POST",
            self.db_api("/_api/cursor"),
            metric_name=metric_name,
            phase=phase,
            json={"query": query, "bindVars": bind_vars, "batchSize": 10000},
            timeout=600,
        )
        payload = self.check(resp, (201,), "Run Arango cursor query")
        result = list(payload.get("result", []))
        cursor_id = payload.get("id")

        while payload.get("hasMore") and cursor_id:
            resp = self.request(
                "PUT",
                self.db_api(f"/_api/cursor/{cursor_id}"),
                metric_name="Arango BFS cursor next page",
                phase=phase,
                timeout=600,
            )
            payload = self.check(resp, (200,), "Read next Arango cursor page")
            result.extend(payload.get("result", []))
            cursor_id = payload.get("id")

        return result

    def bfs(self, start_id: str, max_depth: int, output: str | Path) -> None:
        with self.perf.measure("Arango total BFS command", backend="arango", phase="bfs"):
            query = f"""
            FOR v, e, p IN 0..@maxDepth ANY @startId GRAPH '{GRAPH_NAME}'
              OPTIONS {{ order: "bfs", uniqueVertices: "global" }}
              RETURN {{ vertex_id: v.uuid, distance: LENGTH(p.edges) }}
            """
            rows = self.cursor_query(
                query,
                {"startId": start_id, "maxDepth": max_depth},
                metric_name="Arango native BFS cursor query",
                phase="bfs_query",
            )

            with self.perf.measure("Arango convert/sort BFS rows", backend="arango", phase="bfs_result", count=len(rows)):
                pairs = sorted(((r["vertex_id"], r["distance"]) for r in rows), key=lambda x: (x[1], x[0]))

            write_csv(output, pairs, perf=self.perf)


# -----------------------------------------------------------------------------
# Commands
# -----------------------------------------------------------------------------


def run_command(
        cmd: Sequence[str],
        cwd: Optional[str | Path] = None,
        perf: Optional[PerfRecorder] = None,
        metric_name: str = "subprocess",
        backend: str = "local",
        phase: str = "subprocess",
        output: Optional[Any] = None,
) -> None:
    print("+", " ".join(str(x) for x in cmd), file=sys.stderr)
    cm = perf.measure(metric_name, backend=backend, phase=phase, command=" ".join(map(str, cmd))) if perf else nullcontext()
    with cm:
        subprocess.run(list(cmd), cwd=cwd, stdout=output, check=True)


def wait_http(url: str, timeout: int, auth: Optional[Tuple[str, str]] = None, perf: Optional[PerfRecorder] = None) -> None:
    deadline = time.time() + timeout
    last_error: Optional[Exception] = None
    attempts = 0
    with (perf.measure("wait for HTTP service", backend="docker", phase="startup", url=url) if perf else nullcontext()):
        while time.time() < deadline:
            attempts += 1
            try:
                resp = requests.get(url, auth=auth, timeout=5)
                if resp.status_code < 500:
                    return
            except Exception as exc:  # noqa: BLE001 - CLI helper
                last_error = exc
            time.sleep(1)
    raise RuntimeError(f"Timed out waiting for {url} after {attempts} attempts. Last error: {last_error}")


def cmd_up(args: argparse.Namespace) -> None:
    compose = Path(args.compose)
    run_command(
        ["docker", "compose", "-f", str(compose), "up", "-d"],
        perf=args.perf,
        metric_name="docker compose up",
        backend="docker",
        phase="startup",
    )
    if args.wait:
        wait_http("http://localhost:7474", timeout=args.timeout, perf=args.perf)
        wait_http(
            f"{args.arango_url}/_api/version",
            timeout=args.timeout,
            auth=(args.arango_user, args.arango_password),
            perf=args.perf,
        )


def cmd_load(args: argparse.Namespace) -> None:
    with args.perf.measure("parse input JSON", backend="local", phase="input", data=args.data):
        nodes, rels = parse_data(args.data)

    print(f"Loading full graph: {len(nodes)} nodes, {len(rels)} relationships", file=sys.stderr)

    neo = Neo4jNativeComponent(args.neo4j_endpoint, args.neo4j_user, args.neo4j_password, args.batch_size, perf=args.perf)
    arango = ArangoNativeComponent(args.arango_url, args.arango_user, args.arango_password, args.arango_db, args.batch_size, perf=args.perf)
    try:
        if not args.skip_neo4j:
            print("Loading Neo4j full replica", file=sys.stderr)
            neo.load_all(nodes, rels)
        if not args.skip_arango:
            print("Loading ArangoDB full replica", file=sys.stderr)
            arango.load_all(nodes, rels)
    finally:
        neo.close()
        arango.close()


def cmd_neo4j_bfs(args: argparse.Namespace) -> None:
    with args.perf.measure("parse schema relationship types", backend="local", phase="input", schema=args.schema):
        rel_filter = "|".join(relationship_types_from_schema(args.schema))
    neo = Neo4jNativeComponent(args.neo4j_endpoint, args.neo4j_user, args.neo4j_password, args.batch_size, perf=args.perf)
    try:
        neo.bfs(args.start_uuid, rel_filter, args.max_depth, args.output)
    finally:
        neo.close()


def cmd_print_neo4j_curl(args: argparse.Namespace) -> None:
    rel_filter = "|".join(relationship_types_from_schema(args.schema))
    print(Neo4jNativeComponent.curl_command(
        args.start_uuid,
        rel_filter,
        args.max_depth,
        endpoint=args.neo4j_endpoint,
        username=args.neo4j_user,
        password=args.neo4j_password,
    ))


def cmd_arango_bfs(args: argparse.Namespace) -> None:
    with args.perf.measure("parse input JSON", backend="local", phase="input", data=args.data):
        nodes, _ = parse_data(args.data)
    start_id = arango_doc_id_for_uuid(nodes, args.start_uuid)
    arango = ArangoNativeComponent(args.arango_url, args.arango_user, args.arango_password, args.arango_db, args.batch_size, perf=args.perf)
    try:
        arango.bfs(start_id, args.max_depth, args.output)
    finally:
        arango.close()


def cmd_gpu_bfs(args: argparse.Namespace) -> None:
    binary = Path(args.binary)
    cmd = [
        str(binary),
        "--logical-graph-schema", args.schema,
        "--data", args.data,
        "--start-uuid", args.start_uuid,
        "--max-depth", str(args.max_depth),
    ]
    if args.directed:
        cmd.append("--directed")
    with open(args.output, "w", encoding="utf-8") as f:
        run_command(cmd, perf=args.perf, metric_name="GPU native BFS subprocess", backend="gpu", phase="bfs_query", output=f)


def cmd_run_all(args: argparse.Namespace) -> None:
    cmd_load(argparse.Namespace(**vars(args)))

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not args.skip_neo4j:
        neo_args = argparse.Namespace(**vars(args))
        neo_args.output = str(out_dir / "native-neo4j.csv")
        cmd_neo4j_bfs(neo_args)

    if not args.skip_arango:
        arango_args = argparse.Namespace(**vars(args))
        arango_args.output = str(out_dir / "native-arango.csv")
        cmd_arango_bfs(arango_args)

    if not args.skip_gpu and args.binary:
        gpu_args = argparse.Namespace(**vars(args))
        gpu_args.output = str(out_dir / "native-gpu.csv")
        cmd_gpu_bfs(gpu_args)


def add_common_db_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--neo4j-endpoint", default=NEO4J_TX_ENDPOINT)
    parser.add_argument("--neo4j-user", default="neo4j")
    parser.add_argument("--neo4j-password", default="password")
    parser.add_argument("--arango-url", default=ARANGO_BASE_URL)
    parser.add_argument("--arango-user", default="root")
    parser.add_argument("--arango-password", default="rootpassword")
    parser.add_argument("--arango-db", default=ARANGO_DATABASE)
    parser.add_argument("--batch-size", type=int, default=10000)


def add_metrics_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--metrics-json", default=None, help="Write detailed timing metrics to this JSON file")
    parser.add_argument("--metrics-csv", default=None, help="Write detailed timing metrics to this CSV file")
    parser.add_argument("--quiet-metrics", action="store_true", help="Do not print the timing summary to stderr")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Native Neo4j / ArangoDB / cuGraph BFS component with timing")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("up", help="Start Neo4j and ArangoDB containers")
    p.add_argument("--compose", default="storage_layer/docker-compose.yml")
    p.add_argument("--wait", action="store_true", default=True)
    p.add_argument("--timeout", type=int, default=120)
    add_common_db_args(p)
    add_metrics_args(p)
    p.set_defaults(func=cmd_up)

    p = sub.add_parser("load", help="Replicate the full JSON graph into Neo4j and ArangoDB")
    p.add_argument("--data", required=True)
    p.add_argument("--schema", required=False)
    p.add_argument("--skip-neo4j", action="store_true")
    p.add_argument("--skip-arango", action="store_true")
    add_common_db_args(p)
    add_metrics_args(p)
    p.set_defaults(func=cmd_load)

    p = sub.add_parser("neo4j-bfs", help="Run Neo4j APOC BFS over the full Neo4j replica")
    p.add_argument("--schema", required=True)
    p.add_argument("--data", required=False)
    p.add_argument("--start-uuid", required=True)
    p.add_argument("--max-depth", type=int, default=7)
    p.add_argument("--output", required=True)
    add_common_db_args(p)
    add_metrics_args(p)
    p.set_defaults(func=cmd_neo4j_bfs)

    p = sub.add_parser("print-neo4j-curl", help="Print the direct curl command for Neo4j BFS")
    p.add_argument("--schema", required=True)
    p.add_argument("--start-uuid", required=True)
    p.add_argument("--max-depth", type=int, default=7)
    add_common_db_args(p)
    add_metrics_args(p)
    p.set_defaults(func=cmd_print_neo4j_curl)

    p = sub.add_parser("arango-bfs", help="Run ArangoDB AQL BFS over the full ArangoDB replica")
    p.add_argument("--data", required=True)
    p.add_argument("--start-uuid", required=True)
    p.add_argument("--max-depth", type=int, default=7)
    p.add_argument("--output", required=True)
    add_common_db_args(p)
    add_metrics_args(p)
    p.set_defaults(func=cmd_arango_bfs)

    p = sub.add_parser("gpu-bfs", help="Run native_gpu_bfs C++ executable over a full cuGraph projection")
    p.add_argument("--binary", default="src/core/build/native_gpu_bfs")
    p.add_argument("--schema", required=True)
    p.add_argument("--data", required=True)
    p.add_argument("--start-uuid", required=True)
    p.add_argument("--max-depth", type=int, default=7)
    p.add_argument("--output", required=True)
    p.add_argument("--directed", action="store_true")
    add_metrics_args(p)
    p.set_defaults(func=cmd_gpu_bfs)

    p = sub.add_parser("run-all", help="Load full replicas and run all available native BFS engines")
    p.add_argument("--schema", required=True)
    p.add_argument("--data", required=True)
    p.add_argument("--start-uuid", required=True)
    p.add_argument("--max-depth", type=int, default=7)
    p.add_argument("--output-dir", default="native_bfs_results")
    p.add_argument("--binary") #, default="src/core/build/native_gpu_bfs")
    p.add_argument("--directed", action="store_true")
    p.add_argument("--skip-neo4j", action="store_true")
    p.add_argument("--skip-arango", action="store_true")
    p.add_argument("--skip-gpu", action="store_true")
    add_common_db_args(p)
    add_metrics_args(p)
    p.set_defaults(func=cmd_run_all)

    args = parser.parse_args(argv)
    args.perf = PerfRecorder()

    try:
        args.func(args)
        return_code = 0
    finally:
        if args.metrics_json:
            args.perf.write_json(args.metrics_json)
        if args.metrics_csv:
            args.perf.write_csv(args.metrics_csv)
        if not args.quiet_metrics:
            args.perf.print_summary()

    return return_code


if __name__ == "__main__":
    raise SystemExit(main())