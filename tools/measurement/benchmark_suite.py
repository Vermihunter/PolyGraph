#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Sequence

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.graphio.domain import DomainConfig, load_domain_manifest, resolve_domain_node_id
from tools.measurement.graph_bench.config import (
    ArangoConfig,
    BenchmarkConfig,
    BfsConfig,
    Neo4jConfig,
    PathConfig,
)
from tools.measurement.graph_bench.graph_data import read_start_uuids
from tools.measurement.graph_bench.orchestrator import BfsBenchmarkOrchestrator, normalize_backends
from tools.measurement.graph_bench.plotting import write_plots
from tools.measurement.graph_bench.services import wait_http
from tools.measurement.graph_bench.subprocesses import CommandRunner


@dataclass(frozen=True)
class PrepareSpec:
    command: tuple[str, ...] = ()
    when_missing: bool = True


@dataclass(frozen=True)
class DatasetSpec:
    name: str
    domain: Path
    compose_instance: str | None = None
    compose_project: str | None = None
    start_uuids_file: Path | None = None
    start_uuids: tuple[str, ...] = ()
    prepare: PrepareSpec | None = None


@dataclass(frozen=True)
class BuildSpec:
    enabled: bool = True
    preset: str = "gpu-bfs"


@dataclass(frozen=True)
class ServicesSpec:
    start: bool = True
    reset_volumes: bool = False
    timeout_seconds: int = 180


@dataclass(frozen=True)
class ScenarioSpec:
    distributed: bool = True
    implementation_full: tuple[str, ...] = ("Neo4J", "ArangoDB", "GPU")
    native: tuple[str, ...] = ("Neo4J", "ArangoDB")
    load_distributed: bool = True
    reload_each_repetition: bool = False


@dataclass(frozen=True)
class SuiteConfig:
    name: str
    output_dir: Path
    repetitions: int
    start_uuid_count: int
    compare_with_reference: bool
    fail_on_mismatch: bool
    reconstruct_logs: bool
    batch_size: int
    graph_db_binary: Path | None
    docker_compose: Path
    build: BuildSpec
    services: ServicesSpec
    bfs: BfsConfig
    scenarios: ScenarioSpec
    datasets: tuple[DatasetSpec, ...]


def _object(root: dict[str, Any], name: str) -> dict[str, Any]:
    value = root.get(name, {})
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError(f"'{name}' must be a JSON object")
    return value


def _repo_path(value: str | Path, *, root: Path = ROOT) -> Path:
    candidate = Path(value).expanduser()
    return candidate.resolve() if candidate.is_absolute() else (root / candidate).resolve()


def _string_list(value: Any, *, field_name: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if isinstance(value, str):
        return tuple(item.strip() for item in value.split(",") if item.strip())
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ValueError(f"'{field_name}' must be a string or array of strings")
    return tuple(value)


def load_suite_config(path: str | Path) -> SuiteConfig:
    config_path = Path(path).expanduser().resolve()
    raw = json.loads(config_path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("Benchmark configuration root must be a JSON object")

    build_raw = _object(raw, "build")
    services_raw = _object(raw, "services")
    bfs_raw = _object(raw, "bfs")
    scenarios_raw = _object(raw, "scenarios")

    datasets_raw = raw.get("datasets")
    if not isinstance(datasets_raw, list) or not datasets_raw:
        raise ValueError("'datasets' must contain at least one dataset object")

    datasets: list[DatasetSpec] = []
    seen_names: set[str] = set()
    for index, item in enumerate(datasets_raw):
        if not isinstance(item, dict):
            raise ValueError(f"datasets[{index}] must be an object")
        name = str(item.get("name", "")).strip()
        if not name:
            raise ValueError(f"datasets[{index}].name is required")
        if name in seen_names:
            raise ValueError(f"Duplicate dataset name: {name}")
        seen_names.add(name)
        if "domain" not in item:
            raise ValueError(f"datasets[{index}].domain is required")

        prepare: PrepareSpec | None = None
        prepare_raw = item.get("prepare")
        if prepare_raw is not None:
            if not isinstance(prepare_raw, dict):
                raise ValueError(f"datasets[{index}].prepare must be an object")
            command = prepare_raw.get("command", [])
            if not isinstance(command, list) or not all(isinstance(token, str) for token in command):
                raise ValueError(f"datasets[{index}].prepare.command must be an array of strings")
            prepare = PrepareSpec(
                command=tuple(command),
                when_missing=bool(prepare_raw.get("whenMissing", True)),
            )

        starts_file = item.get("startUuidsFile")
        compose_instance = (
            str(item["composeInstance"]).strip()
            if item.get("composeInstance")
            else None
        )
        compose_project = (
            str(item["composeProject"]).strip()
            if item.get("composeProject")
            else None
        )
        if compose_instance and compose_project:
            raise ValueError(
                f"datasets[{index}] cannot define both composeInstance and composeProject"
            )
        if compose_instance and not re.fullmatch(
            r"sf[0-9]+(?:_[0-9]+)?/[a-z0-9][a-z0-9-]*", compose_instance
        ):
            raise ValueError(
                f"datasets[{index}].composeInstance must look like "
                "'sf0_1/schema-based'"
            )
        if compose_project and not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", compose_project):
            raise ValueError(
                f"datasets[{index}].composeProject must contain only lowercase "
                "letters, digits, '-' or '_' and start with a letter or digit"
            )
        datasets.append(
            DatasetSpec(
                name=name,
                domain=_repo_path(str(item["domain"])),
                compose_instance=compose_instance,
                compose_project=compose_project,
                start_uuids_file=_repo_path(str(starts_file)) if starts_file else None,
                start_uuids=_string_list(item.get("startUuids"), field_name=f"datasets[{index}].startUuids"),
                prepare=prepare,
            )
        )

    instance_selection = [dataset.compose_instance is not None for dataset in datasets]
    project_selection = [dataset.compose_project is not None for dataset in datasets]
    if any(instance_selection) and not all(instance_selection):
        raise ValueError(
            "Either every dataset must define composeInstance or none of them may define it"
        )
    if any(project_selection) and not all(project_selection):
        raise ValueError(
            "Either every dataset must define composeProject or none of them may define it"
        )

    repetitions = int(raw.get("repetitions", 3))
    start_count = int(raw.get("startUuidCount", 5))
    if repetitions < 1:
        raise ValueError("repetitions must be at least 1")
    if start_count < 1:
        raise ValueError("startUuidCount must be at least 1")

    direction = str(bfs_raw.get("direction", "undirected"))
    if direction not in {"directed", "undirected", "both"}:
        raise ValueError("bfs.direction must be directed, undirected, or both")

    neighbor_batch_size_raw = bfs_raw.get("neighborBatchSize")
    neighbor_batch_size = (
        None if neighbor_batch_size_raw in (None, "") else int(neighbor_batch_size_raw)
    )
    if neighbor_batch_size is not None and neighbor_batch_size <= 0:
        raise ValueError("bfs.neighborBatchSize must be greater than zero")

    neo4j_max_concurrent_requests = int(bfs_raw.get("maxConcurrentNeo4jRequests", 4))
    arango_max_concurrent_requests = int(bfs_raw.get("maxConcurrentArangoRequests", 4))
    backend_request_retries = int(bfs_raw.get("backendRequestRetries", 2))
    backend_retry_delay_ms = int(bfs_raw.get("backendRetryDelayMs", 100))
    if neo4j_max_concurrent_requests <= 0:
        raise ValueError("bfs.maxConcurrentNeo4jRequests must be greater than zero")
    if arango_max_concurrent_requests <= 0:
        raise ValueError("bfs.maxConcurrentArangoRequests must be greater than zero")
    if backend_request_retries < 0:
        raise ValueError("bfs.backendRequestRetries must be zero or greater")
    if backend_retry_delay_ms < 0:
        raise ValueError("bfs.backendRetryDelayMs must be zero or greater")
    if direction != "undirected" and scenarios_raw.get("native", ["Neo4J", "ArangoDB"]):
        raise ValueError("Native comparison currently supports only bfs.direction='undirected'")

    load_distributed = bool(scenarios_raw.get("loadDistributed", True))
    reload_each_repetition = bool(scenarios_raw.get("reloadEachRepetition", False))
    if not load_distributed and reload_each_repetition:
        raise ValueError(
            "scenarios.reloadEachRepetition cannot be true when "
            "scenarios.loadDistributed is false"
        )

    output_dir = _repo_path(str(raw.get("outputDir", f"measurements/{raw.get('name', 'bfs-suite')}")))
    binary_raw = raw.get("graphDbBinary")

    return SuiteConfig(
        name=str(raw.get("name", config_path.stem)),
        output_dir=output_dir,
        repetitions=repetitions,
        start_uuid_count=start_count,
        compare_with_reference=bool(raw.get("compareWithReference", True)),
        fail_on_mismatch=bool(raw.get("failOnMismatch", True)),
        reconstruct_logs=bool(raw.get("reconstructLogs", True)),
        batch_size=int(raw.get("batchSize", 5000)),
        graph_db_binary=_repo_path(str(binary_raw)) if binary_raw else None,
        docker_compose=_repo_path(str(raw.get("dockerCompose", "storage_layer/docker-compose.yml"))),
        build=BuildSpec(
            enabled=bool(build_raw.get("enabled", True)),
            preset=str(build_raw.get("preset", "gpu-bfs")),
        ),
        services=ServicesSpec(
            start=bool(services_raw.get("start", True)),
            reset_volumes=bool(services_raw.get("resetVolumes", False)),
            timeout_seconds=int(services_raw.get("timeoutSeconds", 180)),
        ),
        bfs=BfsConfig(
            max_depth=int(bfs_raw.get("maxDepth", 7)),
            direction=direction,
            neighbor_batch_size=neighbor_batch_size,
            neo4j_max_concurrent_requests=neo4j_max_concurrent_requests,
            arango_max_concurrent_requests=arango_max_concurrent_requests,
            backend_request_retries=backend_request_retries,
            backend_retry_delay_ms=backend_retry_delay_ms,
            allowed_edge_types=str(bfs_raw.get("allowedEdgeTypes", "")),
            allowed_target_node_types=str(bfs_raw.get("allowedTargetNodeTypes", "")),
            allow_native_pushdown=bool(bfs_raw.get("allowNativePushdown", False)),
            no_partial_acceleration=bool(bfs_raw.get("noPartialAcceleration", False)),
            no_predecessors=bool(bfs_raw.get("noPredecessors", True)),
        ),
        scenarios=ScenarioSpec(
            distributed=bool(scenarios_raw.get("distributed", True)),
            implementation_full=tuple(
                normalize_backends(
                    _string_list(
                        scenarios_raw.get("implementationFull", ["Neo4J", "ArangoDB", "GPU"]),
                        field_name="scenarios.implementationFull",
                    )
                )
            ),
            native=tuple(
                normalize_backends(
                    _string_list(
                        scenarios_raw.get("native", ["Neo4J", "ArangoDB"]),
                        field_name="scenarios.native",
                    )
                )
            ),
            load_distributed=load_distributed,
            reload_each_repetition=reload_each_repetition,
        ),
        datasets=tuple(datasets),
    )


def _expand_command(command: Sequence[str]) -> list[str]:
    replacements = {
        "{python}": sys.executable,
        "{root}": str(ROOT),
    }
    return [replacements.get(token, token.replace("{root}", str(ROOT))) for token in command]


def _safe_name(value: str) -> str:
    rendered = re.sub(r"[^A-Za-z0-9._-]", "_", value)
    if not rendered or rendered in {".", ".."}:
        raise ValueError(f"Invalid benchmark name: {value!r}")
    return rendered


def _polygraph_event_log_root(dataset_output: Path) -> Path:
    """Return the directory containing only PolyGraph/C++ event logs.

    Command traces, reference stderr logs, and native-backend measurements live
    elsewhere under ``logs`` and are not input for event reconstruction.
    """
    return dataset_output / "logs" / "events"


def _has_polygraph_event_logs(event_log_root: Path) -> bool:
    """Whether a PolyGraph execution produced at least one non-empty event log."""
    if not event_log_root.is_dir():
        return False
    return any(
        path.is_file() and path.stat().st_size > 0
        for path in event_log_root.rglob("*.events.jsonl")
    )


def _read_backend_connection_config(path: Path) -> tuple[Neo4jConfig, ArangoConfig]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    neo = Neo4jConfig()
    arango = ArangoConfig()
    for entry in raw.get("backends", []):
        backend_id = str(entry.get("id", ""))
        connection = entry.get("connection", {})
        credentials = connection.get("credentials", {})
        if backend_id == "Neo4J":
            endpoint = str(connection.get("endpoint", neo.http_endpoint))
            neo = Neo4jConfig(
                http_endpoint=endpoint,
                bolt_uri="bolt://localhost:7687",
                user=str(credentials.get("username", neo.user)),
                password=str(credentials.get("password", neo.password)),
            )
        elif backend_id == "ArangoDB":
            endpoint = str(connection.get("endpoint", ""))
            base_url = endpoint.split("/_db/", 1)[0] if "/_db/" in endpoint else arango.base_url
            arango = ArangoConfig(
                base_url=base_url,
                user=str(credentials.get("username", arango.user)),
                password=str(credentials.get("password", arango.password)),
                database=str(connection.get("database", arango.database)),
            )
    return neo, arango


def _binary_for(config: SuiteConfig) -> Path:
    if config.graph_db_binary:
        return config.graph_db_binary
    return ROOT / "build" / config.build.preset / "bin" / "bfs"


def _benchmark_config(config: SuiteConfig, domain: DomainConfig) -> BenchmarkConfig:
    neo, arango = _read_backend_connection_config(domain.paths.backends)
    return BenchmarkConfig(
        paths=PathConfig(
            schema=domain.paths.schema,
            data=domain.paths.data,
            backend_config=domain.paths.backends,
            graph_db_binary=_binary_for(config),
            loader_script=ROOT / "tools" / "load_graph.py",
            reference_bfs_script=ROOT / "tools" / "algorithms" / "bfs.py",
            docker_compose=config.docker_compose,
            implementation_full={},
            source_data=domain.paths.source_data,
        ),
        neo4j=neo,
        arango=arango,
        bfs=config.bfs,
        batch_size=config.batch_size,
    )


def _dataset_starts(dataset: DatasetSpec, domain: DomainConfig, count: int) -> list[str]:
    starts = read_start_uuids(dataset.start_uuids, dataset.start_uuids_file)
    if not starts:
        starts = [domain.start_uuid]
    if len(starts) < count:
        raise ValueError(
            f"Dataset '{dataset.name}' provides only {len(starts)} start ID(s), "
            f"but startUuidCount={count}"
        )
    return [str(resolve_domain_node_id(domain, value)) for value in starts[:count]]



def _dataset_files_ready(dataset: DatasetSpec) -> bool:
    if not dataset.domain.is_file():
        return False
    if dataset.start_uuids_file is not None and not dataset.start_uuids_file.is_file():
        return False
    return True

def _write_aggregate_csv(output: Path, dataset_csvs: list[tuple[str, Path]]) -> Path:
    output.parent.mkdir(parents=True, exist_ok=True)
    fieldnames: list[str] | None = None
    with output.open("w", encoding="utf-8", newline="") as handle:
        writer: csv.DictWriter[str] | None = None
        for dataset_name, source in dataset_csvs:
            with source.open("r", encoding="utf-8", newline="") as source_handle:
                reader = csv.DictReader(source_handle)
                if reader.fieldnames is None:
                    continue
                if fieldnames is None:
                    fieldnames = ["dataset", *reader.fieldnames]
                    writer = csv.DictWriter(handle, fieldnames=fieldnames)
                    writer.writeheader()
                assert writer is not None
                for row in reader:
                    writer.writerow({"dataset": dataset_name, **row})
    if fieldnames is None:
        output.write_text("dataset\n", encoding="utf-8")
    return output



def _jsonable_suite(config: SuiteConfig) -> dict[str, Any]:
    return json.loads(json.dumps(asdict(config), default=str))


def _resume_config_mismatches(previous: Any, current: Any, path: str = "suite") -> list[str]:
    """Compare fields present in both configs; old runs may lack newer tuning fields."""
    if isinstance(previous, dict) and isinstance(current, dict):
        mismatches: list[str] = []
        for key in sorted(previous.keys() & current.keys()):
            mismatches.extend(
                _resume_config_mismatches(previous[key], current[key], f"{path}.{key}")
            )
        return mismatches
    if isinstance(previous, list) and isinstance(current, list):
        return [] if previous == current else [
            f"{path}: previous={previous!r}, current={current!r}"
        ]
    return [] if previous == current else [
        f"{path}: previous={previous!r}, current={current!r}"
    ]


def _validate_resume_config(suite_output: Path, run_id: str, config: SuiteConfig) -> None:
    effective_path = suite_output / "effective-config.json"
    if not effective_path.is_file():
        raise ValueError(
            f"Cannot resume benchmark without {effective_path}; use a new --run-id instead"
        )
    previous = json.loads(effective_path.read_text(encoding="utf-8"))
    previous_run_id = str(previous.get("runId", ""))
    if previous_run_id and previous_run_id != run_id:
        raise ValueError(
            f"Resume run ID mismatch: existing={previous_run_id!r}, requested={run_id!r}"
        )
    mismatches = _resume_config_mismatches(
        previous.get("suite", {}), _jsonable_suite(config)
    )
    if mismatches:
        rendered = "\n  ".join(mismatches[:10])
        suffix = "" if len(mismatches) <= 10 else f"\n  ... and {len(mismatches) - 10} more"
        raise ValueError(
            "Resume configuration differs from the existing run:\n  " + rendered + suffix
        )



def _compose_command(config: SuiteConfig, project_name: str | None = None) -> list[str | Path]:
    command: list[str | Path] = ["docker", "compose", "-f", config.docker_compose]
    if project_name:
        command.extend(["--project-name", project_name])
    return command


def _instance_command(instance_key: str, action: str, *arguments: str) -> list[str | Path]:
    scale_factor, distribution = instance_key.split("/", 1)
    return [
        sys.executable,
        ROOT / "tools" / "polygraph.py",
        "instance",
        scale_factor,
        distribution,
        action,
        *arguments,
    ]


def _start_services_for_project(
    config: SuiteConfig,
    runner: CommandRunner,
    domain: DomainConfig,
    *,
    project_name: str | None,
    reset_volumes: bool,
    dry_run: bool,
) -> None:
    compose = _compose_command(config, project_name)
    if reset_volumes:
        runner.run([*compose, "down", "--volumes", "--remove-orphans"])
    runner.run([*compose, "up", "-d"])
    if dry_run:
        return

    benchmark_config = _benchmark_config(config, domain)
    wait_http(
        "http://localhost:7474",
        timeout_seconds=config.services.timeout_seconds,
    )
    wait_http(
        f"{benchmark_config.arango.base_url.rstrip('/')}/_api/version",
        timeout_seconds=config.services.timeout_seconds,
        auth=(benchmark_config.arango.user, benchmark_config.arango.password),
    )


def run_suite(
    config: SuiteConfig,
    *,
    run_id: str | None = None,
    output_dir: str | Path | None = None,
    dry_run: bool = False,
    skip_build: bool = False,
    skip_services: bool = False,
    resume: bool = False,
) -> Path:
    effective_run_id = _safe_name(run_id or config.name)
    suite_output = Path(output_dir).expanduser().resolve() if output_dir else config.output_dir / effective_run_id
    output_nonempty = suite_output.exists() and any(suite_output.iterdir())
    if output_nonempty and not resume:
        raise ValueError(
            f"Benchmark output directory is not empty: {suite_output}; pass --resume to continue it"
        )
    if resume and not output_nonempty:
        raise ValueError(f"Cannot resume missing or empty benchmark output: {suite_output}")
    suite_output.mkdir(parents=True, exist_ok=True)
    if resume:
        _validate_resume_config(suite_output, effective_run_id, config)
        if config.scenarios.load_distributed:
            raise ValueError(
                "--resume requires scenarios.loadDistributed=false so the existing resolved graph and "
                "the existing resolved integer IDs are reused safely"
            )
        if config.scenarios.implementation_full or config.scenarios.native:
            raise ValueError(
                "--resume currently supports the distributed scenario only; set "
                "scenarios.implementationFull and scenarios.native to []"
            )
        print(
            f"[resume] Continuing {suite_output}; completed work will be skipped",
            file=sys.stderr,
        )

    runner = CommandRunner(
        ROOT,
        dry_run=dry_run,
        verbose=True,
        log_path=suite_output / "logs" / "setup-commands.jsonl",
    )
    effective = {
        "runId": effective_run_id,
        "suite": asdict(config),
    }
    # Keep the original checkpoint config during continuation.
    if not resume:
        (suite_output / "effective-config.json").write_text(
            json.dumps(effective, indent=2, default=str) + "\n", encoding="utf-8"
        )


    for dataset in config.datasets:
        should_prepare = not resume and dataset.prepare and (
            not dataset.prepare.when_missing or not _dataset_files_ready(dataset)
        )
        if should_prepare:
            if not dataset.prepare.command:
                raise ValueError(f"Dataset '{dataset.name}' is missing and has no prepare command")
            runner.run(_expand_command(dataset.prepare.command))
        if not dry_run and not _dataset_files_ready(dataset):
            raise FileNotFoundError(
                f"Dataset '{dataset.name}' is incomplete after preparation: "
                f"domain={dataset.domain}, starts={dataset.start_uuids_file}"
            )
        if not dry_run:
            prepared_domain = load_domain_manifest(dataset.domain)
            for required in (
                prepared_domain.paths.schema,
                prepared_domain.paths.data,
                prepared_domain.paths.backends,
            ):
                if not required.is_file():
                    raise FileNotFoundError(
                        f"Dataset '{dataset.name}' manifest references a missing file: {required}"
                    )

    if config.build.enabled and not skip_build and not resume:
        runner.run([sys.executable, ROOT / "tools" / "polygraph.py", "build", "--preset", config.build.preset])

    per_dataset_instances = any(dataset.compose_instance for dataset in config.datasets)
    per_dataset_projects = any(dataset.compose_project for dataset in config.datasets)
    if (
        config.services.start
        and not skip_services
        and not resume
        and not per_dataset_instances
        and not per_dataset_projects
    ):
        first_domain = load_domain_manifest(config.datasets[0].domain)
        _start_services_for_project(
            config,
            runner,
            first_domain,
            project_name=None,
            reset_volumes=config.services.reset_volumes,
            dry_run=dry_run,
        )

    dataset_csvs: list[tuple[str, Path]] = []
    dataset_summaries: list[dict[str, Any]] = []
    comparison_failures = 0
    failed_records = 0
    skipped_completed = 0
    active_compose_instance: str | None = None
    initialized_instances: set[str] = set()
    active_compose_project: str | None = None
    initialized_projects: set[str] = set()

    for dataset in config.datasets:
        if dry_run and not dataset.domain.is_file():
            # The prepare command is printed in dry-run mode, but there is no manifest to inspect.
            print(f"Skipping dry-run scenario expansion for missing manifest: {dataset.domain}", file=sys.stderr)
            continue

        domain = load_domain_manifest(dataset.domain)

        if (
            config.services.start
            and not skip_services
            and not resume
            and dataset.compose_instance
            and dataset.compose_instance != active_compose_instance
        ):
            if active_compose_instance:
                runner.run(_instance_command(active_compose_instance, "down"))
            if (
                config.services.reset_volumes
                and dataset.compose_instance not in initialized_instances
            ):
                runner.run(_instance_command(dataset.compose_instance, "destroy", "--yes"))
            runner.run(_instance_command(dataset.compose_instance, "up"))
            initialized_instances.add(dataset.compose_instance)
            active_compose_instance = dataset.compose_instance

        if (
            config.services.start
            and not skip_services
            and not resume
            and dataset.compose_project
            and dataset.compose_project != active_compose_project
        ):
            if active_compose_project:
                runner.run(
                    [
                        *_compose_command(config, active_compose_project),
                        "down",
                        "--remove-orphans",
                    ]
                )
            _start_services_for_project(
                config,
                runner,
                domain,
                project_name=dataset.compose_project,
                reset_volumes=(
                    config.services.reset_volumes
                    and dataset.compose_project not in initialized_projects
                ),
                dry_run=dry_run,
            )
            initialized_projects.add(dataset.compose_project)
            active_compose_project = dataset.compose_project

        starts = _dataset_starts(dataset, domain, config.start_uuid_count)
        dataset_output = suite_output / "datasets" / _safe_name(dataset.name)
        orchestrator = BfsBenchmarkOrchestrator(
            _benchmark_config(config, domain),
            dataset_output,
            dry_run=dry_run,
            resume=resume,
        )
        try:
            for start_uuid in starts:
                if config.compare_with_reference:
                    orchestrator.ensure_reference(start_uuid)

            if config.scenarios.distributed:
                orchestrator.run_distributed(
                    starts,
                    f"{effective_run_id}-{dataset.name}",
                    config.repetitions,
                    compare=config.compare_with_reference,
                    load_before_run=config.scenarios.load_distributed,
                    reload_each_repetition=config.scenarios.reload_each_repetition,
                )

            if config.scenarios.implementation_full:
                orchestrator.run_implementation_full(
                    starts,
                    f"{effective_run_id}-{dataset.name}",
                    config.repetitions,
                    list(config.scenarios.implementation_full),
                    compare=config.compare_with_reference,
                    reload_each_repetition=config.scenarios.reload_each_repetition,
                )

            if config.scenarios.native:
                orchestrator.run_native_full(
                    starts,
                    config.repetitions,
                    list(config.scenarios.native),
                    compare=config.compare_with_reference,
                    reload_each_repetition=config.scenarios.reload_each_repetition,
                )
        finally:
            csv_path, jsonl_path, summary_path = orchestrator.flush()
            dataset_csvs.append((dataset.name, csv_path))
            for plot in write_plots(csv_path, dataset_output / "plots"):
                print(f"Plot: {plot}")
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            comparison_failures += int(summary.get("comparison_failures", 0))
            failed_records += int(summary.get("failed_count", 0))
            skipped_completed += orchestrator.skipped_completed
            dataset_summaries.append(
                {
                    "name": dataset.name,
                    "domain": str(dataset.domain),
                    "composeInstance": dataset.compose_instance,
                    "composeProject": dataset.compose_project,
                    "startUuids": starts,
                    "measurements": str(csv_path),
                    "events": str(jsonl_path),
                    "summary": str(summary_path),
                    "comparisonFailures": int(summary.get("comparison_failures", 0)),
                    "failedRecords": int(summary.get("failed_count", 0)),
                    "skippedCompleted": orchestrator.skipped_completed,
                }
            )


        # Reconstruction is deliberately outside the ``finally`` block. If loading,
        # ID resolution, or the C++ BFS fails before producing event logs, the
        # original benchmark exception must remain visible instead of being masked
        # by a secondary "No valid logs found" error.
        if config.reconstruct_logs and not dry_run:
            event_log_root = _polygraph_event_log_root(dataset_output)
            if _has_polygraph_event_logs(event_log_root):
                runner.run(
                    [
                        sys.executable,
                        ROOT / "tools" / "measurement" / "log_reconstruction.py",
                        event_log_root,
                        "--out-dir",
                        dataset_output / "log-reconstruction",
                        "--inline-plotly",
                    ]
                )
            else:
                print(
                    "Skipping event-log reconstruction: no PolyGraph .events.jsonl "
                    f"files were produced under {event_log_root}",
                    file=sys.stderr,
                )

    aggregate_csv = _write_aggregate_csv(suite_output / "measurements.csv", dataset_csvs)
    status = "ok"
    if failed_records:
        status = "failed"
    elif comparison_failures:
        status = "mismatch"
    summary = {
        "runId": effective_run_id,
        "status": status,
        "repetitions": config.repetitions,
        "startUuidCount": config.start_uuid_count,
        "comparisonFailures": comparison_failures,
        "failedRecords": failed_records,
        "resumed": resume,
        "skippedCompleted": skipped_completed,
        "datasets": dataset_summaries,
        "aggregateMeasurements": str(aggregate_csv),
    }
    summary_path = suite_output / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print(f"Benchmark output: {suite_output}")
    print(f"Summary:          {summary_path}")
    print(f"Measurements:     {aggregate_csv}")

    if failed_records:
        raise RuntimeError(f"Benchmark contains {failed_records} failed record(s); see {summary_path}")
    if comparison_failures and config.fail_on_mismatch:
        raise RuntimeError(
            f"Benchmark contains {comparison_failures} result mismatch(es); see {summary_path}"
        )
    return suite_output


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run the complete PolyGraph BFS benchmark suite from one JSON configuration."
    )
    parser.add_argument("config")
    parser.add_argument("--run-id")
    parser.add_argument("--output-dir")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-services", action="store_true")
    parser.add_argument(
        "--resume",
        action="store_true",
        help="Continue the same non-empty output directory and rerun only failed or missing work.",
    )
    args = parser.parse_args(argv)

    try:
        run_suite(
            load_suite_config(args.config),
            run_id=args.run_id,
            output_dir=args.output_dir,
            dry_run=args.dry_run,
            skip_build=args.skip_build,
            skip_services=args.skip_services,
            resume=args.resume,
        )
        return 0
    except (FileNotFoundError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
