"""Real-backend assertions for the opt-in disposable graph fixture harness.

Import, Docker ownership and cancellation remain in the existing shared workflow.
Only generated owned databases are queried; this module never resets a database.
"""

from __future__ import annotations

from contextlib import closing
import csv
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import sqlite3
import subprocess
import time

from tools.graphio.domain import sanitize_name
from tools.ingestion.docker_environment import (
    DockerEnvironment,
    WorkflowError,
    WorkflowInterrupted,
    private_json,
    request,
)
from tools.ingestion.docker_load import bootstrap_runtime, cancellation_scope, stage

ROOT = Path(__file__).resolve().parents[2]
SKIPPED = 77
BACKENDS = ("Neo4J", "ArangoDB", "PostgreSQL")
TRANSPORT = {
    "uuid",
    "localId",
    "type",
    "source",
    "is_proxy",
    "cache_residency",
    "_id",
    "_key",
    "_rev",
    "_from",
    "_to",
}


def unsigned(value):
    return int(value) & ((1 << 64) - 1)


def properties(document):
    return {key: value for key, value in document.items() if key not in TRANSPORT}


def _neo_id(document):
    return (
        unsigned(document["uuid"])
        if "uuid" in document
        else (1 << 48) | int(document["localId"])
    )


def normalize_neo_node(labels, document):
    proxy = document["is_proxy"]
    kind = document["remote-type"] if proxy else document["type"]
    if ("Proxy" if proxy else sanitize_name(kind)) not in labels:
        raise WorkflowError("Neo4J physical label disagrees with logical node type")
    return {
        "id": _neo_id(document),
        "type": kind,
        "source": document["source"],
        "is_proxy": proxy,
        "properties": properties(document),
    }


def normalize_arango_node(document):
    proxy = document.get("is_proxy", False)
    kind = document["remote-type"] if proxy else document["type"]
    identifier = unsigned(document["uuid"])
    collection, key = document["_id"].split("/", 1)
    if (
        key != str(identifier)
        or document["_key"] != key
        or collection != ("Proxy" if proxy else sanitize_name(kind))
    ):
        raise WorkflowError(
            "ArangoDB physical collection/key disagrees with logical node"
        )
    return {
        "id": identifier,
        "type": kind,
        "source": document["source"],
        "is_proxy": proxy,
        "properties": properties(document),
    }


def _compare(actual, expected, context):
    # JSONB/VPack may serialize integral floating-point numbers without a decimal.
    numeric_float = type(expected) is float and type(actual) in (int, float)
    if not numeric_float and type(actual) is not type(expected):
        raise WorkflowError(f"{context}: value type mismatch")
    if isinstance(expected, dict):
        if set(actual) != set(expected):
            raise WorkflowError(f"{context}: property/metadata keys differ")
        for key, value in expected.items():
            _compare(actual[key], value, context + "." + key)
    elif isinstance(expected, list):
        if len(actual) != len(expected):
            raise WorkflowError(f"{context}: array length differs")
        for index, value in enumerate(expected):
            _compare(actual[index], value, context + f"[{index}]")
    elif actual != expected:
        raise WorkflowError(f"{context}: value mismatch")


def compare_records(expected, actual, context):
    expected_by_id = {row["id"]: row for row in expected}
    actual_by_id = {row["id"]: row for row in actual}
    if len(actual_by_id) != len(actual) or set(expected_by_id) != set(actual_by_id):
        raise WorkflowError(f"{context}: count/ID mismatch")
    for identifier, row in expected_by_id.items():
        _compare(actual_by_id[identifier], row, f"{context}[{identifier}]")


def check_prerequisites(binary, bfs, adapter, images, *, runner=subprocess.run):
    reasons, details = [], {"ready": False, "images": {}}
    for name, path in [("importer", binary), ("BFS", bfs)]:
        if not path.is_file() or not os.access(path, os.X_OK):
            reasons.append(f"{name} executable unavailable: {path}")
    if binary.is_file() and os.access(binary, os.X_OK):
        result = runner([str(binary), "adapters"], text=True, capture_output=True)
        if result.returncode or adapter not in result.stdout.splitlines():
            reasons.append(
                f"Importer does not expose adapter {adapter}; build polygraph_movie_import"
            )
    try:
        result = runner(
            ["docker", "info", "--format", "{{.ServerVersion}}"],
            text=True,
            capture_output=True,
        )
        if result.returncode:
            reasons.append("Docker daemon unavailable or permission denied")
        else:
            details["docker_version"] = result.stdout.strip()
            for key, image in images.items():
                result = runner(
                    ["docker", "image", "inspect", "--format", "{{.Id}}", image],
                    text=True,
                    capture_output=True,
                )
                if result.returncode:
                    reasons.append("Required cached image unavailable: " + image)
                else:
                    details["images"][key] = {
                        "name": image,
                        "id": result.stdout.strip(),
                    }
    except OSError:
        reasons.append("Docker CLI unavailable")
    details["ready"] = not reasons
    return reasons, details


def _docker_read(args):
    result = subprocess.run(["docker", *args], text=True, capture_output=True)
    if result.returncode:
        raise WorkflowError("Cannot inspect Docker resources: " + result.stderr.strip())
    return result.stdout


def resource_snapshot():
    """Keep only stable metadata fingerprints; never persist user Config.Env/options."""
    result = {"containers": {}, "volumes": {}}
    for identifier in _docker_read(["ps", "-aq", "--no-trunc"]).split():
        row = json.loads(_docker_read(["inspect", identifier]))[0]
        value = {
            "id": row["Id"],
            "image": row["Image"],
            "mounts": sorted(
                row.get("Mounts", []),
                key=lambda mount: json.dumps(mount, sort_keys=True),
            ),
            "running": row["State"]["Running"],
            "started": row["State"]["StartedAt"],
            "finished": row["State"]["FinishedAt"],
        }
        result["containers"][identifier] = hashlib.sha256(
            json.dumps(value, sort_keys=True).encode()
        ).hexdigest()
    for name in _docker_read(["volume", "ls", "-q"]).split():
        value = json.loads(_docker_read(["volume", "inspect", name]))[0]
        result["volumes"][name] = hashlib.sha256(
            json.dumps(value, sort_keys=True).encode()
        ).hexdigest()
    return result


def assert_preserved(before, after):
    for kind in ("containers", "volumes"):
        for identifier, fingerprint in before[kind].items():
            if after[kind].get(identifier) != fingerprint:
                raise WorkflowError(f"Pre-existing Docker {kind} changed: {identifier}")


def normalized_graph(environment, manifest):
    index = environment.artifacts / manifest["index"]["path"]
    with closing(sqlite3.connect(index.as_uri() + "?mode=ro", uri=True)) as db:
        nodes = {
            unsigned(id): {
                "id": unsigned(id),
                "key": key,
                "type": kind,
                "owner": owner,
                "properties": json.loads(props),
            }
            for id, key, kind, owner, props in db.execute(
                "SELECT id,key,type,owner,properties FROM nodes"
            )
        }
        edges = {
            unsigned(id): {
                "id": unsigned(id),
                "type": kind,
                "source": owner,
                "from": unsigned(source),
                "to": unsigned(target),
                "properties": json.loads(props),
            }
            for id, kind, owner, source, target, props in db.execute(
                "SELECT id,type,owner,from_id,to_id,properties FROM edges"
            )
        }
        proxies = [
            (backend, unsigned(id))
            for backend, id in db.execute("SELECT backend,node_id FROM proxies")
        ]
        replicas = [
            (backend, unsigned(id))
            for backend, id in db.execute("SELECT backend,edge_id FROM replicas")
        ]
    expected = {backend: {"nodes": [], "edges": []} for backend in BACKENDS}
    for node in nodes.values():
        expected[node["owner"]]["nodes"].append(
            {
                "id": node["id"],
                "type": node["type"],
                "source": node["owner"],
                "is_proxy": False,
                "properties": properties(node["properties"]),
            }
        )
    for backend, identifier in proxies:
        node = nodes[identifier]
        expected[backend]["nodes"].append(
            {
                "id": identifier,
                "type": node["type"],
                "source": backend,
                "is_proxy": True,
                "properties": {
                    "remote-type": node["type"],
                    "remote-source": node["owner"],
                },
            }
        )
    for backend, identifier in replicas:
        expected[backend]["edges"].append(edges[identifier])
    return nodes, edges, expected


def _cypher(environment, statement):
    if environment.owned_container("neo") is None:
        raise WorkflowError("Owned Neo4j container is missing")
    endpoint = environment.targets["targets"]["Neo4J"]["endpoint"]
    result = request(
        endpoint + f"/db/{environment.database}/tx/commit",
        "neo4j",
        environment.password,
        {"statements": [{"statement": statement}]},
    )
    if result.get("errors"):
        raise WorkflowError("Neo4J read failed: " + json.dumps(result["errors"]))
    return [row["row"] for row in result["results"][0]["data"]]


def fetch_neo(environment):
    nodes = [
        normalize_neo_node(labels, props)
        for labels, props in _cypher(
            environment, "MATCH (n:POLYGRAPHVertex) RETURN labels(n), properties(n)"
        )
    ]
    edges = []
    for physical, props, source, target in _cypher(
        environment,
        "MATCH (a:POLYGRAPHVertex)-[r]->(b:POLYGRAPHVertex) RETURN type(r), properties(r), properties(a), properties(b)",
    ):
        if physical != sanitize_name(props["type"]):
            raise WorkflowError("Neo4J relationship type mismatch")
        edges.append(
            {
                "id": _neo_id(props),
                "type": props["type"],
                "source": props["source"],
                "from": _neo_id(source),
                "to": _neo_id(target),
                "properties": properties(props),
            }
        )
    return {"nodes": nodes, "edges": edges}


def fetch_arango(environment, manifest):
    if environment.owned_container("arango") is None:
        raise WorkflowError("Owned ArangoDB container is missing")
    endpoint = (
        environment.targets["targets"]["ArangoDB"]["endpoint"]
        + "/_db/"
        + environment.database
    )
    nodes, edges, handles = [], [], {}
    collections = {
        artifact["collection"]: artifact["kind"]
        for artifact in manifest["backends"]["ArangoDB"]["artifacts"]
        if artifact["kind"] in ("nodes", "proxies", "edges")
    }
    documents = {}
    for collection in collections:
        result = request(
            endpoint + "/_api/cursor",
            "root",
            environment.password,
            {
                "query": "FOR d IN @@collection RETURN d",
                "bindVars": {"@collection": collection},
                "batchSize": 1000,
            },
        )
        if result.get("error") or result.get("hasMore"):
            raise WorkflowError(
                "ArangoDB fixture read failed or exceeded 1000-row batch"
            )
        documents[collection] = result["result"]
        if collections[collection] != "edges":
            for document in result["result"]:
                node = normalize_arango_node(document)
                handles[document["_id"]] = node["id"]
                nodes.append(node)
    for collection, rows in documents.items():
        if collections[collection] != "edges":
            continue
        for row in rows:
            if collection != sanitize_name(row["type"]) or row["_key"] != str(
                unsigned(row["uuid"])
            ):
                raise WorkflowError("ArangoDB relationship collection/key mismatch")
            if row["_from"] not in handles or row["_to"] not in handles:
                raise WorkflowError(
                    "ArangoDB relationship references a missing physical endpoint"
                )
            edges.append(
                {
                    "id": unsigned(row["uuid"]),
                    "type": row["type"],
                    "source": row["source"],
                    "from": handles[row["_from"]],
                    "to": handles[row["_to"]],
                    "properties": properties(row),
                }
            )
    return {"nodes": nodes, "edges": edges}


def fetch_postgres(environment):
    inspected = environment.owned_container("pg")
    if inspected is None:
        raise WorkflowError("Owned PostgreSQL container is missing")

    def rows(table):
        query = f"SELECT COALESCE(json_agg(n ORDER BY n.id),'[]'::json) FROM public.{table} AS n"
        result = environment.docker(
            [
                "exec",
                inspected["Id"],
                "psql",
                "-X",
                "-U",
                "postgres",
                "-d",
                environment.database,
                "-v",
                "ON_ERROR_STOP=1",
                "-Atc",
                query,
            ]
        )
        return json.loads(result.stdout)

    nodes = [
        {
            "id": unsigned(row["id"]),
            "type": row["type"],
            "source": row["source"],
            "is_proxy": row["is_proxy"],
            "properties": row["properties"],
        }
        for row in rows("polygraph_nodes")
    ]
    by_id = {node["id"]: node for node in nodes}
    edges = []
    for row in rows("polygraph_edges"):
        source, target = unsigned(row["from_id"]), unsigned(row["to_id"])
        if (
            source not in by_id
            or target not in by_id
            or row["from_type"] != by_id[source]["type"]
            or row["to_type"] != by_id[target]["type"]
        ):
            raise WorkflowError("PostgreSQL relationship endpoint/type mismatch")
        edges.append(
            {
                "id": unsigned(row["id"]),
                "type": row["edge_type"],
                "source": row["source"],
                "from": source,
                "to": target,
                "properties": row["properties"],
            }
        )
    return {"nodes": nodes, "edges": edges}


def verify_stored_graph(environment, manifest, expected):
    actual = {
        "Neo4J": fetch_neo(environment),
        "ArangoDB": fetch_arango(environment, manifest),
        "PostgreSQL": fetch_postgres(environment),
    }
    report = {}
    for backend, data in actual.items():
        compare_records(expected[backend]["nodes"], data["nodes"], backend + ".nodes")
        compare_records(expected[backend]["edges"], data["edges"], backend + ".edges")
        ids = {node["id"] for node in data["nodes"]}
        if any(
            edge["from"] not in ids or edge["to"] not in ids for edge in data["edges"]
        ):
            raise WorkflowError(backend + ": missing live endpoint")
        report[backend] = {
            "nodes": sum(not row["is_proxy"] for row in data["nodes"]),
            "proxies": sum(row["is_proxy"] for row in data["nodes"]),
            "edges": len(data["edges"]),
            "node_types": sorted(
                {row["type"] for row in data["nodes"] if not row["is_proxy"]}
            ),
            "edge_types": sorted({row["type"] for row in data["edges"]}),
            "attributes_verified": True,
            "ids_types_endpoints_verified": True,
        }
    return report


def verify_fixture_bfs(environment, binary, domain, oracle, nodes):
    aliases = {}
    for identifier, node in nodes.items():
        key = json.loads(node["key"])
        if (
            key[:3] != ["polygraph.source.v1", oracle["namespace"], "vertex"]
            or key[4] != "int64"
        ):
            raise WorkflowError(
                "Fixture oracle needs exact int64 typed source identities"
            )
        alias = f"{key[3]}:{key[5]}"
        if alias in aliases:
            raise WorkflowError("Ambiguous fixture source alias")
        aliases[alias] = identifier
    report = []
    for number, root in enumerate(oracle["roots"]):
        direction = root["direction"]
        if direction not in ("directed", "undirected"):
            raise WorkflowError("Unsupported oracle direction")
        selector = {
            "namespace": oracle["namespace"],
            "type": root["type"],
            "idType": "int64",
            "id": root["id"],
        }
        expected = {
            aliases[alias]: depth
            for depth, layer in enumerate(root["layers"])
            for alias in layer
        }
        result_path = environment.output / f"bfs-oracle-{number}.csv"
        stage(
            environment,
            f"bfs-oracle-{number}",
            [
                binary,
                "--domain",
                domain,
                "--start-source",
                json.dumps(selector),
                "--max-depth",
                str(len(root["layers"]) - 1),
                "--direction",
                direction,
                "--no-events",
                "--no-logging",
                "--no-predecessors",
                "--output-file",
                result_path,
            ],
        )
        with result_path.open(newline="") as stream:
            records = list(csv.DictReader(stream, delimiter=";"))
        actual = {int(row["vertex_id"]): int(row["distance"]) for row in records}
        if len(records) != len(actual) or actual != expected:
            raise WorkflowError(
                f"Fixture BFS oracle {number} differs; inspect {result_path}"
            )
        report.append(
            {
                "root": selector,
                "global_id": aliases[f"{root['type']}:{root['id']}"],
                "direction": direction,
                "max_depth": len(root["layers"]) - 1,
                "vertices": len(actual),
                "verified": True,
                "path": str(result_path),
            }
        )
    return report


def run_movie_fixture(args):
    output = args.output_dir.resolve()
    if args.unique_output:
        output /= "run-" + secrets.token_hex(6)
    if output.exists():
        raise WorkflowError(
            "Live test requires a fresh output directory: " + str(output)
        )
    binary, bfs = args.binary.resolve(), args.bfs.resolve()
    images = {"neo": args.neo_image, "arango": args.arango_image, "pg": args.pg_image}
    reasons, prerequisites = check_prerequisites(binary, bfs, args.adapter, images)
    output.mkdir(parents=True, mode=0o700)
    report = {
        "state": "skipped" if reasons else "running",
        "adapter": args.adapter,
        "prerequisites": prerequisites,
        "output_dir": str(output),
        "cleanup": {"attempted": False},
    }
    if reasons:
        report["reasons"] = reasons
        private_json(output / "integration-result.json", report)
        print(json.dumps(report, indent=2))
        return SKIPPED
    before = None
    environment = None
    started = time.monotonic()
    with cancellation_scope():
        try:
            before = resource_snapshot()
            source = (
                args.graph_manifest or ROOT / "examples/movie-graph/domain.json"
            ).resolve()
            oracle_path = (
                args.oracle or ROOT / "examples/movie-graph/expected.json"
            ).resolve()
            document, oracle = (
                json.loads(source.read_text()),
                json.loads(oracle_path.read_text()),
            )
            if (
                args.workflow != "native"
                or document["ingestion"]["adapter"] != args.adapter
            ):
                raise WorkflowError(
                    "Movie fixture requires its linked adapter and native workflow"
                )
            environment = DockerEnvironment(
                output, "polygraph-live-" + secrets.token_hex(5), images=images
            )
            paths = {
                key: str((source.parent / value).resolve())
                for key, value in document["paths"].items()
            }
            paths.update(
                ingestionArtifacts=str(environment.artifacts),
                targetConfig=str(environment.target_path),
            )
            document["paths"] = paths
            document.pop("workspace", None)
            domain = output / "import-domain.json"
            private_json(domain, document)
            stage(
                environment, "prepare", [binary, "import", "--graph-manifest", domain]
            )
            manifest = json.loads((environment.artifacts / "manifest.json").read_text())
            if manifest["counts"] != oracle["counts"]:
                raise WorkflowError(
                    "Prepared fixture counts differ from its independent oracle"
                )
            nodes, edges, expected = normalized_graph(environment, manifest)
            environment.provision()
            stage(
                environment,
                "import",
                [
                    binary,
                    "import",
                    "--graph-manifest",
                    domain,
                    "--execute",
                    "--confirm-create",
                ],
            )
            stage(
                environment,
                "verify-live",
                [
                    binary,
                    "verify-live",
                    "--manifest",
                    environment.artifacts / "manifest.json",
                    "--target-config",
                    environment.target_path,
                ],
            )
            report["native_verification"] = json.loads(
                (environment.artifacts / "live-verification.json").read_text()
            )
            report["counts"] = manifest["counts"]
            report["backend_data"] = verify_stored_graph(
                environment, manifest, expected
            )
            schema, data = bootstrap_runtime(environment, manifest)
            document["paths"].update(
                schema=str(schema),
                data=str(data),
                backendConfig=str(environment.runtime_config_path),
            )
            runtime_domain = output / "runtime-domain.json"
            private_json(runtime_domain, document)
            report["bfs"] = verify_fixture_bfs(
                environment, bfs, runtime_domain, oracle, nodes
            )
            report["state"] = "verified"
        except (Exception, WorkflowInterrupted) as error:
            report.update(state="failed", error=str(error))
        finally:
            # Finish ownership-checked cleanup even if cancellation first arrives here.
            for sig in (signal.SIGINT, signal.SIGTERM):
                signal.signal(sig, signal.SIG_IGN)
            if environment is not None:
                report["cleanup"]["attempted"] = True
                try:
                    try:
                        environment.capture_logs()
                    finally:
                        environment.cleanup()
                    report["cleanup"].update(
                        state=environment.state["status"], owned_containers_removed=True
                    )
                except Exception as error:
                    report.update(state="failed", cleanup_error=str(error))
                    report["cleanup"]["state"] = "failed"
                try:
                    environment.runtime_config_path.unlink(missing_ok=True)
                except OSError as error:
                    report.update(state="failed", credential_cleanup_error=str(error))
            try:
                after = resource_snapshot()
                if before is None:
                    raise WorkflowError("Initial resource snapshot unavailable")
                assert_preserved(before, after)
                if environment is not None and any(
                    value["id"] in after["containers"]
                    for value in environment.state["containers"].values()
                ):
                    raise WorkflowError("Owned fixture containers remain after cleanup")
                report["preexisting_resources"] = {
                    "containers": len(before["containers"]),
                    "volumes": len(before["volumes"]),
                    "unchanged": True,
                }
            except Exception as error:
                report.update(state="failed", preservation_error=str(error))
            report["duration_seconds"] = time.monotonic() - started
            if report["state"] == "verified":
                report["state"] = "passed"
            private_json(output / "integration-result.json", report)
    print(json.dumps(report, indent=2))
    return 0 if report["state"] == "passed" else 1
