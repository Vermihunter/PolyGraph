#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Opt-in relational acceptance on uniquely owned PostgreSQL/Neo4j/ArangoDB.

python3 tools/ingestion/relational_live.py --run --unique-output \
  --binary build/cpu-tests/bin/polygraph_bulk_import --bfs build/cpu-tests/bin/polygraph_bfs \
  --attach build/cpu-tests/bin/polygraph_attach_postgres \
  --probe build/cpu-tests/bin/polygraph_postgres_relational_acceptance_test \
  --output-dir build/relational-live

Requires cached images. An explicit run fails on missing prerequisites; never skips.
Existing services/volumes are inspected for preservation and are never imported into.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.ingestion.docker_environment import (
    DockerEnvironment, WorkflowError, WorkflowInterrupted, private_json,
)
from tools.ingestion.docker_load import cancellation_scope, stage
from tools.ingestion.live_graph import (
    assert_preserved, check_prerequisites, resource_snapshot,
)

FIXTURE = ROOT / "tests/fixtures/relational_postgresql"
BACKENDS = ["Neo4J", "ArangoDB", "PostgreSQL"]
TABLES = ["app.person", "geo.place", "app.document", "refs.gateway", "refs.sensor",
          "app.reads", "app.opens", "refs.next", "refs.returns", "app.sentinel"]


def assert_distances(rows, expected):
    actual = {int(row["vertex_id"]): int(row["distance"]) for row in rows}
    if len(actual) != len(rows) or actual != expected:
        raise WorkflowError(f"BFS distances differ: {actual} != {expected}")


def normalize_dump(text):
    # New pg_dump versions randomize only these client-side guard tokens.
    return "\n".join(line for line in text.splitlines()
                     if not line.startswith(("\\restrict ", "\\unrestrict ")))


def pg(environment, database, sql, *, expect_failure=False):
    owned = environment.owned_container("pg")
    if owned is None:
        raise WorkflowError("Owned PostgreSQL container disappeared")
    result = subprocess.run(
        ["docker", "exec", "-i", owned["Id"], "psql", "-X", "-v", "ON_ERROR_STOP=1",
         "-U", "postgres", "-d", database, "-At"],
        input=sql, capture_output=True, text=True, env=environment.env,
    )
    if expect_failure:
        if result.returncode == 0 or "foreign key" not in result.stderr:
            raise WorkflowError("Expected native constraint rejection")
    elif result.returncode:
        raise WorkflowError("PostgreSQL fixture query failed: " + result.stderr)
    return result.stdout.strip()


def dump(environment, database, *, schema_only=False):
    owned = environment.owned_container("pg")
    if owned is None:
        raise WorkflowError("Owned PostgreSQL container disappeared")
    result = subprocess.run(
        ["docker", "exec", owned["Id"], "pg_dump", "-U", "postgres", "-d", database,
         "--no-owner", "--no-privileges", *(["--schema-only"] if schema_only else [])],
        capture_output=True, text=True, check=True, env=environment.env,
    )
    return normalize_dump(result.stdout)


def native_rows(environment, database):
    result = {}
    for table in TABLES:
        schema, name = table.split(".")
        result[table] = json.loads(pg(environment, database,
            f'SELECT COALESCE(jsonb_agg(r ORDER BY r::text),\'[]\'::jsonb) '
            f'FROM (SELECT to_jsonb(t) AS r FROM "{schema}"."{name}" t) s;'))
    return result


def expected_failure(environment, name, argv, *, diagnostic):
    try:
        stage(environment, name, argv)
    except WorkflowError as error:
        stderr = (environment.output / "logs" / (name + ".stderr.log")).read_text()
        if diagnostic not in stderr:
            raise WorkflowError(name + " failed for an unexpected reason") from error
        return
    raise WorkflowError(name + " unexpectedly succeeded")


def runtime(environment, directory, database, catalog, backends, *, reader=False):
    directory.mkdir()
    config = json.loads(environment.runtime_config(backends).read_text())
    postgres = next(entry for entry in config["backends"] if entry["id"] == "PostgreSQL")
    postgres.update(relationalMapping=str(FIXTURE / "mapping.json"), relationalIdentity=str(catalog))
    postgres["connection"]["database"] = database
    if reader:
        postgres["connection"]["credentials"]["username"] = "relational_reader"
    config_path = directory / "backends.secret.json"
    private_json(config_path, config)
    graph = directory / "graph.jsonl"
    graph.write_text(json.dumps({"kind": "metadata", "idRouting": backends}) + "\n")
    scenario = directory / "scenario.json"
    private_json(scenario, {"schema": str(FIXTURE / "schema.json"), "mapping": str(FIXTURE / "mapping.json"),
                           "catalog": str(catalog), "backends": str(config_path), "data": str(graph),
                           "oracle": str(FIXTURE / "expected.json")})
    return scenario


def verify_runtime(environment, args, scenario, name):
    # The C++ probe checks every source's rich/minimal/streamed adjacency and
    # BFS predecessors against the independent literal fixture oracle.
    stage(environment, name + "-interfaces", [args.probe, scenario])
    config = json.loads(scenario.read_text())
    oracle = json.loads((FIXTURE / "expected.json").read_text())
    # Use the C++ source resolver's IDs written by the probe; never derive IDs from PKs.
    ids = json.loads((scenario.parent / "resolved-ids.json").read_text())
    count = 0
    for case in oracle["cases"]:
        expected = {ids[alias]: depth for depth, layer in enumerate(case["layers"]) for alias in layer}
        for streamed in (False, True):
            output = scenario.parent / (case["name"] + ("-stream.csv" if streamed else "-pred.csv"))
            argv = [args.bfs, "--logical-graph-schema", config["schema"], "--data", config["data"],
                    "--backend-config", config["backends"], "--start-source", json.dumps(oracle["nodes"][case["root"]]),
                    "--source-index", config["catalog"], "--direction", case["direction"],
                    "--max-depth", str(case.get("maxDepth", -1)), "--neighbor-batch-size", "2",
                    "--no-logging", "--no-events", "--output-file", output]
            if case.get("edgeTypes"):
                argv += ["--allowed-edge-types", ",".join(case["edgeTypes"])]
            if case.get("targetTypes"):
                argv += ["--allowed-target-node-types", ",".join(case["targetTypes"])]
            if streamed:
                argv += ["--no-predecessors", "--streaming-candidate-batch-size", "1"]
            stage(environment, name + "-" + output.stem, argv)
            with output.open(newline="") as stream:
                assert_distances(list(csv.DictReader(stream, delimiter=";")), expected)
            count += 1
    return count


def run_acceptance(args):
    output = args.output_dir.resolve()
    if args.unique_output:
        output /= "run-" + secrets.token_hex(6)
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    report = {"state": "failed", "cleanup": {"attempted": False}}
    environment, before = None, None
    with cancellation_scope():
        try:
            images = {"neo": args.neo_image, "arango": args.arango_image, "pg": args.pg_image}
            reasons, details = check_prerequisites(args.binary, args.bfs, "source-jsonl", images)
            for label, path in (("attachment", args.attach), ("interface probe", args.probe)):
                if not path.is_file() or not os.access(path, os.X_OK):
                    reasons.append(label + " executable unavailable: " + str(path))
            report["prerequisites"] = details
            if reasons:
                raise WorkflowError("; ".join(reasons))
            before = resource_snapshot()
            environment = DockerEnvironment(output / "services", "polygraph-relational-" + secrets.token_hex(5), images=images)
            stage(environment, "prepare", [args.binary, "prepare", "--adapter", "source-jsonl",
                "--input", FIXTURE / "graph.jsonl", "--placement-config", FIXTURE / "placement.json",
                "--partition-file", FIXTURE / "assignments.jsonl", "--partition-map", FIXTURE / "partition-map.json",
                "--relational-schema", FIXTURE / "schema.json", "--relational-mapping", FIXTURE / "mapping.json",
                "--output-dir", environment.artifacts])
            stage(environment, "strict-artifacts", [args.binary, "verify", "--manifest", environment.artifacts / "manifest.json", "--strict"])
            environment.provision()
            attached = environment.database + "-attached"
            pg(environment, environment.database, f'CREATE DATABASE "{attached}";')
            ddl = (FIXTURE / "schema.sql").read_text()
            references = (FIXTURE / "references.sql").read_text()
            pg(environment, attached, ddl + references + (FIXTURE / "seed.sql").read_text())
            pg(environment, attached, f"CREATE ROLE relational_reader LOGIN PASSWORD '{environment.password}';"
               "ALTER ROLE relational_reader SET default_transaction_read_only=on;"
               f'REVOKE TEMPORARY ON DATABASE "{attached}" FROM PUBLIC;'
               "GRANT USAGE ON SCHEMA app,geo,refs TO relational_reader;"
               "GRANT SELECT ON ALL TABLES IN SCHEMA app,geo,refs TO relational_reader;")
            before_attach = dump(environment, attached)
            expected_rows = native_rows(environment, attached)
            environment.env.update(PGHOST="127.0.0.1", PGPORT=str(environment.ports["pg"]), PGDATABASE=attached,
                                   PGUSER="relational_reader", PGPASSWORD=environment.password)
            catalog = output / "attached-catalog"
            stage(environment, "attach", [args.attach, "--schema", FIXTURE / "schema.json", "--mapping", FIXTURE / "mapping.json", "--output-dir", catalog])
            attached_scenario = runtime(environment, output / "attached-runtime", attached, catalog / "manifest.json", ["PostgreSQL"], reader=True)
            report["attached_bfs_runs"] = verify_runtime(environment, args, attached_scenario, "attached")
            stage(environment, "refresh", [args.attach, "--schema", FIXTURE / "schema.json",
                "--mapping", FIXTURE / "mapping.json", "--previous", catalog / "manifest.json",
                "--output-dir", output / "refreshed-catalog"])
            refresh_scenario = runtime(environment, output / "refreshed-runtime", attached,
                output / "refreshed-catalog/manifest.json", ["PostgreSQL"], reader=True)
            stage(environment, "refresh-interfaces", [args.probe, refresh_scenario])
            if json.loads((refresh_scenario.parent / "resolved-ids.json").read_text()) != json.loads((attached_scenario.parent / "resolved-ids.json").read_text()):
                raise WorkflowError("Refresh changed stable typed source IDs")
            report["refresh_ids_stable"] = True
            if dump(environment, attached) != before_attach:
                raise WorkflowError("Read-only attachment/traversal changed application schema or data")
            report["read_only_attach_unchanged"] = True
            # A native FK violation fails atomically and cannot silently create an edge.
            pg(environment, attached, "UPDATE app.document SET place='missing' WHERE place='7';", expect_failure=True)
            if dump(environment, attached) != before_attach:
                raise WorkflowError("Invalid native FK assignment changed data")

            database = environment.database
            pg(environment, database, ddl + references)
            schema_before = dump(environment, database, schema_only=True)
            targets = environment.targets
            targets["targets"]["PostgreSQL"].update(relationalMapping=str(FIXTURE / "mapping.json"),
                                                     relationalIdentityOutput=str(output / "imported-catalog"))
            private_json(environment.target_path, targets)
            # Missing remote reference rows fail all-backend preflight before any import.
            pg(environment, database, "DELETE FROM refs.sensor;")
            expected_failure(environment, "missing-reference", [args.binary, "execute", "--manifest", environment.artifacts / "manifest.json", "--target-config", environment.target_path, "--confirm-create"], diagnostic='"refs"."sensor"')
            if pg(environment, database, "SELECT count(*) FROM app.person;") != "0":
                raise WorkflowError("Failed reference preflight wrote application nodes")
            pg(environment, database, "INSERT INTO refs.sensor VALUES ('7','sensor seven');")
            stage(environment, "execute", [args.binary, "execute", "--manifest", environment.artifacts / "manifest.json", "--target-config", environment.target_path, "--confirm-create", "--concurrency", "2"])
            stage(environment, "verify-live", [args.binary, "verify-live", "--manifest", environment.artifacts / "manifest.json", "--target-config", environment.target_path])
            manifest = json.loads((environment.artifacts / "manifest.json").read_text())
            publication = json.loads((output / "imported-catalog/ingestion.json").read_text())
            if manifest["partitioning"]["partition_cut_relationships"] != 4 or manifest["partitioning"]["backend_cross_edges"] != 3 or publication["counts"]["proxies"] != 2:
                raise WorkflowError("Partition cuts or relational proxy metadata differ")
            report["placement"] = {"partition_cuts": 4, "backend_cross_edges": 3, "relational_proxies": 2}
            expected_rows["refs.next"] = []  # Both endpoints are remote; PG needs no replica.
            if native_rows(environment, database) != expected_rows:
                raise WorkflowError("Imported native PK/property/FK/junction/reference/sentinel rows differ")
            if dump(environment, database, schema_only=True) != schema_before:
                raise WorkflowError("Native import altered application schema")
            report["native_schema_and_rows_verified"] = True
            environment.env.update(PGDATABASE=database, PGUSER="postgres")
            imported_scenario = runtime(environment, output / "imported-runtime", database, output / "imported-catalog/manifest.json", BACKENDS)
            report["mixed_bfs_runs"] = verify_runtime(environment, args, imported_scenario, "mixed")

            # Deliberate fingerprint mismatch must fail before any traversal result.
            wrong = json.loads((FIXTURE / "mapping.json").read_text())
            wrong["nodes"][0]["namespace"] = "wrong-generation"
            private_json(output / "wrong-mapping.json", wrong)
            config = json.loads((imported_scenario.parent / "backends.secret.json").read_text())
            next(b for b in config["backends"] if b["id"] == "PostgreSQL")["relationalMapping"] = str(output / "wrong-mapping.json")
            private_json(output / "wrong-backends.secret.json", config)
            oracle = json.loads((FIXTURE / "expected.json").read_text())
            common = [args.bfs, "--logical-graph-schema", FIXTURE / "schema.json", "--data", imported_scenario.parent / "graph.jsonl", "--no-events", "--no-logging", "--source-index", output / "imported-catalog/manifest.json", "--start-source", json.dumps(oracle["nodes"]["p7"])]
            expected_failure(environment, "generation-mismatch", [*common, "--backend-config", output / "wrong-backends.secret.json"], diagnostic="different mapping")
            expected_failure(environment, "unknown-filter", [*common, "--backend-config", imported_scenario.parent / "backends.secret.json", "--allowed-edge-types", "UNKNOWN"], diagnostic="unknown edge type")
            report["negative_cases"] = ["invalid-native-fk", "missing-remote-reference", "mapping-generation-mismatch", "unknown-logical-filter"]
            report["state"] = "passed"
        except (Exception, WorkflowInterrupted) as error:
            report.update(state="failed", error=str(error))
        finally:
            for sig in (signal.SIGINT, signal.SIGTERM):
                signal.signal(sig, signal.SIG_IGN)
            if environment is not None:
                report["cleanup"]["attempted"] = True
                try:
                    try:
                        environment.capture_logs()
                    finally:
                        environment.cleanup()
                    report["cleanup"]["state"] = environment.state["status"]
                except Exception as error:
                    report.update(state="failed", cleanup_error=str(error))
            for secret in output.glob("**/*secret.json"):
                secret.unlink(missing_ok=True)
            if before is not None:
                try:
                    assert_preserved(before, resource_snapshot())
                    report["existing_resources_preserved"] = True
                except Exception as error:
                    report.update(state="failed", preservation_error=str(error))
            private_json(output / "integration-result.json", report)
    print(json.dumps(report, indent=2))
    return 0 if report["state"] == "passed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run", action="store_true")
    for name in ("binary", "bfs", "attach", "probe", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--unique-output", action="store_true")
    parser.add_argument("--neo-image", default="polygraph-neo4j:5.26-enterprise")
    parser.add_argument("--arango-image", default="polygraph-arangodb:3.12")
    parser.add_argument("--pg-image", default="postgres:17")
    args = parser.parse_args()
    if not args.run:
        parser.error("--run is required; no services are touched by default")
    for name in ("binary", "bfs", "attach", "probe"):
        setattr(args, name, getattr(args, name).resolve())
    return run_acceptance(args)


if __name__ == "__main__":
    raise SystemExit(main())
