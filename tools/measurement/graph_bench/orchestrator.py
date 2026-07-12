from __future__ import annotations

import sys
from pathlib import Path
from typing import Iterable

from .backend_configs import write_backend_config
from .backends.native import ArangoNativeClient, Neo4jNativeClient, relationship_filter_from_schema
from .common import ensure_dir, repo_root, safe_file_part, utc_now_iso
from .config import BenchmarkConfig, ImplementationBackendPaths
from .csv_compare import compare_bfs_csv, read_bfs_csv
from .graph_data import GraphData, write_schema_with_source
from .metrics import MeasurementSink, RunRecord
from .subprocesses import CommandRunner

BACKEND_ALIASES = {
    "neo": "Neo4J",
    "neo4j": "Neo4J",
    "Neo4J": "Neo4J",
    "arango": "ArangoDB",
    "arangodb": "ArangoDB",
    "ArangoDB": "ArangoDB",
    "gpu": "GPU",
    "cugraph": "GPU",
    "GPU": "GPU",
}


def normalize_backends(values: Iterable[str]) -> list[str]:
    result: list[str] = []
    for value in values:
        key = value.strip()
        if not key:
            continue
        try:
            backend = BACKEND_ALIASES[key]
        except KeyError as exc:
            raise ValueError(f"Unknown backend '{value}', expected neo4j, arango, gpu") from exc
        if backend not in result:
            result.append(backend)
    return result


class BfsBenchmarkOrchestrator:
    def __init__(self, config: BenchmarkConfig, output_dir: str | Path, *, dry_run: bool = False, verbose: bool = True):
        self.config = config
        self.output_dir = ensure_dir(output_dir)
        self.graph = GraphData.load(config.paths.data)
        self.sink = MeasurementSink(self.output_dir)
        self.runner = CommandRunner(repo_root(), dry_run=dry_run, verbose=verbose)
        self.generated_dir = ensure_dir(self.output_dir / "generated")
        self.outputs_dir = ensure_dir(self.output_dir / "outputs")
        self.logs_dir = ensure_dir("logs")
        self.diffs_dir = ensure_dir(self.output_dir / "diffs")
        self.reference_dir = ensure_dir(self.output_dir / "reference")

    def flush(self) -> tuple[Path, Path, Path]:
        return self.sink.flush_all()

    def reference_output(self, start_uuid: str) -> Path:
        return self.reference_dir / f"reference_{safe_file_part(start_uuid)}.csv"

    def ensure_reference(self, start_uuid: str) -> Path:
        out = self.reference_output(start_uuid)
        if out.exists():
            return out
        with self.sink.measure(
            scenario="reference",
            system="tools/bfs.py",
            phase="bfs_query",
            start_uuid=start_uuid,
            output_csv=out,
            details={"data": str(self.config.paths.data)},
        ) as record:
            self.runner.run([
                sys.executable,
                self.config.paths.reference_bfs_script,
                "--input-json", self.config.paths.data,
                "--output-csv", out,
                "--start-uuid", start_uuid,
            ], stderr_path=self.logs_dir / "reference.log")
            if not self.runner.dry_run:
                record.vertex_count = len(read_bfs_csv(out))
        return out

    def _compare_to_reference(self, *, expected: Path, actual: Path, scenario: str, system: str, start_uuid: str, repetition: int) -> None:
        diff = self.diffs_dir / f"{scenario}_{system}_{safe_file_part(start_uuid)}_rep{repetition:03d}.csv"
        cmp = compare_bfs_csv(expected, actual, diff)
        self.sink.add(record=RunRecord(
            timestamp=utc_now_iso(),
            scenario=scenario,
            system=system,
            phase="compare_reference",
            repetition=repetition,
            start_uuid=start_uuid,
            seconds=0.0,
            status="ok" if cmp.same else "failed",
            output_csv=str(actual),
            expected_csv=str(expected),
            same_as_reference=cmp.same,
            vertex_count=cmp.actual_count,
            details={
                "expected_count": cmp.expected_count,
                "actual_count": cmp.actual_count,
                "missing_count": cmp.missing_count,
                "extra_count": cmp.extra_count,
                "wrong_distance_count": cmp.wrong_distance_count,
                "diff_csv": cmp.diff_csv,
            },
        ))

    def run_native_full(self, start_uuids: list[str], repetitions: int, backends: list[str], *, compare: bool = True, reload_each_repetition: bool = False) -> None:
        """Measure only real native DB BFS implementations.

        There is intentionally no native full-GPU scenario here. GPU can still be measured
        through graph_db in implementation-full mode with config/backends.gpu.json.
        """
        if "GPU" in backends:
            print("Ignoring native backend 'gpu': there is no standalone native full-GPU BFS scenario.", file=sys.stderr)
            backends = [b for b in backends if b != "GPU"]

        if self.runner.dry_run:
            for backend in backends:
                system = "neo4j_native" if backend == "Neo4J" else "arango_native"
                for rep in range(1, repetitions + 1):
                    if reload_each_repetition or rep == 1:
                        with self.sink.measure(scenario="native_full", system=system, phase="load", repetition=rep, details={"mode": "full_replica"}):
                            print(f"+ load full graph into {backend}", file=sys.stderr)
                    for start_uuid in start_uuids:
                        out = self.outputs_dir / f"native_{backend}_{safe_file_part(start_uuid)}_rep{rep:03d}.csv"
                        with self.sink.measure(scenario="native_full", system=system, phase="bfs_query", repetition=rep, start_uuid=start_uuid, output_csv=out):
                            print(f"+ run {system} BFS start={start_uuid} max_depth={self.config.bfs.max_depth}", file=sys.stderr)
            return

        relation_filter = relationship_filter_from_schema(self.config.paths.schema)

        if "Neo4J" in backends:
            client = Neo4jNativeClient(
                self.config.neo4j.http_endpoint,
                self.config.neo4j.user,
                self.config.neo4j.password,
                batch_size=self.config.batch_size,
            )
            try:
                loaded = False
                for rep in range(1, repetitions + 1):
                    if reload_each_repetition or not loaded:
                        with self.sink.measure(scenario="native_full", system="neo4j_native", phase="load", repetition=rep, details={"mode": "full_replica"}):
                            client.load_full_graph(self.graph)
                        loaded = True
                    for start_uuid in start_uuids:
                        expected = self.ensure_reference(start_uuid) if compare else None
                        out = self.outputs_dir / f"native_neo4j_{safe_file_part(start_uuid)}_rep{rep:03d}.csv"
                        with self.sink.measure(scenario="native_full", system="neo4j_native", phase="bfs_query", repetition=rep, start_uuid=start_uuid, output_csv=out) as record:
                            record.vertex_count = client.bfs(start_uuid, relation_filter, self.config.bfs.max_depth, out)
                        if compare and expected:
                            self._compare_to_reference(expected=expected, actual=out, scenario="native_full", system="neo4j_native", start_uuid=start_uuid, repetition=rep)
            finally:
                client.close()

        if "ArangoDB" in backends:
            client = ArangoNativeClient(
                self.config.arango.base_url,
                self.config.arango.user,
                self.config.arango.password,
                self.config.arango.database,
                batch_size=self.config.batch_size,
            )
            try:
                loaded = False
                for rep in range(1, repetitions + 1):
                    if reload_each_repetition or not loaded:
                        with self.sink.measure(scenario="native_full", system="arango_native", phase="load", repetition=rep, details={"mode": "full_replica"}):
                            client.load_full_graph(self.graph)
                        loaded = True
                    for start_uuid in start_uuids:
                        expected = self.ensure_reference(start_uuid) if compare else None
                        out = self.outputs_dir / f"native_arango_{safe_file_part(start_uuid)}_rep{rep:03d}.csv"
                        with self.sink.measure(scenario="native_full", system="arango_native", phase="bfs_query", repetition=rep, start_uuid=start_uuid, output_csv=out) as record:
                            record.vertex_count = client.bfs(self.graph, start_uuid, self.config.bfs.max_depth, out)
                        if compare and expected:
                            self._compare_to_reference(expected=expected, actual=out, scenario="native_full", system="arango_native", start_uuid=start_uuid, repetition=rep)
            finally:
                client.close()

    def _reset_databases(self, scenario: str, system: str, repetition: int) -> None:
        """Clear Neo4j and ArangoDB before a loader run so scenarios do not contaminate each other."""
        if self.runner.dry_run:
            with self.sink.measure(scenario=scenario, system=system, phase="reset", repetition=repetition):
                print("+ reset Neo4j and ArangoDB", file=sys.stderr)
            return
        with self.sink.measure(scenario=scenario, system=system, phase="reset", repetition=repetition):
            neo = Neo4jNativeClient(
                self.config.neo4j.http_endpoint,
                self.config.neo4j.user,
                self.config.neo4j.password,
                batch_size=self.config.batch_size,
            )
            arango = ArangoNativeClient(
                self.config.arango.base_url,
                self.config.arango.user,
                self.config.arango.password,
                self.config.arango.database,
                batch_size=self.config.batch_size,
            )
            try:
                neo.reset()
                arango.reset_database()
            finally:
                neo.close()
                arango.close()

    def _run_loader(self, data_path: Path, scenario: str, system: str, repetition: int) -> None:
        self._reset_databases(scenario, system, repetition)
        with self.sink.measure(scenario=scenario, system=system, phase="load", repetition=repetition, details={"data": str(data_path)}):
            self.runner.run([
                sys.executable,
                self.config.paths.loader_script,
                data_path,
                "--arango-url", self.config.arango.base_url,
                "--arango-user", self.config.arango.user,
                "--arango-password", self.config.arango.password,
                "--arango-db", self.config.arango.database,
                "--neo4j-uri", self.config.neo4j.bolt_uri,
                "--neo4j-user", self.config.neo4j.user,
                "--neo4j-password", self.config.neo4j.password,
            ], stderr_path=self.logs_dir / f"loader_{scenario}_{system}.log")

    def _graph_db_command(self, *, schema: Path, run_id: str, repetition: int, data: Path, backend_config: Path, start_uuid: str, system: str) -> list[str | Path]:
        start_type = self.graph.start_type(start_uuid)

        event_log_file = (
                self.logs_dir
                / safe_file_part(run_id)
                / safe_file_part(system)
                / f"rep{repetition:03d}.events.jsonl"
        )
        event_log_file.parent.mkdir(parents=True, exist_ok=True)

        cmd: list[str | Path] = [
            self.config.paths.graph_db_binary,
            "--logical-graph-schema", schema,
            "--data", data,
            "--log-file", event_log_file,
            "--backend-config", backend_config,
            "--start-uuid", start_uuid,
            "--start-type", start_type,
            "--max-depth", str(self.config.bfs.max_depth),
            "--direction", self.config.bfs.direction,
        ]
        if self.config.bfs.allowed_edge_types:
            cmd += ["--allowed-edge-types", self.config.bfs.allowed_edge_types]
        if self.config.bfs.allowed_target_node_types:
            cmd += ["--allowed-target-node-types", self.config.bfs.allowed_target_node_types]
        if self.config.bfs.allow_native_pushdown:
            cmd.append("--allow-native-pushdown")
        if self.config.bfs.no_partial_acceleration:
            cmd.append("--no-partial-acceleration")
        if self.config.bfs.no_predecessors:
            cmd.append("--no-predecessors")
        return cmd

    def _run_graph_db_bfs(self, *, scenario: str, run_id: str, system: str, repetition: int, start_uuid: str, schema: Path, data: Path, backend_config: Path, compare: bool) -> None:
        expected = self.ensure_reference(start_uuid) if compare else None
        out = self.outputs_dir / f"{scenario}_{system}_{safe_file_part(start_uuid)}_rep{repetition:03d}.csv"
        with self.sink.measure(
            scenario=scenario,
            system=system,
            phase="bfs_query",
            repetition=repetition,
            start_uuid=start_uuid,
            output_csv=out,
            expected_csv=expected or "",
            details={"schema": str(schema), "data": str(data), "backend_config": str(backend_config)},
        ) as record:

            self.runner.run(
                self._graph_db_command(schema=schema, run_id=run_id, repetition=repetition, data=data, backend_config=backend_config, start_uuid=start_uuid, system=system),
                stdout_path=out,
                stderr_path=self.logs_dir / f"graph_db_{scenario}_{system}.log",
                env={"CPGDB_PROFILE": "1", "CPGDB_PROFILE_LAYERS": "1"},
            )
            if not self.runner.dry_run:
                record.vertex_count = len(read_bfs_csv(out))
        if compare and expected and not self.runner.dry_run:
            self._compare_to_reference(expected=expected, actual=out, scenario=scenario, system=system, start_uuid=start_uuid, repetition=repetition)

    def _implementation_paths_for(self, backend: str) -> tuple[Path, Path, Path]:
        configured = self.config.paths.implementation_full.get(backend, ImplementationBackendPaths())

        schema_path = configured.schema
        if schema_path is None or not schema_path.exists():
            schema_path = self.generated_dir / f"full_{backend}.schema.json"
            write_schema_with_source(self.config.paths.schema, schema_path, backend)

        data_path = configured.data
        if data_path is None or not data_path.exists():
            data_path = self.generated_dir / f"full_{backend}.data.json"
            self.graph.write_data_with_source(data_path, backend)

        backend_config = configured.backend_config
        if backend_config is None or not backend_config.exists():
            backend_config = write_backend_config(self.config, self.generated_dir / f"backend_{backend}.json", [backend])

        return schema_path, data_path, backend_config

    def run_implementation_full(self, start_uuids: list[str], run_id: str, repetitions: int, backends: list[str], *, compare: bool = True, reload_each_repetition: bool = False) -> None:
        """Measure graph_db with all vertices/edges assigned to one backend at a time.

        This uses the configured per-backend schema/backend-config files when available:
        - config/backends.neo.json + storage_layer/*.neo.graph-shape.json
        - config/backends.arango.json + storage_layer/*.arango.graph-shape.json
        - config/backends.gpu.json + storage_layer/*.gpu.graph-shape.json
        """
        for backend in backends:
            schema_path, data_path, backend_config = self._implementation_paths_for(backend)

            loaded = False
            for rep in range(1, repetitions + 1):
                if backend in {"Neo4J", "ArangoDB"} and (reload_each_repetition or not loaded):
                    self._run_loader(data_path, "implementation_full", f"graph_db_{backend}", rep)
                    loaded = True
                for start_uuid in start_uuids:
                    self._run_graph_db_bfs(
                        scenario="implementation_full",
                        run_id=run_id,
                        system=f"graph_db_{backend}",
                        repetition=rep,
                        start_uuid=start_uuid,
                        schema=schema_path,
                        data=data_path,
                        backend_config=backend_config,
                        compare=compare,
                    )

    def run_distributed(self, start_uuids: list[str], run_id: str, repetitions: int, *, compare: bool = True, reload_each_repetition: bool = False) -> None:
        backend_config = self.config.paths.backend_config or write_backend_config(
            self.config,
            self.generated_dir / "backend_distributed.json",
            ["Neo4J", "ArangoDB", "GPU"],
        )
        loaded = False
        for rep in range(1, repetitions + 1):
            if reload_each_repetition or not loaded:
                self._run_loader(self.config.paths.data, "distributed", "graph_db_distributed", rep)
                loaded = True
            for start_uuid in start_uuids:
                self._run_graph_db_bfs(
                    scenario="distributed",
                    run_id=run_id,
                    system="graph_db_distributed",
                    repetition=rep,
                    start_uuid=start_uuid,
                    schema=self.config.paths.schema,
                    data=self.config.paths.data,
                    backend_config=backend_config,
                    compare=compare,
                )

    def run_all(self, start_uuids: list[str], run_id: str, repetitions: int, *, native_backends: list[str], implementation_backends: list[str], compare: bool = True, reload_each_repetition: bool = False) -> None:
        self.run_native_full(start_uuids, repetitions, native_backends, compare=compare, reload_each_repetition=reload_each_repetition)
        self.run_implementation_full(start_uuids, run_id, repetitions, implementation_backends, compare=compare, reload_each_repetition=reload_each_repetition)
        self.run_distributed(start_uuids, run_id, repetitions, compare=compare, reload_each_repetition=reload_each_repetition)
