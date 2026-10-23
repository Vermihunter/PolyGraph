#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.graphio.domain import (
    discover_domain_manifests,
    find_first_node,
    generate_sample_graph,
    load_domain_manifest,
    materialize_graph_jsonl,
    record_native_id_resolution,
    resolve_domain,
    resolve_domain_node_id,
    select_roots,
    validate_graph,
    write_backend_config_for_sources,
    write_domain_manifest,
    write_roots_csv,
)
from tools.graphio.loaders import (
    BackendConnection,
    load_graph,
    read_backend_connections,
    sync_cache_residency,
)
from tools.graphio.reader import open_graph_reader
from tools.graphio.cache_planner import load_cache_config, plan_graph_caches
from tools.graphio.gpu_storage_planner import load_gpu_distribution_config, plan_gpu_storage
from tools.ldbc_snb.convert import convert_dataset
from tools.ldbc_snb.statistics import analyze_dataset as analyze_ldbc_dataset
from tools.ldbc_snb.statistics import print_report as print_ldbc_statistics_report
from tools.ldbc_snb.statistics import write_report_outputs as write_ldbc_statistics_outputs

DATA_ROOT = ROOT / "data"
STORAGE_ROOT = ROOT / "storage_layer"
COMPOSE_FILE = STORAGE_ROOT / "docker-compose.yml"
COMPOSE_ENV_FILE = STORAGE_ROOT / ".env"
COMPOSE_INSTANCES_FILE = STORAGE_ROOT / "environments.json"
UNIVERSITY_SCHEMA = DATA_ROOT / "university" / "schema" / "sample-university-domain.graph-shape.json"
DATAGEN_DOCKERFILE = ROOT / "tools" / "ldbc_snb" / "Dockerfile.datagen"


@dataclass(frozen=True)
class ComposeInstance:
    key: str
    scale_factor: str
    distribution: str
    project: str
    compose_file: Path
    env_file: Path
    timeout_seconds: int
    domain: str | None
    variables: dict[str, str]

    @property
    def neo4j_http_port(self) -> int:
        return int(self.variables["NEO4J_HTTP_PORT"])

    @property
    def neo4j_bolt_port(self) -> int:
        return int(self.variables["NEO4J_BOLT_PORT"])

    @property
    def arango_port(self) -> int:
        return int(self.variables["ARANGO_PORT"])

    @property
    def postgresql_port(self) -> int:
        return int(self.variables["POSTGRES_PORT"])

    @property
    def neo4j_username(self) -> str:
        return self.variables["NEO4J_USERNAME"]

    @property
    def neo4j_password(self) -> str:
        return self.variables["NEO4J_PASSWORD"]

    @property
    def arango_password(self) -> str:
        return self.variables["ARANGO_ROOT_PASSWORD"]

    @property
    def postgresql_username(self) -> str:
        return self.variables["POSTGRES_USER"]

    @property
    def postgresql_password(self) -> str:
        return self.variables["POSTGRES_PASSWORD"]

    @property
    def postgresql_database(self) -> str:
        return self.variables["POSTGRES_DB"]


def step(message: str) -> None:
    print(f"\n[polygraph] {message}", flush=True)


def _write_domain_observability_config(output_root: Path, backends_path: Path) -> Path:
    """Materialize observability config with the generated CUDA device set."""
    template = ROOT / "config" / "observability" / "observability.json"
    raw = json.loads(template.read_text(encoding="utf-8"))
    backend_root = json.loads(backends_path.read_text(encoding="utf-8"))
    devices = {
        int(entry.get("device_id", 0))
        for entry in backend_root.get("backends", [])
        if entry.get("enabled", True) and str(entry.get("kind")) == "CuGraph"
    }

    # Newly prepared domains keep authoritative GPU runtime entries in the
    # separate gpu-storage-plan.json instead of coupling them to cache config.
    storage_plan_value = backend_root.get("gpuStoragePlan")
    if storage_plan_value:
        storage_plan_path = Path(str(storage_plan_value)).expanduser()
        if not storage_plan_path.is_absolute():
            storage_plan_path = backends_path.parent / storage_plan_path
        if storage_plan_path.is_file():
            storage_plan = json.loads(storage_plan_path.read_text(encoding="utf-8"))
            devices.update(
                int(entry.get("device_id", 0))
                for entry in storage_plan.get("runtimeBackends", [])
                if entry.get("enabled", True) and str(entry.get("kind")) == "CuGraph"
            )

    device_indices = sorted(devices)
    gpu = raw.setdefault("metrics", {}).setdefault("collectors", {}).setdefault("gpu", {})
    gpu["enabled"] = bool(device_indices)
    gpu["device_indices"] = device_indices
    gpu.pop("device_index", None)
    output = output_root / "observability.json"
    output.write_text(json.dumps(raw, indent=2) + "\n", encoding="utf-8")
    return output


def run(command: Sequence[str | os.PathLike[str]], *, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    rendered = [str(item) for item in command]
    print("+", " ".join(rendered), flush=True)
    subprocess.run(rendered, cwd=cwd, env=env, check=True)


def _read_dotenv(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        key, separator, value = line.partition("=")
        if not separator:
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {'"', "'"}:
            value = value[1:-1]
        values[key.strip()] = value
    return values


def _repo_path(value: str | os.PathLike[str]) -> Path:
    candidate = Path(value).expanduser()
    return candidate.resolve() if candidate.is_absolute() else (ROOT / candidate).resolve()


def _slug(value: str) -> str:
    return re.sub(r"^-+|-+$", "", re.sub(r"[^a-z0-9]+", "-", value.lower()))


def _normalize_scale_factor(value: str) -> str:
    """Canonicalize user-facing SF spellings to the on-disk sfX_Y form.

    Examples: 1, 1.0, sf1 and sf1_0 all become sf1_0; 0.1 becomes sf0_1.
    """
    normalized = value.strip().lower()
    if normalized.startswith("sf"):
        normalized = normalized[2:]
    normalized = normalized.replace("_", ".")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", normalized):
        raise ValueError(f"Invalid scale factor: {value!r}")
    whole, dot, fraction = normalized.partition(".")
    whole = str(int(whole))
    if not dot:
        fraction = "0"
    else:
        fraction = fraction.rstrip("0") or "0"
    return f"sf{whole}_{fraction}"


def _compose_instance_key(scale_factor: str, distribution: str) -> str:
    scale = _normalize_scale_factor(scale_factor)
    distribution_slug = _slug(distribution)
    if not distribution_slug:
        raise ValueError(f"Invalid distribution: {distribution!r}")
    return f"{scale}/{distribution_slug}"


def compose_instance_choices(
    registry_path: Path = COMPOSE_INSTANCES_FILE,
) -> tuple[list[str], list[str]]:
    """Return registered scale factors and distributions in registry order."""
    if not registry_path.is_file():
        return [], []
    try:
        raw = json.loads(registry_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return [], []
    instances = raw.get("instances", {}) if isinstance(raw, dict) else {}
    if not isinstance(instances, dict):
        return [], []
    scales: list[str] = []
    distributions: list[str] = []
    for key in instances:
        scale, separator, distribution = str(key).partition("/")
        if not separator:
            continue
        if scale not in scales:
            scales.append(scale)
        if distribution not in distributions:
            distributions.append(distribution)
    return scales, distributions


def resolve_compose_instance(
    scale_factor: str,
    distribution: str,
    *,
    registry_path: Path = COMPOSE_INSTANCES_FILE,
) -> ComposeInstance:
    if not registry_path.is_file():
        raise FileNotFoundError(f"Compose instance registry does not exist: {registry_path}")
    raw = json.loads(registry_path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError(f"Compose instance registry root must be an object: {registry_path}")
    defaults = raw.get("defaults", {})
    instances = raw.get("instances", {})
    if not isinstance(defaults, dict) or not isinstance(instances, dict):
        raise ValueError("environments.json must contain object fields 'defaults' and 'instances'")

    key = _compose_instance_key(scale_factor, distribution)
    entry = instances.get(key)
    if entry is None:
        # Instances are intentionally synthesizable: Docker Compose project names
        # namespace volumes, so a new SF can be created lazily without editing
        # environments.json first. Registry entries remain useful for overrides.
        scale, distribution_slug = key.split("/", 1)
        entry = {
            "project": f"polygraph-{scale.replace('_', '-')}-{distribution_slug}",
            "domain": f"data/ldbc-snb/{scale}/{distribution_slug}/domain.json",
        }
    elif not isinstance(entry, dict):
        raise ValueError(f"Compose instance {key!r} must be a JSON object")

    compose_file = _repo_path(str(entry.get("composeFile", defaults.get("composeFile", COMPOSE_FILE))))
    env_file = _repo_path(str(entry.get("envFile", defaults.get("envFile", COMPOSE_ENV_FILE))))
    if not compose_file.is_file():
        raise FileNotFoundError(f"Compose file does not exist: {compose_file}")
    if not env_file.is_file():
        raise FileNotFoundError(f"Compose env file does not exist: {env_file}")

    default_variables = defaults.get("variables", {})
    entry_variables = entry.get("variables", {})
    if not isinstance(default_variables, dict) or not isinstance(entry_variables, dict):
        raise ValueError("Compose instance 'variables' fields must be JSON objects")

    scale = key.split("/", 1)[0]
    distribution_slug = key.split("/", 1)[1]
    variables = {
        **_read_dotenv(env_file),
        **{str(name): str(value) for name, value in default_variables.items()},
        **{str(name): str(value) for name, value in entry_variables.items()},
        "POLYGRAPH_SCALE_FACTOR": scale,
        "POLYGRAPH_DISTRIBUTION": distribution_slug,
    }
    # Preserve compatibility with instance registries/env files created before
    # PostgreSQL became part of the repository storage stack. Compose uses the
    # same defaults when these variables are omitted.
    variables.setdefault("POSTGRES_PORT", "5432")
    variables.setdefault("POSTGRES_USER", "polygraph")
    variables.setdefault("POSTGRES_PASSWORD", "polygraph")
    variables.setdefault("POSTGRES_DB", "polygraph")
    required_variables = (
        "NEO4J_HTTP_PORT",
        "NEO4J_BOLT_PORT",
        "NEO4J_USERNAME",
        "NEO4J_PASSWORD",
        "ARANGO_PORT",
        "ARANGO_ROOT_PASSWORD",
        "POSTGRES_PORT",
        "POSTGRES_USER",
        "POSTGRES_PASSWORD",
        "POSTGRES_DB",
    )
    missing = [name for name in required_variables if not variables.get(name)]
    if missing:
        raise ValueError(f"Compose instance {key!r} is missing variables: {', '.join(missing)}")
    for port_name in ("NEO4J_HTTP_PORT", "NEO4J_BOLT_PORT", "ARANGO_PORT", "POSTGRES_PORT"):
        port = int(variables[port_name])
        if not 1 <= port <= 65535:
            raise ValueError(f"Invalid {port_name} for {key}: {port}")

    project = str(
        entry.get("project") or f"polygraph-{scale.replace('_', '-')}-{distribution_slug}"
    )
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", project):
        raise ValueError(f"Invalid Compose project name for {key}: {project!r}")
    timeout_seconds = int(entry.get("timeoutSeconds", defaults.get("timeoutSeconds", 600)))
    if timeout_seconds <= 0:
        raise ValueError(f"timeoutSeconds must be positive for {key}")

    domain_raw = entry.get("domain")
    return ComposeInstance(
        key=key,
        scale_factor=scale,
        distribution=distribution_slug,
        project=project,
        compose_file=compose_file,
        env_file=env_file,
        timeout_seconds=timeout_seconds,
        domain=str(domain_raw) if domain_raw else None,
        variables=variables,
    )


def _compose_command(instance: ComposeInstance) -> list[str | os.PathLike[str]]:
    return [
        "docker",
        "compose",
        "--env-file",
        instance.env_file,
        "-f",
        instance.compose_file,
        "--project-name",
        instance.project,
    ]


def _compose_environment(instance: ComposeInstance) -> dict[str, str]:
    return {**os.environ, **instance.variables}


def run_compose(instance: ComposeInstance, *arguments: str | os.PathLike[str]) -> None:
    run([*_compose_command(instance), *arguments], env=_compose_environment(instance))


def ensure_observability_network() -> None:
    """Create the shared storage/observability network on first use."""
    inspected = subprocess.run(
        ["docker", "network", "inspect", "polygraph-observability"],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if inspected.returncode != 0:
        run(["docker", "network", "create", "polygraph-observability"])


def stop_other_polygraph_instances(instance: ComposeInstance) -> None:
    """Stop other SF storage containers while preserving their volumes.

    Only containers carrying PolyGraph's scale-factor label are touched, so the
    independent Redis cache and observability stack are not stopped.
    """
    completed = subprocess.run(
        [
            "docker", "ps",
            "--filter", "label=polygraph.scale-factor",
            "--format", '{{.ID}}\t{{.Label "com.docker.compose.project"}}',
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    )
    ids: list[str] = []
    projects: set[str] = set()
    for raw in completed.stdout.splitlines():
        container_id, separator, project = raw.partition("\t")
        if not separator or not container_id or project == instance.project:
            continue
        ids.append(container_id)
        if project:
            projects.add(project)
    if ids:
        step("Stopping active storage instance(s): " + ", ".join(sorted(projects)))
        run(["docker", "stop", *ids])


def _instance_connection_overrides(
    instance: ComposeInstance,
    backend_config: str | Path,
) -> dict[str, BackendConnection]:
    """Bind a prepared domain to the selected Compose instance ports.

    Domain backend configs intentionally remain portable/default-oriented.  An
    ``instance ... load`` must not accidentally talk to a legacy container that
    happens to own 7474/7687/8529, so the selected instance is authoritative for
    host ports and credentials at load time.
    """
    configured = read_backend_connections(backend_config)
    result: dict[str, BackendConnection] = {}
    neo = configured.get("Neo4J")
    if neo is not None:
        result["Neo4J"] = BackendConnection(
            source="Neo4J",
            endpoint=f"http://127.0.0.1:{instance.neo4j_http_port}/db/{neo.database or 'neo4j'}/tx/commit",
            database=neo.database or "neo4j",
            username=instance.neo4j_username,
            password=instance.neo4j_password,
            bolt_endpoint=f"bolt://127.0.0.1:{instance.neo4j_bolt_port}",
        )
    arango = configured.get("ArangoDB")
    if arango is not None:
        database = arango.database or "university"
        result["ArangoDB"] = BackendConnection(
            source="ArangoDB",
            endpoint=f"http://127.0.0.1:{instance.arango_port}/_db/{database}/_api/cursor",
            database=database,
            username=arango.username or "root",
            password=instance.arango_password,
        )
    postgresql = configured.get("PostgreSQL")
    if postgresql is not None:
        result["PostgreSQL"] = BackendConnection(
            source="PostgreSQL",
            endpoint=f"127.0.0.1:{instance.postgresql_port}",
            database=postgresql.database or instance.postgresql_database,
            username=instance.postgresql_username,
            password=instance.postgresql_password,
        )
    return result


def require(command: str) -> str:
    value = shutil.which(command)
    if not value:
        raise RuntimeError(f"Required command is not installed or not on PATH: {command}")
    return value


def write_json(path: Path, value: Any) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    return path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sf_key(scale_factor: str) -> str:
    return _normalize_scale_factor(scale_factor)


def infer_scale_factor(path: Path) -> str | None:
    for part in (path.name, *[parent.name for parent in path.parents[:3]]):
        match = re.search(r"sf([0-9]+(?:\.[0-9]+)?)", part, flags=re.IGNORECASE)
        if match:
            return match.group(1)
    return None


def resolve_social_network_dir(input_path: str | Path) -> Path:
    candidate = Path(input_path).expanduser().resolve()
    if not candidate.is_dir():
        raise FileNotFoundError(f"LDBC dataset directory does not exist: {candidate}")
    if (candidate / "static").is_dir() and (candidate / "dynamic").is_dir():
        return candidate
    nested = candidate / "social_network"
    if (nested / "static").is_dir() and (nested / "dynamic").is_dir():
        return nested
    matches = sorted(
        parent
        for dynamic in candidate.glob("**/dynamic")
        if dynamic.is_dir() and (parent := dynamic.parent).joinpath("static").is_dir()
    )
    unique = list(dict.fromkeys(matches))
    if len(unique) == 1:
        return unique[0]
    if not unique:
        raise FileNotFoundError(
            f"Could not find sibling static/ and dynamic/ directories below {candidate}"
        )
    rendered = "\n".join(f"  {item}" for item in unique)
    raise ValueError(f"More than one social_network directory was found:\n{rendered}")


def write_benchmark_config(domain_id: str, *, schema: Path, data: Path, backends: Path) -> Path:
    output = ROOT / "tools" / "measurement" / "configs" / f"{domain_id}.json"
    payload = {
        "paths": {
            "schema": str(schema),
            "data": str(data),
            "backend_config": str(backends),
            "graph_db_binary": str(
                ROOT
                / "build"
                / ("gpu-bfs" if backend_config_requires_gpu(backends) else "cpu-bfs")
                / "bin"
                / "bfs"
            ),
            "loader_script": str(ROOT / "tools" / "load_graph.py"),
            "reference_bfs_script": str(ROOT / "tools" / "algorithms" / "bfs.py"),
            "docker_compose": str(COMPOSE_FILE),
        },
        "neo4j": {
            "http_endpoint": "http://localhost:7474/db/neo4j/tx/commit",
            "bolt_uri": "bolt://localhost:7687",
            "user": "neo4j",
            "password": "password",
        },
        "arango": {
            "base_url": "http://localhost:8529",
            "user": "root",
            "password": "rootpassword",
            "database": re.sub(r"[^A-Za-z0-9_]", "_", domain_id),
        },
        "bfs": {
            "max_depth": 7,
            "direction": "undirected",
            "allowed_edge_types": "",
            "allowed_target_node_types": "",
            "allow_native_pushdown": False,
            "no_partial_acceleration": False,
            "no_predecessors": True,
        },
        "batch_size": 5000,
    }
    return write_json(output, payload)


def prepare_university(args: argparse.Namespace) -> Path:
    output_root = DATA_ROOT / "university"
    output_root.mkdir(parents=True, exist_ok=True)
    schema = Path(args.schema or UNIVERSITY_SCHEMA).expanduser().resolve()
    if not schema.is_file():
        raise FileNotFoundError(f"University schema does not exist: {schema}")

    graph = output_root / "graph.jsonl"
    routing_backends = tuple(args.storage_backends) if args.storage_backends else None
    if args.input:
        source_graph = Path(args.input).expanduser().resolve()
        if not source_graph.is_file():
            raise FileNotFoundError(f"University graph does not exist: {source_graph}")
        step(f"Converting existing University graph to JSONL: {source_graph}")
        node_count, relationship_count = materialize_graph_jsonl(
            source_graph,
            graph,
            metadata={"domain": "university"},
            schema_path=schema,
            placement_strategy=args.placement_strategy,
            routing_backends=routing_backends,
            reroute=args.reroute_input,
            migrate_missing_routing=True,
        )
        print(
            f"Converted {node_count} nodes and {relationship_count} relationships "
            f"to {graph}"
        )
        graph_routing = open_graph_reader(graph).metadata().get("idRouting")
        if not graph_routing:
            raise ValueError("Imported graph has no idRouting metadata")
        routing_backends = tuple(str(value) for value in graph_routing)
    else:
        step(f"Generating University sample graph from {schema}")
        node_count, relationship_count = generate_sample_graph(
            schema,
            graph,
            nodes_per_type=args.nodes_per_type,
            seed=args.seed,
            placement_strategy=args.placement_strategy,
            routing_backends=routing_backends,
        )
        print(f"Generated {node_count} nodes and {relationship_count} relationships")

    graph_metadata = open_graph_reader(graph).metadata()
    graph_routing = graph_metadata.get("idRouting")
    if not graph_routing:
        raise ValueError("Generated graph has no idRouting metadata")
    routing_backends = tuple(str(value) for value in graph_routing)

    backends = output_root / "backends.json"
    database = args.arango_database or "university"
    write_backend_config_for_sources(routing_backends, backends, arango_database=database)

    validation = validate_graph(graph)
    write_json(output_root / "validation.json", validation)
    if not validation["valid"]:
        raise RuntimeError(f"University graph validation failed: {validation['errors'][:3]}")

    source = find_first_node(graph, args.start_type)
    observability = _write_domain_observability_config(output_root, backends)

    manifest = write_domain_manifest(
        output_root / "domain.json",
        domain_id="university",
        name="University Ecosystem",
        description="University graph domain using the existing graph-shape schema.",
        schema=schema,
        data=graph,
        backends=backends,
        observability=observability,
        start_uuid=source.uuid,
        max_depth=args.max_depth,
        direction=args.direction,
    )
    write_benchmark_config("university", schema=schema, data=graph, backends=backends)
    step("University domain is ready")
    print(f"manifest: {manifest}\ngraph:    {graph}\nschema:   {schema}\nbackends: {backends}")
    return manifest


def prepare_ldbc(args: argparse.Namespace) -> Path:
    social_network = resolve_social_network_dir(args.input)
    scale_factor = args.sf or infer_scale_factor(social_network) or "0.1"
    key = sf_key(scale_factor)
    domain_id = args.domain_id or f"ldbc-snb-{key}"
    output_root = (
        Path(args.output_dir).expanduser().resolve()
        if getattr(args, "output_dir", None)
        else DATA_ROOT / "ldbc-snb" / key
    )
    output_root.mkdir(parents=True, exist_ok=True)

    graph = output_root / "graph.jsonl"
    schema = output_root / "ldbc-snb.graph-shape.json"
    backends = output_root / "backends.json"
    arango_database = args.arango_database or re.sub(r"[^A-Za-z0-9_]", "_", domain_id)

    step(f"Converting LDBC SNB static + dynamic snapshot from {social_network}")
    result = convert_dataset(
        input_dir=social_network,
        output=graph,
        schema_output=schema,
        backend_config_output=backends,
        placement_config=args.placement_config,
        default_source=args.default_source,
        storage_backends=args.storage_backends,
        arango_database=arango_database,
        placement_report_dir=output_root,
        render_placement_graph=not getattr(args, "no_placement_graph_render", False),
        cache_config=getattr(args, "cache_config", None),
        cache_output_dir=getattr(args, "cache_output_dir", None),
        gpu_distribution_config=getattr(args, "gpu_distribution_config", None),
        gpu_distribution_output_dir=getattr(args, "gpu_distribution_output_dir", None),
    )
    print(json.dumps(result, indent=2))
    observability = _write_domain_observability_config(output_root, backends)

    validation_mode = getattr(args, "validation_mode", "basic")
    if validation_mode == "full":
        step("Running full graph validation (memory-intensive on very large graphs)")
        validation = validate_graph(graph)
    elif validation_mode == "none":
        validation = {
            "graph": str(graph),
            "valid": True,
            "mode": "none",
            "node_count": result["nodes"],
            "relationship_count": result["relationships"],
            "errors": [],
            "error_count": 0,
            "checks_skipped": ["post-conversion graph validation"],
        }
    else:
        # The LDBC converter now allocates IDs collision-free by construction
        # and streams placement/routing counts while writing the graph.  Reusing
        # those facts avoids constructing O(|V|+|E|) Python sets purely for a
        # second validation pass, which is not viable at SF30+.
        validation = {
            "graph": str(graph),
            "valid": True,
            "mode": "conversion",
            "node_count": result["nodes"],
            "relationship_count": result["relationships"],
            "id_routing": result["uuid_routing"],
            "id_routing_bits": result["uuid_routing_bits"],
            "id_payload_strategy": result.get("id_payload_strategy"),
            "errors": [],
            "error_count": 0,
            "checks": {
                "id_payload_collision_free_by_construction": True,
                "placement_counted_while_streaming": True,
                "duplicate_raw_node_ids": False,
                "endpoint_existence": False,
            },
        }
    validation_path = write_json(output_root / "validation.json", validation)
    if not validation["valid"]:
        raise RuntimeError(f"LDBC graph validation failed: {validation['errors'][:3]}")

    roots = select_roots(
        graph,
        node_type=args.start_type,
        count=args.root_count,
        seed=args.seed,
    )
    roots_path = write_roots_csv(output_root / "bfs_roots.csv", roots)
    (output_root / "start_uuids.txt").write_text(
        "".join(f"{node.uuid}\n" for node in roots), encoding="utf-8"
    )

    manifest = write_domain_manifest(
        output_root / "domain.json",
        domain_id=domain_id,
        name=f"LDBC SNB Interactive v1 SF{scale_factor}",
        description=(
            "LDBC SNB Interactive v1 initial graph snapshot converted from both static/ and dynamic/ CSV data. "
            "Top-level update streams are intentionally excluded from the read-only BFS graph."
        ),
        schema=schema,
        data=graph,
        backends=backends,
        observability=observability,
        start_uuid=roots[0].uuid,
        max_depth=args.max_depth,
        direction=args.direction,
        cache_config=getattr(args, "cache_config", None),
        cache_plan=(result.get("cache") or {}).get("plan"),
        gpu_distribution_config=getattr(args, "gpu_distribution_config", None),
        gpu_storage_plan=(result.get("gpu_storage") or {}).get("plan"),
    )
    write_benchmark_config(domain_id, schema=schema, data=graph, backends=backends)
    placement_files = {
        f"placement_{name}": value
        for name, value in result.get("placement", {}).items()
        if name != "warnings"
    }
    manifest_outputs = {
        "domain": str(manifest),
        "graph": str(graph),
        "schema": str(schema),
        "backends": str(backends),
        "observability": str(observability),
        "roots": str(roots_path),
        "validation": str(validation_path),
        **placement_files,
        **({"cache_plan": (result.get("cache") or {}).get("plan")} if (result.get("cache") or {}).get("plan") else {}),
    }
    manifest_hashes = {
        "graph": sha256(graph),
        "schema": sha256(schema),
        "backends": sha256(backends),
        "observability": sha256(observability),
        **{
            name: sha256(Path(path))
            for name, path in placement_files.items()
            if Path(path).is_file()
        },
    }
    write_json(
        output_root / "dataset-manifest.json",
        {
            "dataset": "LDBC SNB Interactive v1",
            "scale_factor": scale_factor,
            "input": str(social_network),
            "input_sections": ["static", "dynamic"],
            "update_streams_loaded": False,
            "outputs": manifest_outputs,
            "sha256": manifest_hashes,
        },
    )
    step("LDBC SNB domain is ready")
    placement_json = result.get("placement", {}).get("json", "not generated")
    placement_graph = result.get("placement", {}).get(
        "svg", result.get("placement", {}).get("dot", "not generated")
    )
    print(
        f"domain id:       {domain_id}\n"
        f"manifest:        {manifest}\n"
        f"graph:           {graph}\n"
        f"schema:          {schema}\n"
        f"placement report: {placement_json}\n"
        f"placement graph: {placement_graph}"
    )
    return manifest


def update_datagen_params(path: Path, scale_factor: str, partitions: int) -> None:
    lines = path.read_text(encoding="utf-8").splitlines()
    result: list[str] = []
    scale_written = False
    partition_written = False
    for line in lines:
        if line.startswith("ldbc.snb.datagen.generator.scaleFactor:"):
            result.append(f"ldbc.snb.datagen.generator.scaleFactor:snb.interactive.{scale_factor}")
            scale_written = True
        elif line.startswith("ldbc.snb.datagen.serializer.numUpdatePartitions:"):
            result.append(f"ldbc.snb.datagen.serializer.numUpdatePartitions:{partitions}")
            partition_written = True
        else:
            result.append(line)
    if not scale_written:
        result.append(f"ldbc.snb.datagen.generator.scaleFactor:snb.interactive.{scale_factor}")
    if not partition_written:
        result.append(f"ldbc.snb.datagen.serializer.numUpdatePartitions:{partitions}")
    path.write_text("\n".join(result) + "\n", encoding="utf-8")


def generate_ldbc(args: argparse.Namespace) -> Path:
    require("git")
    require("docker")
    tag = args.tag
    scale_factor = args.sf or "0.1"
    work_root = ROOT / "tools" / "ldbc_snb" / ".datagen"
    repository = work_root / "ldbc_snb_datagen_hadoop"
    output_root = DATA_ROOT / "ldbc-snb" / sf_key(scale_factor)
    raw = output_root / "raw"
    params = output_root / "params.ini"
    work_root.mkdir(parents=True, exist_ok=True)
    output_root.mkdir(parents=True, exist_ok=True)

    if args.force and raw.exists():
        shutil.rmtree(raw)
    raw.mkdir(parents=True, exist_ok=True)

    step(f"Checking out official LDBC SNB Hadoop Datagen {tag}")
    if not (repository / ".git").is_dir():
        run(["git", "clone", "https://github.com/ldbc/ldbc_snb_datagen_hadoop.git", repository])
    run(["git", "fetch", "--tags", "--force"], cwd=repository)
    run(
        ["git", "-c", "advice.detachedHead=false", "checkout", "--force", "--detach", tag],
        cwd=repository,
    )
    run(["git", "clean", "-fdx"], cwd=repository)

    shutil.copy2(repository / "params-csv-basic.ini", params)
    update_datagen_params(params, scale_factor, args.update_partitions)

    image = f"polygraph/ldbc-snb-datagen:{tag}"
    step("Building Datagen container")
    run(
        [
            "docker",
            "build",
            "--pull",
            "--build-arg",
            f"DATAGEN_BASE_IMAGE={args.base_image}",
            "--tag",
            image,
            "--file",
            DATAGEN_DOCKERFILE,
            repository,
        ]
    )

    step(f"Generating LDBC SNB SF{scale_factor}")
    run(
        [
            "docker",
            "run",
            "--rm",
            "-e",
            f"HADOOP_CLIENT_OPTS=-Xmx{args.memory}",
            "--mount",
            f"type=bind,source={raw},target=/opt/ldbc_snb_datagen/out",
            "--mount",
            f"type=bind,source={params},target=/opt/ldbc_snb_datagen/params.ini,readonly",
            image,
            "/bin/bash",
            "-lc",
            (
                "/opt/ldbc_snb_datagen/docker_run.sh "
                f"&& chown -R {os.getuid()}:{os.getgid()} /opt/ldbc_snb_datagen/out"
            ),
        ]
    )

    social_network = resolve_social_network_dir(raw)
    prepare_args = argparse.Namespace(
        input=str(social_network),
        sf=scale_factor,
        domain_id=args.domain_id,
        placement_config=args.placement_config,
        default_source=args.default_source,
        storage_backends=args.storage_backends,
        arango_database=args.arango_database,
        start_type=args.start_type,
        root_count=args.root_count,
        seed=args.seed,
        max_depth=args.max_depth,
        direction=args.direction,
        output_dir=args.output_dir,
        no_placement_graph_render=args.no_placement_graph_render,
        cache_config=args.cache_config,
        cache_output_dir=args.cache_output_dir,
    )
    return prepare_ldbc(prepare_args)


def build_project(args: argparse.Namespace) -> None:
    require("cmake")
    step(f"Building C++ BFS executable with preset {args.preset}")
    run(["cmake", "--preset", args.preset])
    run(["cmake", "--build", "--preset", args.preset])


def wait_for_port(host: str, port: int, name: str, timeout: int) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=2):
                print(f"{name} TCP port is reachable at {host}:{port}")
                return
        except OSError:
            time.sleep(1)
    raise TimeoutError(f"Timed out waiting for {name} at {host}:{port}")


def _basic_auth(username: str, password: str) -> str:
    token = base64.b64encode(f"{username}:{password}".encode("utf-8")).decode("ascii")
    return f"Basic {token}"


def wait_for_neo4j(
    timeout: int,
    *,
    host: str = "127.0.0.1",
    port: int = 7474,
    username: str = "neo4j",
    password: str = "password",
) -> None:
    url = f"http://{host}:{port}/db/neo4j/tx/commit"
    payload = json.dumps({"statements": [{"statement": "RETURN 1 AS ready"}]}).encode("utf-8")
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None

    while time.monotonic() < deadline:
        request = urllib.request.Request(url, data=payload, method="POST")
        request.add_header("Accept", "application/json")
        request.add_header("Content-Type", "application/json")
        request.add_header("Authorization", _basic_auth(username, password))
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                body = json.loads(response.read().decode("utf-8"))
            if response.status in {200, 201} and not body.get("errors"):
                print("Neo4j transaction API is ready")
                return
            last_error = RuntimeError(f"Neo4j returned: {body.get('errors', body)}")
        except (OSError, urllib.error.URLError, urllib.error.HTTPError, ValueError) as exc:
            last_error = exc
        time.sleep(2)

    raise TimeoutError(f"Timed out waiting for Neo4j transaction API: {last_error}")


def wait_for_arangodb(
    timeout: int,
    *,
    host: str = "127.0.0.1",
    port: int = 8529,
    username: str = "root",
    password: str = "rootpassword",
) -> None:
    url = f"http://{host}:{port}/_api/version"
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None

    while time.monotonic() < deadline:
        request = urllib.request.Request(url, method="GET")
        request.add_header("Accept", "application/json")
        request.add_header("Authorization", _basic_auth(username, password))
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                body = json.loads(response.read().decode("utf-8"))
            if response.status == 200 and body.get("server") == "arango":
                print("ArangoDB HTTP API is ready")
                return
            last_error = RuntimeError(f"ArangoDB returned: {body}")
        except (OSError, urllib.error.URLError, urllib.error.HTTPError, ValueError) as exc:
            last_error = exc
        time.sleep(2)

    raise TimeoutError(f"Timed out waiting for ArangoDB HTTP API: {last_error}")


def services_start(args: argparse.Namespace) -> None:
    require("docker")
    step("Starting Neo4j, ArangoDB, and PostgreSQL")
    run(["docker", "compose", "-f", COMPOSE_FILE, "up", "-d"])
    # A listening TCP port is not sufficient: Neo4j can accept a connection
    # before its database and transaction endpoint are ready, or briefly reset
    # connections while startup/plugin initialization completes.
    wait_for_port("127.0.0.1", 7474, "Neo4j", args.timeout)
    wait_for_neo4j(args.timeout)
    wait_for_port("127.0.0.1", 8529, "ArangoDB", args.timeout)
    wait_for_arangodb(args.timeout)
    wait_for_port("127.0.0.1", 5432, "PostgreSQL", args.timeout)


def services_stop(args: argparse.Namespace) -> None:
    require("docker")
    command: list[str | os.PathLike[str]] = ["docker", "compose", "-f", COMPOSE_FILE, "down"]
    if args.volumes:
        command.append("--volumes")
    run(command)


def _print_instance_diagnostics(instance: ComposeInstance) -> None:
    print("\n[polygraph] Instance startup diagnostics:", file=sys.stderr)
    environment = _compose_environment(instance)
    for arguments in (("ps",), ("logs", "--tail", "120")):
        command = [*_compose_command(instance), *arguments]
        print("+", " ".join(str(item) for item in command), file=sys.stderr, flush=True)
        subprocess.run(command, cwd=ROOT, env=environment, check=False)


def wait_for_compose_instance(
    instance: ComposeInstance,
    services: Iterable[str] | None = None,
) -> None:
    selected = set(services or ("neo4j", "arangodb", "postgresql"))
    try:
        if "neo4j" in selected:
            wait_for_port("127.0.0.1", instance.neo4j_http_port, "Neo4j", instance.timeout_seconds)
            wait_for_neo4j(
                instance.timeout_seconds,
                port=instance.neo4j_http_port,
                username=instance.neo4j_username,
                password=instance.neo4j_password,
            )
        if "arangodb" in selected:
            wait_for_port("127.0.0.1", instance.arango_port, "ArangoDB", instance.timeout_seconds)
            wait_for_arangodb(
                instance.timeout_seconds,
                port=instance.arango_port,
                password=instance.arango_password,
            )
        if "postgresql" in selected:
            wait_for_port(
                "127.0.0.1", instance.postgresql_port, "PostgreSQL", instance.timeout_seconds
            )
    except BaseException:
        _print_instance_diagnostics(instance)
        raise


def start_compose_instance(
    instance: ComposeInstance,
    *,
    recreate: bool = False,
    services: Iterable[str] | None = None,
) -> None:
    ensure_observability_network()
    selected = list(services or [])
    arguments = ["up", "-d"]
    if recreate:
        arguments.append("--force-recreate")
    arguments.extend(selected)
    run_compose(instance, *arguments)
    wait_for_compose_instance(instance, selected or None)


def manage_compose_instance(args: argparse.Namespace) -> None:
    instance = resolve_compose_instance(args.scale_factor, args.distribution)
    action = args.instance_action

    if action == "project":
        print(instance.project)
        return
    if action == "options":
        payload = asdict(instance)
        payload["compose_file"] = str(instance.compose_file)
        payload["env_file"] = str(instance.env_file)
        if not args.show_secrets:
            payload["variables"] = {
                name: (
                    "<redacted>"
                    if any(token in name.upper() for token in ("PASSWORD", "SECRET", "TOKEN"))
                    else value
                )
                for name, value in instance.variables.items()
            }
        print(json.dumps(payload, indent=2, sort_keys=True))
        return
    if action == "benchmark":
        command: list[str | os.PathLike[str]] = [
            sys.executable,
            ROOT / "tools" / "polygraph.py",
            "benchmark",
            args.config,
            *args.benchmark_args,
        ]
        run(command)
        return

    require("docker")

    selected_services = list(getattr(args, "services", None) or [])
    if action == "activate":
        stop_other_polygraph_instances(instance)
        start_compose_instance(instance, services=selected_services or None)
        return
    if action == "up":
        start_compose_instance(instance, services=selected_services or None)
        return
    if action == "start":
        run_compose(instance, "start", *selected_services)
        wait_for_compose_instance(instance, selected_services or None)
        return
    if action == "restart":
        start_compose_instance(
            instance, recreate=args.recreate, services=selected_services or None
        )
        return
    if action == "stop":
        run_compose(instance, "stop")
        return
    if action == "down":
        run_compose(instance, "down", "--remove-orphans")
        return
    if action in {"status", "ps"}:
        run_compose(instance, "ps")
        return
    if action == "logs":
        command: list[str | os.PathLike[str]] = ["logs"]
        if args.follow:
            command.append("--follow")
        command.extend(["--tail", str(args.tail)])
        command.extend(args.services)
        run_compose(instance, *command)
        return
    if action == "load":
        start_compose_instance(instance, services=selected_services or None)
        domain = args.domain or instance.domain
        if not domain:
            raise ValueError(
                f"Instance {instance.key!r} has no domain. Add 'domain' in "
                f"{COMPOSE_INSTANCES_FILE} or pass it after 'load'."
            )
        resolved_domain = resolve_domain(domain, DATA_ROOT)
        load_domain(
            argparse.Namespace(
                domain=str(resolved_domain.paths.manifest),
                reset=not args.no_reset,
                batch_size=args.batch_size,
                workers=args.workers,
                postgres_indexes=args.postgres_indexes,
                only_source=args.only_source,
            ),
            connection_overrides=_instance_connection_overrides(
                instance, resolved_domain.paths.backends
            ),
        )
        return
    if action == "cache-sync":
        start_compose_instance(instance)
        domain = args.domain or instance.domain
        if not domain:
            raise ValueError(
                f"Instance {instance.key!r} has no domain. Add 'domain' in "
                f"{COMPOSE_INSTANCES_FILE} or pass it after 'cache-sync'."
            )
        resolved_domain = resolve_domain(domain, DATA_ROOT)
        sync_domain_cache_residency(
            argparse.Namespace(
                domain=str(resolved_domain.paths.manifest),
                batch_size=args.batch_size,
                only_source=args.only_source,
            ),
            connection_overrides=_instance_connection_overrides(
                instance, resolved_domain.paths.backends
            ),
        )
        return
    if action == "destroy":
        if not args.yes:
            raise ValueError(
                f"Refusing to delete volumes for {instance.project}; pass destroy --yes"
            )
        run_compose(instance, "down", "--volumes", "--remove-orphans")
        return
    raise ValueError(f"Unsupported instance action: {action}")


def load_domain(
    args: argparse.Namespace,
    *,
    connection_overrides: dict[str, BackendConnection] | None = None,
) -> None:
    domain = resolve_domain(args.domain, DATA_ROOT)
    step(f"Loading domain {domain.domain_id}")
    selected = args.only_source or None
    # Stable 48-bit payload IDs mean cache reconfiguration never requires an
    # old->new Neo4j graph rewrite.  Always load the active domain graph so the
    # persisted cache_residency node attribute matches the current cache plan.
    input_graph = domain.paths.data
    print(
        f"[polygraph] Sources: {', '.join(selected or ['all enabled'])}; "
        f"input graph: {input_graph}",
        flush=True,
    )
    if connection_overrides:
        for source, connection in sorted(connection_overrides.items()):
            print(
                f"[polygraph] {source} instance endpoint: "
                f"{connection.bolt_endpoint or connection.endpoint}",
                flush=True,
            )
    print(
        "[polygraph] PostgreSQL traversal indexes: "
        + ("enabled" if getattr(args, "postgres_indexes", False) else "disabled (default)"),
        flush=True,
    )
    result = load_graph(
        input_graph,
        domain.paths.backends,
        reset=args.reset,
        batch_size=args.batch_size,
        load_workers=getattr(args, "workers", 4),
        postgresql_indexes=getattr(args, "postgres_indexes", False),
        only_sources=selected,
        connection_overrides=connection_overrides,
    )
    print(json.dumps(result, indent=2))

def _relative_to_manifest(path: Path, manifest: Path) -> str:
    try:
        return os.path.relpath(path.resolve(), manifest.parent.resolve())
    except ValueError:
        return str(path.resolve())


def _rewrite_backend_cache_config(
    domain,
    cache_config: dict[str, Any],
    cache_root: Path,
) -> None:
    """Regenerate cache/device entries while preserving DB connection details."""
    backend_path = domain.paths.backends
    old = json.loads(backend_path.read_text(encoding="utf-8"))
    old_entries = {
        str(entry.get("id")): entry
        for entry in old.get("backends", [])
        if isinstance(entry, dict)
    }
    routing = tuple(str(value) for value in old.get("idRouting", ()))
    if not routing:
        routing = tuple(str(value) for value in open_graph_reader(domain.paths.data).metadata().get("idRouting", ()))
    if not routing:
        raise ValueError("Cannot reconfigure cache: backend config/graph has no idRouting")
    arango_database = "university"
    old_arango = old_entries.get("ArangoDB")
    if old_arango:
        arango_database = str(old_arango.get("connection", {}).get("database", arango_database))

    temporary = backend_path.with_name(f".{backend_path.name}.cache.tmp")
    write_backend_config_for_sources(
        routing,
        temporary,
        arango_database=arango_database,
        cache_config=cache_config,
        cache_root=cache_root,
    )
    new = json.loads(temporary.read_text(encoding="utf-8"))
    for entry in new.get("backends", []):
        backend_id = str(entry.get("id"))
        if backend_id not in {"Neo4J", "ArangoDB", "PostgreSQL"}:
            continue
        previous = old_entries.get(backend_id)
        if not previous:
            continue
        # Cache priorities may change, but deployment-specific connection data
        # must survive a cache-only reconfiguration.
        for field in ("connection", "bolt_endpoint"):
            if field in previous:
                entry[field] = previous[field]
    backend_path.write_text(json.dumps(new, indent=2) + "\n", encoding="utf-8")
    temporary.unlink(missing_ok=True)


def reconfigure_cache(args: argparse.Namespace) -> None:
    domain = resolve_domain(args.domain, DATA_ROOT)
    config_path = Path(args.cache_config).expanduser().resolve()
    config = load_cache_config(config_path)
    output_dir = (
        Path(args.output_dir).expanduser().resolve()
        if args.output_dir
        else domain.paths.manifest.parent / "cache"
    )
    step(f"Reconfiguring caches for {domain.domain_id} from existing graph files")
    print(f"[polygraph] graph:        {domain.paths.data}", flush=True)
    print(f"[polygraph] cache config: {config_path}", flush=True)
    print(f"[polygraph] cache output: {output_dir}", flush=True)
    planning_db = (
        Path(args.planning_db).expanduser().resolve()
        if getattr(args, "planning_db", None)
        else None
    )
    result = plan_graph_caches(
        domain.paths.data,
        config,
        output_dir=output_dir,
        resume=bool(getattr(args, "resume", False)),
        planning_db_path=planning_db,
    )
    # Cache backends are injected from cache-plan.json at runtime. Do not rewrite
    # the authoritative backend configuration or its gpuStoragePlan reference.
    _write_domain_observability_config(domain.paths.manifest.parent, domain.paths.backends)

    raw = json.loads(domain.paths.manifest.read_text(encoding="utf-8"))
    paths = raw.setdefault("paths", {})
    paths["cacheConfig"] = _relative_to_manifest(config_path, domain.paths.manifest)
    plan_path = Path(str(result["plan"])).resolve()
    paths["cachePlan"] = _relative_to_manifest(plan_path, domain.paths.manifest)
    raw.setdefault("runtime", {})["cacheGeneration"] = int(
        raw.get("runtime", {}).get("cacheGeneration", 0)
    ) + 1
    domain.paths.manifest.write_text(json.dumps(raw, indent=2) + "\n", encoding="utf-8")

    if args.reload_redis and config.get("redis", {}).get("enabled", False):
        from tools.cache.load_redis import load as load_redis_artifact
        step("Reloading Redis from the new cache artifact")
        redis_result = load_redis_artifact(
            domain.paths.manifest,
            reset=True,
            batch_size=args.redis_batch_size,
        )
        result["redisReload"] = redis_result

    print(json.dumps({
        "domain": str(domain.paths.manifest),
        "graph": str(domain.paths.data),
        "cachePlan": str(plan_path),
        "backendConfig": str(domain.paths.backends),
        "result": result,
    }, indent=2))


def sync_domain_cache_residency(
    args: argparse.Namespace,
    *,
    connection_overrides: dict[str, BackendConnection] | None = None,
) -> None:
    domain = resolve_domain(args.domain, DATA_ROOT)
    step(f"Synchronizing cache_residency metadata for {domain.domain_id}")
    result = sync_cache_residency(
        domain.paths.data,
        domain.paths.backends,
        batch_size=args.batch_size,
        only_sources=args.only_source or None,
        connection_overrides=connection_overrides,
    )
    print(json.dumps(result, indent=2))


def load_redis_cache(args: argparse.Namespace) -> None:
    if args.batch_size <= 0:
        raise ValueError("--batch-size must be positive")
    domain = resolve_domain(args.domain, DATA_ROOT)
    from tools.cache.load_redis import load as load_redis_artifact

    step(f"Loading generated Redis cache for {domain.domain_id}")
    result = load_redis_artifact(
        domain.paths.manifest,
        reset=not args.no_reset,
        batch_size=args.batch_size,
        cache_plan=Path(args.cache_plan).expanduser().resolve() if args.cache_plan else None,
    )
    print(json.dumps(result, indent=2))


def build_gpu_storage(args: argparse.Namespace) -> None:
    """Rebuild authoritative GPU distribution from an already prepared graph."""
    domain = resolve_domain(args.domain, DATA_ROOT)
    config_path = Path(args.config).expanduser().resolve() if args.config else None
    distribution = load_gpu_distribution_config(config_path)
    output_dir = (
        Path(args.output_dir).expanduser().resolve()
        if args.output_dir
        else domain.paths.manifest.parent / "gpu-storage"
    )
    planning_db = (
        Path(args.planning_db).expanduser().resolve()
        if args.planning_db
        else domain.paths.data.parent / "cache-plan.sqlite3"
    )

    step(f"Building authoritative GPU storage for {domain.domain_id}")
    print(f"[polygraph] graph:             {domain.paths.data}", flush=True)
    print(
        f"[polygraph] GPU distribution:  {config_path if config_path else 'default GPU:0'}",
        flush=True,
    )
    print(f"[polygraph] GPU storage root:  {output_dir}", flush=True)
    print(f"[polygraph] planning DB:       {planning_db}", flush=True)

    result = plan_gpu_storage(
        domain.paths.data,
        distribution,
        output_dir=output_dir,
        planning_db_path=planning_db,
    )
    plan_path = Path(str(result["plan"])).resolve()

    # Physical GPU:N entries are injected from gpu-storage-plan.json at runtime.
    # Keep backends.json authoritative-only and point it at the new base plan.
    backend_raw = json.loads(domain.paths.backends.read_text(encoding="utf-8"))
    backend_raw["backends"] = [
        entry
        for entry in backend_raw.get("backends", [])
        if str(entry.get("kind")) != "CuGraph"
    ]
    backend_raw["gpuStoragePlan"] = _relative_to_manifest(plan_path, domain.paths.backends)
    domain.paths.backends.write_text(json.dumps(backend_raw, indent=2) + "\n", encoding="utf-8")
    _write_domain_observability_config(domain.paths.manifest.parent, domain.paths.backends)

    manifest_raw = json.loads(domain.paths.manifest.read_text(encoding="utf-8"))
    paths = manifest_raw.setdefault("paths", {})
    paths["gpuStoragePlan"] = _relative_to_manifest(plan_path, domain.paths.manifest)
    if config_path is not None:
        paths["gpuDistributionConfig"] = _relative_to_manifest(config_path, domain.paths.manifest)
    manifest_raw.setdefault("runtime", {})["gpuStorageGeneration"] = int(
        manifest_raw.get("runtime", {}).get("gpuStorageGeneration", 0)
    ) + 1
    domain.paths.manifest.write_text(json.dumps(manifest_raw, indent=2) + "\n", encoding="utf-8")

    print(json.dumps({
        "domain": domain.domain_id,
        "gpuStoragePlan": str(plan_path),
        "backendConfig": str(domain.paths.backends),
        "result": result,
    }, indent=2))


def build_cache_plans(args: argparse.Namespace) -> None:
    """Build one or more independent cache plans from one reusable graph index."""
    if args.redis_batch_size <= 0:
        raise ValueError("--redis-batch-size must be positive")
    domain = resolve_domain(args.domain, DATA_ROOT)
    from tools.cache.load_redis import load as load_redis_artifact
    from tools.graphio.cache_planner import load_cache_config, plan_graph_caches

    cache_root = (
        Path(args.output_dir).expanduser().resolve()
        if args.output_dir
        else domain.paths.manifest.parent / "caches"
    )
    planning_db = (
        Path(args.planning_db).expanduser().resolve()
        if args.planning_db
        else domain.paths.data.parent / "cache-plan.sqlite3"
    )
    cache_root.mkdir(parents=True, exist_ok=True)

    results: list[dict[str, Any]] = []
    used_names: set[str] = set()
    for raw_config in args.config:
        config_path = Path(raw_config).expanduser().resolve()
        base = re.sub(r"[^A-Za-z0-9._-]+", "-", config_path.stem).strip("-.") or "cache"
        name = base
        suffix = 2
        while name in used_names:
            name = f"{base}-{suffix}"
            suffix += 1
        used_names.add(name)
        plan_dir = cache_root / name

        step(f"Building cache plan {name} from {config_path.name}")
        plan = plan_graph_caches(
            domain.paths.data,
            load_cache_config(config_path),
            output_dir=plan_dir,
            planning_db_path=planning_db,
        )
        item: dict[str, Any] = {
            "name": name,
            "config": str(config_path),
            "plan": str(plan["plan"]),
            "planningDatabase": str(planning_db),
            "reusedPlanningDatabase": bool(plan.get("reusedPlanningDatabase", False)),
        }
        if args.load_redis and plan.get("redis", {}).get("enabled", False):
            item["redis"] = load_redis_artifact(
                domain.paths.manifest,
                reset=not args.redis_no_reset,
                batch_size=args.redis_batch_size,
                cache_plan=Path(plan["plan"]),
            )
        results.append(item)

    print(json.dumps({"domain": domain.domain_id, "caches": results}, indent=2))


def backend_config_requires_gpu(config_path: Path) -> bool:
    config = json.loads(config_path.read_text(encoding="utf-8"))
    routing = {str(value) for value in config.get("idRouting", [])}
    enabled = {
        str(entry.get("id", ""))
        for entry in config.get("backends", [])
        if entry.get("enabled", True)
    }
    return "GPU" in routing and any(value == "GPU" or value.startswith("GPU:") for value in enabled)


def find_bfs_binary(backend_config: Path, override: str | None = None) -> Path:
    if override:
        binary = Path(override).expanduser().resolve()
        if not os.access(binary, os.X_OK):
            raise FileNotFoundError(f"BFS binary is not executable: {binary}")
        return binary

    gpu = backend_config_requires_gpu(backend_config)
    names = ["gpu-bfs", "gpu-bfs-debug"] if gpu else [
        "cpu-bfs",
        "cpu-bfs-debug",
        "full-cpu",
        "gpu-bfs",
        "gpu-bfs-debug",
    ]
    candidates = [ROOT / "build" / name / "bin" / "polygraph_bfs" for name in names]
    candidates.extend(
        [
            ROOT / "cmake-build-debug" / "bin" / "bfs",
            ROOT / "cmake-build-release" / "bin" / "bfs",
        ]
    )
    for candidate in candidates:
        if os.access(candidate, os.X_OK):
            return candidate
    requirement = "GPU-enabled " if gpu else ""
    raise FileNotFoundError(
        f"No {requirement}BFS executable was found. Run: python3 tools/polygraph.py build "
        f"--preset {'gpu-bfs' if gpu else 'cpu-bfs'}"
    )


def has_cli_option(arguments: list[str], option: str) -> bool:
    return any(value == option or value.startswith(f"{option}=") for value in arguments)


def pop_cli_option(arguments: list[str], option: str) -> tuple[str | None, list[str]]:
    """Remove one value-taking option from a forwarded CLI argument list."""
    value: str | None = None
    remaining: list[str] = []
    index = 0

    while index < len(arguments):
        item = arguments[index]
        if item == option:
            if index + 1 >= len(arguments):
                raise ValueError(f"{option} requires a value")
            if value is not None:
                raise ValueError(f"{option} was supplied more than once")
            value = arguments[index + 1]
            index += 2
            continue

        prefix = f"{option}="
        if item.startswith(prefix):
            if value is not None:
                raise ValueError(f"{option} was supplied more than once")
            value = item[len(prefix) :]
            index += 1
            continue

        remaining.append(item)
        index += 1

    return value, remaining


def safe_path_component(value: str, *, label: str) -> str:
    if label == "run id" and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", value):
        raise ValueError(
            "Run id must start with an alphanumeric character and contain only "
            "letters, digits, '.', '_' or '-'."
        )

    rendered = re.sub(r"[^A-Za-z0-9._-]", "_", value)
    if not rendered or rendered in {".", ".."}:
        raise ValueError(f"Invalid {label}: {value!r}")
    return rendered


def read_start_uuids(path: str | Path) -> list[str]:
    source = Path(path).expanduser().resolve()
    if not source.is_file():
        raise FileNotFoundError(f"Start-ID file does not exist: {source}")

    values = [
        line.strip()
        for line in source.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not values:
        raise ValueError(f"Start-ID file is empty: {source}")
    return values


def run_domain(args: argparse.Namespace) -> None:
    domain = resolve_domain(args.domain, DATA_ROOT)
    binary = find_bfs_binary(domain.paths.backends, getattr(args, "binary", None))
    extras = list(getattr(args, "bfs_args", None) or [])
    if extras and extras[0] == "--":
        extras.pop(0)

    if has_cli_option(extras, "--event-file"):
        raise ValueError(
            "Do not pass --event-file directly. Use --run-id; polygraph.py creates "
            "measurements/<run-id>/events/<start-uuid>/<repetition>.events.jsonl."
        )

    if has_cli_option(extras, "--start-type"):
        raise ValueError(
            "--start-type is no longer supported by the BFS executable; "
            "provide only --start-uuid."
        )

    forwarded_start_uuid, extras = pop_cli_option(extras, "--start-uuid")

    wrapper_start_uuid = getattr(args, "start_uuid", None)
    start_uuids_file = getattr(args, "start_uuids_file", None)
    if start_uuids_file and (wrapper_start_uuid or forwarded_start_uuid):
        raise ValueError("--start-uuids-file cannot be combined with --start-uuid")
    if wrapper_start_uuid and forwarded_start_uuid:
        raise ValueError("--start-uuid was supplied both before and after '--'")

    if start_uuids_file:
        start_uuids = read_start_uuids(start_uuids_file)
    else:
        start_uuids = [wrapper_start_uuid or forwarded_start_uuid or domain.start_uuid]
    if domain.paths.native_id_map:
        start_uuids = [str(resolve_domain_node_id(domain, value)) for value in start_uuids]

    repetitions = int(getattr(args, "repetitions", 1))
    if repetitions < 1:
        raise ValueError("--repetitions must be at least 1")

    run_id = getattr(args, "run_id", None) or (
        f"{domain.domain_id}-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}"
    )
    run_id = safe_path_component(run_id, label="run id")
    measurement_root = ROOT / "measurements" / run_id
    measurement_root.mkdir(parents=True, exist_ok=True)
    overwrite_logs = bool(getattr(args, "overwrite_logs", False))

    common_command: list[str | os.PathLike[str]] = [
        binary,
        "--domain",
        domain.paths.manifest,
    ]

    thread_count = getattr(args, "thread_count", None)
    if thread_count is not None:
        if thread_count <= 0:
            raise ValueError("--thread-count must be greater than zero")
        if not has_cli_option(extras, "--thread-count"):
            common_command.extend(["--thread-count", str(thread_count)])

    neighbor_batch_size = getattr(args, "neighbor_batch_size", None)
    if neighbor_batch_size is not None:
        if neighbor_batch_size <= 0:
            raise ValueError("--neighbor-batch-size must be greater than zero")
        if not has_cli_option(extras, "--neighbor-batch-size"):
            common_command.extend(["--neighbor-batch-size", str(neighbor_batch_size)])

    measurements_csv = measurement_root / "measurements.csv"
    summary_json = measurement_root / "summary.json"
    fieldnames = [
        "run_id",
        "domain",
        "start_uuid",
        "repetition",
        "seconds",
        "status",
        "result_rows",
        "output_csv",
        "event_file",
        "application_log_file",
        "output_sha256",
        "same_as_first",
    ]

    if measurements_csv.exists() and not overwrite_logs:
        raise ValueError(
            f"Measurement file already exists: {measurements_csv}. "
            "Use a different --run-id or pass --overwrite-logs."
        )
    if overwrite_logs:
        measurements_csv.unlink(missing_ok=True)
        summary_json.unlink(missing_ok=True)

    total_runs = len(start_uuids) * repetitions
    current_run = 0
    successful_runs = 0
    failed_runs = 0
    mismatched_repetitions = 0
    first_hash_by_uuid: dict[str, str] = {}

    with measurements_csv.open("w", encoding="utf-8", newline="") as measurements_handle:
        writer = csv.DictWriter(measurements_handle, fieldnames=fieldnames)
        writer.writeheader()

        for start_uuid in start_uuids:
            uuid_directory = safe_path_component(start_uuid, label="start ID")
            for repetition in range(1, repetitions + 1):
                current_run += 1
                event_file = measurement_root / "events" / uuid_directory / f"{repetition}.events.jsonl"
                application_log_file = measurement_root / "logs" / uuid_directory / f"{repetition}.log"
                output_csv = measurement_root / "outputs" / uuid_directory / f"{repetition}.csv"
                event_file.parent.mkdir(parents=True, exist_ok=True)
                application_log_file.parent.mkdir(parents=True, exist_ok=True)
                output_csv.parent.mkdir(parents=True, exist_ok=True)

                existing = [path for path in (event_file, application_log_file, output_csv) if path.exists()]
                if existing and not overwrite_logs:
                    rendered = ", ".join(str(path) for path in existing)
                    raise ValueError(
                        f"Run artifacts already exist: {rendered}. "
                        "Use a different --run-id or pass --overwrite-logs."
                    )
                if overwrite_logs:
                    event_file.unlink(missing_ok=True)
                    application_log_file.unlink(missing_ok=True)
                    output_csv.unlink(missing_ok=True)

                command = [
                    *common_command,
                    "--start-uuid",
                    start_uuid,
                    "--event-file",
                    event_file,
                    "--log-file",
                    application_log_file,
                    "--output-file",
                    output_csv,
                    *extras,
                ]

                step(
                    f"Running BFS for {domain.domain_id}: ID {start_uuid}, "
                    f"repetition {repetition}/{repetitions} ({current_run}/{total_runs})"
                )
                rendered_command = [str(item) for item in command]
                print("+", " ".join(rendered_command), flush=True)

                started = time.perf_counter()
                completed = subprocess.run(
                    rendered_command,
                    cwd=ROOT,
                    check=False,
                )
                elapsed = time.perf_counter() - started

                status = "success" if completed.returncode == 0 else "failed"
                output_hash = sha256(output_csv) if output_csv.is_file() else ""
                result_rows = 0
                if output_csv.is_file():
                    with output_csv.open("r", encoding="utf-8", errors="replace") as result_handle:
                        result_rows = max(sum(1 for _ in result_handle) - 1, 0)

                first_hash = first_hash_by_uuid.get(start_uuid)
                if completed.returncode == 0 and first_hash is None:
                    first_hash_by_uuid[start_uuid] = output_hash
                    same_as_first: bool | str = ""
                elif completed.returncode == 0:
                    same_as_first = output_hash == first_hash
                    if not same_as_first:
                        mismatched_repetitions += 1
                else:
                    same_as_first = ""

                writer.writerow(
                    {
                        "run_id": run_id,
                        "domain": domain.domain_id,
                        "start_uuid": start_uuid,
                        "repetition": repetition,
                        "seconds": f"{elapsed:.9f}",
                        "status": status,
                        "result_rows": result_rows,
                        "output_csv": output_csv.relative_to(measurement_root),
                        "event_file": event_file.relative_to(measurement_root),
                        "application_log_file": application_log_file.relative_to(measurement_root),
                        "output_sha256": output_hash,
                        "same_as_first": same_as_first,
                    }
                )
                measurements_handle.flush()

                if completed.returncode == 0:
                    successful_runs += 1
                else:
                    failed_runs += 1
                    write_json(
                        summary_json,
                        {
                            "runId": run_id,
                            "domain": domain.domain_id,
                            "totalRuns": total_runs,
                            "completedRuns": current_run,
                            "successfulRuns": successful_runs,
                            "failedRuns": failed_runs,
                            "mismatchedRepetitions": mismatched_repetitions,
                            "status": "failed",
                        },
                    )
                    raise subprocess.CalledProcessError(completed.returncode, rendered_command)

    write_json(
        summary_json,
        {
            "runId": run_id,
            "domain": domain.domain_id,
            "startUuidCount": len(start_uuids),
            "repetitions": repetitions,
            "totalRuns": total_runs,
            "successfulRuns": successful_runs,
            "failedRuns": failed_runs,
            "mismatchedRepetitions": mismatched_repetitions,
            "allRepetitionsIdentical": mismatched_repetitions == 0,
            "status": "success" if failed_runs == 0 else "failed",
        },
    )

    reconstruction_dir = measurement_root / "log_reconstruction"
    print(f"Measurements: {measurements_csv}")
    print(f"Summary:      {summary_json}")
    print(f"Outputs:      {measurement_root / 'outputs'}")
    print(f"Logs:         {measurement_root / 'logs'}")
    print("Reconstruct logs with:")
    print(
        f"  {sys.executable} tools/measurement/log_reconstruction.py "
        f"{measurement_root} --out-dir {reconstruction_dir} --inline-plotly"
    )


def analyze_measurement_logs(args: argparse.Namespace) -> None:
    candidate = Path(args.run).expanduser()
    if candidate.exists():
        measurement_root = candidate.resolve()
    else:
        measurement_root = (ROOT / "measurements" / safe_path_component(args.run, label="run id")).resolve()

    output_dir = (
        Path(args.out_dir).expanduser().resolve()
        if args.out_dir
        else measurement_root / "log_reconstruction"
    )
    command: list[str | os.PathLike[str]] = [
        sys.executable,
        ROOT / "tools" / "measurement" / "log_reconstruction.py",
        measurement_root,
        "--out-dir",
        output_dir,
    ]
    if args.inline_plotly:
        command.append("--inline-plotly")
    if args.no_averages:
        command.append("--no-averages")
    run(command)
    print(f"Dashboard: {output_dir / 'dashboard.html'}")


def serve_live_measurement_log(args: argparse.Namespace) -> None:
    log_file: Path | None = None
    if args.log_file:
        log_file = Path(args.log_file).expanduser()
        if not log_file.is_absolute():
            log_file = (ROOT / log_file).resolve(strict=False)
        else:
            log_file = log_file.resolve(strict=False)

    if args.watch_dir:
        watch_dir = Path(args.watch_dir).expanduser()
        if not watch_dir.is_absolute():
            watch_dir = (ROOT / watch_dir).resolve(strict=False)
        else:
            watch_dir = watch_dir.resolve(strict=False)
    elif log_file is not None:
        try:
            log_file.relative_to(ROOT / "measurements")
            watch_dir = (ROOT / "measurements").resolve(strict=False)
        except ValueError:
            watch_dir = log_file.parent
    else:
        watch_dir = (ROOT / "measurements").resolve(strict=False)

    output_dir = (
        Path(args.out_dir).expanduser().resolve(strict=False)
        if args.out_dir
        else watch_dir / "live_timeline"
    )
    command: list[str | os.PathLike[str]] = [
        sys.executable,
        ROOT / "tools" / "measurement" / "live_log_server.py",
    ]
    if log_file is not None:
        command.append(log_file)
    command.extend([
        "--watch-dir",
        watch_dir,
        "--host",
        args.host,
        "--port",
        str(args.port),
        "--out-dir",
        output_dir,
        "--poll-interval-ms",
        str(args.poll_interval_ms),
        "--export-idle-seconds",
        str(args.export_idle_seconds),
    ])
    if args.inline_plotly:
        command.append("--inline-plotly")
    if args.open_browser:
        command.append("--open-browser")
    run(command)


def register_domain(args: argparse.Namespace) -> None:
    schema = Path(args.schema).expanduser().resolve()
    data = Path(args.data).expanduser().resolve()
    backends = Path(args.backends).expanduser().resolve()
    observability = (
        Path(args.observability_config).expanduser().resolve()
        if args.observability_config
        else None
    )
    for path in (schema, data, backends, observability):
        if path is not None and not path.is_file():
            raise FileNotFoundError(path)
    source = find_first_node(data, args.start_type)
    output = DATA_ROOT / "domains" / args.domain_id / "domain.json"
    write_domain_manifest(
        output,
        domain_id=args.domain_id,
        name=args.name or args.domain_id,
        description=args.description or "Runtime-registered PolyGraph domain.",
        schema=schema,
        data=data,
        backends=backends,
        start_uuid=args.start_uuid or source.uuid,
        observability=observability,
        max_depth=args.max_depth,
        direction=args.direction,
    )
    print(output)


def list_domains(_args: argparse.Namespace) -> None:
    rows: list[tuple[str, str, Path]] = []
    for manifest in discover_domain_manifests(DATA_ROOT):
        try:
            domain = load_domain_manifest(manifest)
        except Exception as exc:  # noqa: BLE001 - list invalid manifests without aborting
            print(f"invalid {manifest}: {exc}", file=sys.stderr)
            continue
        rows.append((domain.domain_id, domain.name, manifest))
    if not rows:
        print("No prepared domains. Run one of the prepare commands.")
        return
    width = max(len(item[0]) for item in rows)
    for domain_id, name, manifest in rows:
        print(f"{domain_id:<{width}}  {name}  {manifest}")


def quickstart_university(args: argparse.Namespace) -> None:
    manifest = prepare_university(args)
    domain = load_domain_manifest(manifest)
    if not args.no_build:
        build_project(argparse.Namespace(preset=args.preset))
    services_start(argparse.Namespace(timeout=args.timeout))
    load_domain(
        argparse.Namespace(
            domain=domain.domain_id,
            reset=True,
            batch_size=args.batch_size,
            only_source=[],
        )
    )
    if not args.no_run:
        run_domain(argparse.Namespace(domain=domain.domain_id, binary=args.binary, bfs_args=args.bfs_args))


def quickstart_ldbc(args: argparse.Namespace) -> None:
    if args.generate:
        manifest = generate_ldbc(args)
    else:
        if not args.input:
            raise ValueError("quickstart ldbc requires --input or --generate")
        manifest = prepare_ldbc(args)
    domain = load_domain_manifest(manifest)
    if not args.no_build:
        build_project(argparse.Namespace(preset=args.preset))
    services_start(argparse.Namespace(timeout=args.timeout))
    load_domain(
        argparse.Namespace(
            domain=domain.domain_id,
            reset=True,
            batch_size=args.batch_size,
            only_source=[],
        )
    )
    if not args.no_run:
        run_domain(argparse.Namespace(domain=domain.domain_id, binary=args.binary, bfs_args=args.bfs_args))


def run_benchmark(args: argparse.Namespace) -> None:
    from tools.measurement.benchmark_suite import load_suite_config, run_suite

    run_suite(
        load_suite_config(args.config),
        run_id=args.run_id,
        output_dir=args.output_dir,
        dry_run=args.dry_run,
        skip_build=args.skip_build,
        skip_services=args.skip_services,
        resume=args.resume,
    )


def benchmark_cache_configurations(args: argparse.Namespace) -> None:
    """Benchmark generated cache configs or reuse existing cache plans."""
    domain = resolve_domain(args.domain, DATA_ROOT)
    command: list[str | os.PathLike[str]] = [
        sys.executable,
        ROOT / "tools" / "measurement" / "benchmark_cache_configs.py",
        "--domain",
        domain.paths.manifest,
    ]
    if args.cache_configs:
        command.extend(["--cache-configs", *args.cache_configs])
    else:
        command.extend(["--cache-plans", *args.cache_plans])
    command.extend([
        "--repetitions",
        str(args.repetitions),
        "--max-depth",
        str(args.max_depth),
        "--direction",
        args.direction,
        "--transport",
        args.transport,
        "--http-host",
        args.http_host,
        "--http-port",
        str(args.http_port),
        "--http-startup-timeout",
        str(args.http_startup_timeout),
        "--http-request-timeout",
        str(args.http_request_timeout),
        "--http-response-mode",
        args.http_response_mode,
        "--cooldown-ms",
        str(args.cooldown_ms),
        "--redis-batch-size",
        str(args.redis_batch_size),
    ])
    for flag, value in (
        ("--benchmark-binary", args.benchmark_binary),
        ("--http-server-binary", args.http_server_binary),
        ("--http-server-config", args.http_server_config),
        ("--http-request-plan", args.http_request_plan),
        ("--output-dir", args.output_dir),
        ("--cache-root", args.cache_root),
        ("--planning-db", args.planning_db),
        ("--start-uuid", args.start_uuid),
        ("--start-uuids-file", args.start_uuids_file),
        ("--thread-count", args.thread_count),
        ("--neighbor-batch-size", args.neighbor_batch_size),
    ):
        if value is not None:
            command.extend([flag, str(value)])
    if args.skip_redis_load:
        command.append("--skip-redis-load")
    if args.redis_no_reset:
        command.append("--redis-no-reset")
    if args.overwrite:
        command.append("--overwrite")
    if args.bfs_args:
        command.append("--")
        command.extend(args.bfs_args)
    run(command)


def merge_benchmark_results_command(args: argparse.Namespace) -> None:
    from tools.measurement.benchmark_merge import collect_merge_inputs, merge_benchmark_results

    inputs = collect_merge_inputs(args.dataset, args.suite)
    merge_benchmark_results(
        inputs,
        output_dir=args.output_dir,
        run_id=args.run_id,
        conflict_policy=args.conflict_policy,
        reconstruct_logs=args.reconstruct_logs,
    )


def add_university_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--input",
        help=(
            "Existing University graph .json/.jsonl. It is converted to "
            "data/university/graph.jsonl; omit to generate a sample graph."
        ),
    )
    parser.add_argument("--schema", default=str(UNIVERSITY_SCHEMA))
    parser.add_argument("--nodes-per-type", type=int, default=3)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--start-type", default="Faculty")
    parser.add_argument("--max-depth", type=int, default=7)
    parser.add_argument("--direction", choices=["directed", "undirected", "both"], default="undirected")
    parser.add_argument("--arango-database", default="university")
    parser.add_argument(
        "--placement-strategy",
        choices=["schema", "identity-hash"],
        default="schema",
        help=(
            "Storage placement strategy. 'schema' (default) keeps every logical type on "
            "the backend assigned by its graph-shape source field; 'identity-hash' "
            "distributes individual objects across the ID routing layout. The strategy "
            "is also used when automatically migrating legacy --input graphs."
        ),
    )
    parser.add_argument(
        "--reroute-input",
        action="store_true",
        help=(
            "Regenerate ID MSB prefix routing and source fields for --input even when the "
            "input already contains idRouting metadata."
        ),
    )
    parser.add_argument(
        "--storage-backends",
        nargs="+",
        choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"],
        default=None,
        help=(
            "Optional ordered ID routing layout. By default it is read from idRouting/"
            "storageBackends in the schema, or derived from the schema type sources."
        ),
    )


def show_ldbc_statistics(args: argparse.Namespace) -> None:
    report = analyze_ldbc_dataset(
        args.input,
        placement_config=args.placement_config,
        default_source=args.default_source,
        storage_backends=args.storage_backends,
    )
    if args.json_output:
        write_json(Path(args.json_output).expanduser().resolve(), report)

    outputs: dict[str, Any] | None = None
    if args.output_dir or args.placement_config:
        output_dir = (
            Path(args.output_dir).expanduser().resolve()
            if args.output_dir
            else Path(report["input"]) / "statistics"
        )
        outputs = write_ldbc_statistics_outputs(
            report,
            output_dir,
            render_placement_graph=not args.no_placement_graph_render,
        )

    print_ldbc_statistics_report(report)
    if outputs is not None:
        print("\nGenerated statistics artifacts:")
        print(json.dumps(outputs, indent=2))


def launch_gui(_args: argparse.Namespace) -> None:
    from tools.polygraph_gui import main as gui_main

    gui_main()


def add_ldbc_options(parser: argparse.ArgumentParser, *, require_input: bool) -> None:
    parser.add_argument("--input", required=require_input, help="Extracted CsvBasic directory containing static/ and dynamic/.")
    parser.add_argument("--sf", help="Scale factor; inferred from the directory name when omitted.")
    parser.add_argument("--domain-id")
    parser.add_argument(
        "--output-dir",
        help=(
            "Directory for domain.json, graph.jsonl, schema, roots, and backend config. "
            "Defaults to data/ldbc-snb/sf<scale>."
        ),
    )
    parser.add_argument("--placement-config")
    parser.add_argument(
        "--cache-config",
        help="GPU/Redis/mmap acceleration-cache planning JSON applied after graph generation.",
    )
    parser.add_argument(
        "--cache-output-dir",
        help="Directory for cache-plan.json and binary cache artifacts; defaults to <output>/cache.",
    )
    parser.add_argument(
        "--gpu-distribution-config",
        help=(
            "Separate physical distribution JSON for authoritative GPU-owned vertices. "
            "Omitted means all authoritative GPU data is materialized on GPU:0."
        ),
    )
    parser.add_argument(
        "--gpu-distribution-output-dir",
        help=(
            "Directory for gpu-storage-plan.json and base GPU projection artifacts; "
            "defaults to <output>/gpu-storage."
        ),
    )
    parser.add_argument(
        "--storage-backends",
        nargs="+",
        choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"],
        default=["Neo4J", "ArangoDB", "PostgreSQL"],
        help="Ordered backend layout encoded in the most-significant bits of each uint64 ID.",
    )
    parser.add_argument(
        "--default-source",
        choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"],
        default="Neo4J",
        help="Legacy fallback used only by an explicit placement config.",
    )
    parser.add_argument("--arango-database")
    parser.add_argument("--start-type", default="Person")
    parser.add_argument("--root-count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--max-depth", type=int, default=7)
    parser.add_argument("--direction", choices=["directed", "undirected", "both"], default="undirected")
    parser.add_argument(
        "--no-placement-graph-render",
        action="store_true",
        help=(
            "Still generate placement-report.json/.txt and placement-graph.dot, "
            "but skip Graphviz SVG/PDF rendering."
        ),
    )
    parser.add_argument(
        "--validation-mode",
        choices=["basic", "full", "none"],
        default="basic",
        help=(
            "Post-conversion validation. 'basic' (default) uses bounded-memory "
            "conversion checks/counts; 'full' performs the legacy exact duplicate/endpoint "
            "scan and is intended only for smaller graphs; 'none' skips it."
        ),
    )


def add_datagen_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--tag", default="v1.0.0")
    parser.add_argument("--memory", default="6G")
    parser.add_argument("--update-partitions", type=int, default=1)
    parser.add_argument("--base-image", default="eclipse-temurin:8-jdk-jammy")
    parser.add_argument("--force", action="store_true")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="One Python entry point for preparing, loading, building, and running PolyGraph domains."
    )
    commands = parser.add_subparsers(dest="command", required=True)

    gui = commands.add_parser("gui", help="Open the generated desktop command interface.")
    gui.set_defaults(handler=launch_gui)

    prepare = commands.add_parser("prepare", help="Prepare a runtime domain from source data.")
    prepare_domains = prepare.add_subparsers(dest="prepare_domain", required=True)
    prepare_university_parser = prepare_domains.add_parser("university")
    add_university_options(prepare_university_parser)
    prepare_university_parser.set_defaults(handler=prepare_university)
    prepare_ldbc_parser = prepare_domains.add_parser("ldbc")
    add_ldbc_options(prepare_ldbc_parser, require_input=True)
    prepare_ldbc_parser.set_defaults(handler=prepare_ldbc)

    generate = commands.add_parser("generate", help="Generate source datasets and prepare their domains.")
    generate_domains = generate.add_subparsers(dest="generate_domain", required=True)
    generate_ldbc_parser = generate_domains.add_parser("ldbc")
    add_ldbc_options(generate_ldbc_parser, require_input=False)
    add_datagen_options(generate_ldbc_parser)
    generate_ldbc_parser.set_defaults(handler=generate_ldbc)

    stats = commands.add_parser("stats", help="Inspect raw or prepared graph datasets.")
    stats_domains = stats.add_subparsers(dest="stats_domain", required=True)
    stats_ldbc = stats_domains.add_parser(
        "ldbc", help="Print raw LDBC table row counts and degree statistics."
    )
    stats_ldbc.add_argument(
        "--input", required=True, help="Raw LDBC directory containing static/ and dynamic/."
    )
    stats_ldbc.add_argument("--json-output", help="Optional machine-readable report path.")
    stats_ldbc.add_argument(
        "--placement-config",
        help=(
            "Resolve raw node/relationship counts through this placement.config.json "
            "and generate backend-container reports."
        ),
    )
    stats_ldbc.add_argument(
        "--output-dir",
        help=(
            "Write ldbc-statistics.json and placement-report/graph artifacts here. "
            "When omitted with --placement-config, defaults to <dataset>/statistics."
        ),
    )
    stats_ldbc.add_argument(
        "--storage-backends",
        nargs="+",
        choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"],
        default=["Neo4J", "ArangoDB", "PostgreSQL"],
        help="Fallback routing order when it is not declared by the placement config.",
    )
    stats_ldbc.add_argument(
        "--default-source",
        choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"],
        default="Neo4J",
        help="Legacy fallback for placement configs that only contain per-type overrides.",
    )
    stats_ldbc.add_argument(
        "--no-placement-graph-render",
        action="store_true",
        help="Generate placement JSON/TXT/DOT but skip Graphviz SVG/PDF rendering.",
    )
    stats_ldbc.set_defaults(handler=show_ldbc_statistics)

    build = commands.add_parser("build", help="Configure and build the C++ project.")
    build.add_argument("--preset", default="gpu-bfs")
    build.set_defaults(handler=build_project)

    services = commands.add_parser("services", help="Start or stop local graph databases.")
    services_sub = services.add_subparsers(dest="service_action", required=True)
    start = services_sub.add_parser("start")
    start.add_argument("--timeout", type=int, default=180)
    start.set_defaults(handler=services_start)
    stop = services_sub.add_parser("stop")
    stop.add_argument("--volumes", action="store_true", help="Also remove persistent database volumes.")
    stop.set_defaults(handler=services_stop)

    instance = commands.add_parser(
        "instance",
        help="Manage one persistent scale-factor/distribution Compose environment.",
    )
    instance.add_argument(
        "scale_factor",
        help="Scale factor (for example 1, 1.0, sf1 or sf1_0). New instances are created lazily.",
    )
    instance.add_argument(
        "distribution",
        help="Distribution name (for example schema-based). Registry entries are optional overrides.",
    )
    instance_actions = instance.add_subparsers(dest="instance_action", required=True)

    instance_service_choices = ["neo4j", "arangodb", "postgresql"]
    instance_activate = instance_actions.add_parser(
        "activate",
        help="Stop another active PolyGraph SF (preserve volumes), then create/start this one.",
    )
    instance_activate.add_argument(
        "--services", nargs="+", choices=instance_service_choices,
        help="Start only these storage services. Omit to start the complete instance.",
    )
    instance_activate.set_defaults(handler=manage_compose_instance)

    instance_up = instance_actions.add_parser("up", help="Create/start and wait until ready.")
    instance_up.add_argument(
        "--services", nargs="+", choices=instance_service_choices,
        help="Start only these storage services. Omit to start the complete instance.",
    )
    instance_up.set_defaults(handler=manage_compose_instance)

    instance_start = instance_actions.add_parser("start", help="Start stopped containers.")
    instance_start.add_argument(
        "--services", nargs="+", choices=instance_service_choices,
        help="Start only these storage services. Omit to start the complete instance.",
    )
    instance_start.set_defaults(handler=manage_compose_instance)

    instance_stop = instance_actions.add_parser("stop", help="Stop containers and preserve volumes.")
    instance_stop.set_defaults(handler=manage_compose_instance)

    instance_restart = instance_actions.add_parser("restart", help="Restart and wait until ready.")
    instance_restart.add_argument("--recreate", action="store_true", help="Force container recreation.")
    instance_restart.add_argument(
        "--services", nargs="+", choices=instance_service_choices,
        help="Restart only these storage services. Omit to restart the complete instance.",
    )
    instance_restart.set_defaults(handler=manage_compose_instance)

    instance_down = instance_actions.add_parser(
        "down", help="Remove containers/network while preserving volumes."
    )
    instance_down.set_defaults(handler=manage_compose_instance)

    instance_status = instance_actions.add_parser("status", aliases=["ps"], help="Show status.")
    instance_status.set_defaults(handler=manage_compose_instance)

    instance_logs = instance_actions.add_parser("logs", help="Show service logs.")
    instance_logs.add_argument("services", nargs="*", help="Optional service names.")
    instance_logs.add_argument("--follow", "-f", action="store_true")
    instance_logs.add_argument("--tail", type=int, default=120)
    instance_logs.set_defaults(handler=manage_compose_instance)

    instance_load = instance_actions.add_parser(
        "load", help="Start, wait, and load/reset the configured prepared domain."
    )
    instance_load.add_argument(
        "domain",
        nargs="?",
        help="Optional domain override; defaults to storage_layer/environments.json.",
    )
    instance_load.add_argument("--no-reset", action="store_true")
    instance_load.add_argument("--batch-size", type=int, default=20000)
    instance_load.add_argument(
        "--workers", type=int, default=4,
        help="Parallel ArangoDB import workers (default: 4).",
    )
    instance_load.add_argument(
        "--postgres-indexes", action="store_true",
        help="Create PostgreSQL traversal indexes after loading (default: off).",
    )
    instance_load.add_argument(
        "--services", nargs="+", choices=instance_service_choices,
        help="Start only these storage services before loading.",
    )
    instance_load.add_argument(
        "--only-source", choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"], action="append", default=[]
    )
    instance_load.set_defaults(handler=manage_compose_instance)

    instance_cache_sync = instance_actions.add_parser(
        "cache-sync",
        help="Start the instance and update only cache_residency node attributes from the active graph.",
    )
    instance_cache_sync.add_argument(
        "domain", nargs="?",
        help="Optional domain override; defaults to storage_layer/environments.json.",
    )
    instance_cache_sync.add_argument("--batch-size", type=int, default=20000)
    instance_cache_sync.add_argument(
        "--only-source", choices=["Neo4J", "ArangoDB", "PostgreSQL"], action="append", default=[]
    )
    instance_cache_sync.set_defaults(handler=manage_compose_instance)

    instance_benchmark = instance_actions.add_parser(
        "benchmark", help="Run an existing benchmark-suite configuration."
    )
    instance_benchmark.add_argument("config")
    instance_benchmark.add_argument("benchmark_args", nargs=argparse.REMAINDER)
    instance_benchmark.set_defaults(handler=manage_compose_instance)

    instance_destroy = instance_actions.add_parser(
        "destroy", help="Delete containers and the loaded database volumes."
    )
    instance_destroy.add_argument("--yes", action="store_true", help="Confirm permanent deletion.")
    instance_destroy.set_defaults(handler=manage_compose_instance)

    instance_project = instance_actions.add_parser("project", help="Print the Compose project name.")
    instance_project.set_defaults(handler=manage_compose_instance)

    instance_options = instance_actions.add_parser(
        "options", help="Print the merged configuration and environment variables."
    )
    instance_options.add_argument("--show-secrets", action="store_true")
    instance_options.set_defaults(handler=manage_compose_instance)

    load = commands.add_parser("load", help="Load a prepared domain into its persistent backends.")
    load.add_argument("domain")
    load.add_argument("--reset", action="store_true")
    load.add_argument("--batch-size", type=int, default=20000)
    load.add_argument(
        "--workers", type=int, default=4,
        help="Parallel ArangoDB import workers (default: 4).",
    )
    load.add_argument(
        "--postgres-indexes", action="store_true",
        help="Create PostgreSQL traversal indexes after loading (default: off).",
    )
    load.add_argument(
        "--only-source", action="append", default=[],
        help="Load/inspect only one configured source (e.g. Neo4J, ArangoDB, GPU:0). May be repeated.",
    )
    load.set_defaults(handler=load_domain)

    gpu_storage = commands.add_parser(
        "gpu-storage",
        help="Manage authoritative GPU storage independently of acceleration caches.",
    )
    gpu_storage_actions = gpu_storage.add_subparsers(dest="gpu_storage_action", required=True)
    gpu_storage_build = gpu_storage_actions.add_parser(
        "build",
        help="Materialize/rebalance authoritative GPU-owned vertices across GPU devices.",
    )
    gpu_storage_build.add_argument("domain")
    gpu_storage_build.add_argument(
        "--config",
        help="GPU distribution JSON. Omitted means all authoritative GPU data on GPU:0.",
    )
    gpu_storage_build.add_argument(
        "--output-dir",
        help="GPU storage root; defaults to <domain>/gpu-storage.",
    )
    gpu_storage_build.add_argument(
        "--planning-db",
        help="Reusable graph/index SQLite DB; defaults beside graph.jsonl as cache-plan.sqlite3.",
    )
    gpu_storage_build.set_defaults(handler=build_gpu_storage)

    cache = commands.add_parser("cache", help="Manage prepared acceleration caches.")
    cache_actions = cache.add_subparsers(dest="cache_action", required=True)
    cache_build = cache_actions.add_parser(
        "build",
        help="Build independent cache plans while reusing one cache-plan.sqlite3 graph/index database.",
    )
    cache_build.add_argument("domain")
    cache_build.add_argument(
        "--config", nargs="+", required=True,
        help="One or more cache configuration JSON files. Each becomes its own cache directory.",
    )
    cache_build.add_argument("--output-dir", help="Cache root; defaults to <domain>/caches.")
    cache_build.add_argument(
        "--planning-db",
        help="Reusable graph/index SQLite DB; defaults beside graph.jsonl as cache-plan.sqlite3.",
    )
    cache_build.add_argument("--load-redis", action="store_true", help="Materialize Redis for each Redis-enabled plan.")
    cache_build.add_argument("--redis-no-reset", action="store_true", help="Do not FLUSHDB before Redis materialization.")
    cache_build.add_argument("--redis-batch-size", type=int, default=10000)
    cache_build.set_defaults(handler=build_cache_plans)

    redis_load = cache_actions.add_parser("redis-load", help="Load generated adjacency blobs into Redis.")
    redis_load.add_argument("domain")
    redis_load.add_argument("--cache-plan", help="cache-plan.json to materialize; otherwise use domain.paths.cachePlan.")
    redis_load.add_argument("--no-reset", action="store_true", help="Do not FLUSHDB before loading (reset is the safe default).")
    redis_load.add_argument("--batch-size", type=int, default=10000)
    redis_load.set_defaults(handler=load_redis_cache)

    cache_reconfigure = cache_actions.add_parser(
        "reconfigure",
        help="Re-plan GPU/Redis/mmap caches from an already generated graph without re-reading raw SNB CSVs.",
    )
    cache_reconfigure.add_argument("domain")
    cache_reconfigure.add_argument("--cache-config", required=True)
    cache_reconfigure.add_argument(
        "--output-dir",
        help="Cache artifact directory; defaults to <domain>/cache and replaces the previous plan.",
    )
    cache_reconfigure.add_argument(
        "--resume", action="store_true",
        help=(
            "Resume an interrupted cache reconfiguration from an existing planning SQLite DB. "
            "Skips graph indexing/cache selection, reuses completed Redis/mmap artifacts, and "
            "continues a partial .graph.jsonl.cache.tmp rewrite when present."
        ),
    )
    cache_reconfigure.add_argument(
        "--planning-db",
        help=(
            "Planning SQLite DB to resume from; defaults to <output-dir>/cache-plan.sqlite3. "
            "Useful when the preserved DB was moved to another disk."
        ),
    )
    cache_reconfigure.add_argument(
        "--reload-redis", action="store_true",
        help="After planning, reset/reload Redis from the newly generated artifact.",
    )
    cache_reconfigure.add_argument("--redis-batch-size", type=int, default=10000)
    cache_reconfigure.set_defaults(handler=reconfigure_cache)

    cache_sync = cache_actions.add_parser(
        "sync-residency",
        help="Update only Neo4j/ArangoDB cache_residency node attributes from the active graph.",
    )
    cache_sync.add_argument("domain")
    cache_sync.add_argument("--batch-size", type=int, default=20000)
    cache_sync.add_argument(
        "--only-source", choices=["Neo4J", "ArangoDB", "PostgreSQL"], action="append", default=[]
    )
    cache_sync.set_defaults(handler=sync_domain_cache_residency)

    run_parser = commands.add_parser("run", help="Run the C++ BFS executable for a prepared domain.")
    run_parser.add_argument("domain")
    run_parser.add_argument("--binary")
    run_parser.add_argument(
        "--run-id",
        help=(
            "Measurement run identifier. BFS result CSVs and event logs are written below "
            "measurements/<run-id>/. Defaults to <domain>-<UTC timestamp>."
        ),
    )
    roots = run_parser.add_mutually_exclusive_group()
    roots.add_argument("--start-uuid", help="Run from one uint64 global ID instead of the domain default.")
    roots.add_argument(
        "--start-uuids-file",
        help="Run every non-empty, non-comment uint64 global ID in this text file.",
    )
    run_parser.add_argument(
        "--repetitions",
        type=int,
        default=1,
        help="Number of BFS executions per start ID (default: 1).",
    )
    run_parser.add_argument(
        "--overwrite-logs",
        action="store_true",
        help="Allow existing logs, result CSVs, and measurement metadata to be replaced.",
    )
    run_parser.add_argument(
        "--thread-count",
        type=int,
        help="BFS worker count reserved for the C++ thread pool.",
    )
    run_parser.add_argument(
        "--neighbor-batch-size",
        type=int,
        help="Maximum frontier vertices sent in one Neo4j/ArangoDB request.",
    )
    run_parser.set_defaults(handler=run_domain)

    analyze = commands.add_parser(
        "analyze-logs",
        help="Generate per-directory and global average event tables and HTML visualizations.",
    )
    analyze.add_argument(
        "run",
        help="Measurement run ID or path to a measurements/<run-id> directory.",
    )
    analyze.add_argument(
        "--out-dir",
        help="Output directory. Defaults to measurements/<run-id>/log_reconstruction.",
    )
    analyze.add_argument(
        "--inline-plotly",
        action="store_true",
        help="Embed Plotly into dashboard.html for offline viewing.",
    )
    analyze.add_argument(
        "--no-averages",
        action="store_true",
        help="Reconstruct individual runs without generating average executions.",
    )
    analyze.set_defaults(handler=analyze_measurement_logs)

    live_logs = commands.add_parser(
        "live-logs",
        help="Serve a live Timeline: by thread dashboard and attach JSONL logs from the UI.",
    )
    live_logs.add_argument(
        "log_file",
        nargs="?",
        help="Optional initial JSONL event log; it may be created after the server starts.",
    )
    live_logs.add_argument(
        "--watch-dir",
        help="Directory exposed by the UI log selector. Defaults to measurements/ or the initial log's parent.",
    )
    live_logs.add_argument(
        "--out-dir",
        help="Root for per-log static dashboards. Defaults to <watch-dir>/live_timeline.",
    )
    live_logs.add_argument("--host", default="127.0.0.1")
    live_logs.add_argument("--port", type=int, default=8765)
    live_logs.add_argument("--poll-interval-ms", type=float, default=50.0)
    live_logs.add_argument(
        "--export-idle-seconds",
        type=float,
        default=1.0,
        help="Auto-save when all spans are closed and the log is idle; 0 disables.",
    )
    live_logs.add_argument(
        "--inline-plotly",
        action="store_true",
        help="Embed Plotly.js in the live page and the final static dashboard.",
    )
    live_logs.add_argument("--open-browser", action="store_true")
    live_logs.set_defaults(handler=serve_live_measurement_log)

    benchmark = commands.add_parser(
        "benchmark",
        help="Run distributed, full-backend, native, and reference BFS scenarios from JSON.",
    )
    benchmark.add_argument("config", help="Path to the benchmark suite JSON configuration.")
    benchmark.add_argument("--run-id", help="Override the run ID from the configuration.")
    benchmark.add_argument("--output-dir", help="Override the benchmark output directory.")
    benchmark.add_argument("--dry-run", action="store_true", help="Print setup and benchmark commands without executing them.")
    benchmark.add_argument("--skip-build", action="store_true")
    benchmark.add_argument("--skip-services", action="store_true")
    benchmark.add_argument(
        "--resume",
        action="store_true",
        help="Continue the same non-empty run directory and rerun only failed or missing work.",
    )
    benchmark.set_defaults(handler=run_benchmark)

    benchmark_caches = commands.add_parser(
        "benchmark-caches",
        help="Compare generated cache configurations or existing cache plans on one immutable graph.",
    )
    benchmark_caches.add_argument("domain", help="Domain ID or domain.json path.")
    cache_inputs = benchmark_caches.add_mutually_exclusive_group(required=True)
    cache_inputs.add_argument(
        "--cache-configs",
        nargs="+",
        help="Cache configuration JSON files to generate/materialize before benchmarking.",
    )
    cache_inputs.add_argument(
        "--cache-plans",
        nargs="+",
        help="Existing cache-plan.json files to reuse without regenerating cache artifacts.",
    )
    benchmark_caches.add_argument(
        "--transport",
        choices=["binary", "http"],
        default="binary",
        help="Execute BFS in-process with the benchmark binary or through the real HTTP server endpoint.",
    )
    benchmark_caches.add_argument("--benchmark-binary")
    benchmark_caches.add_argument("--http-server-binary")
    benchmark_caches.add_argument(
        "--http-server-config",
        help="Base Drogon config. The benchmark writes a per-cache copy with domain/cache/observability overrides.",
    )
    benchmark_caches.add_argument("--http-host", default="0.0.0.0")
    benchmark_caches.add_argument(
        "--http-port",
        type=int,
        default=8080,
        help="Server port. 8080 matches observability/prometheus/prometheus.yml by default.",
    )
    benchmark_caches.add_argument(
        "--http-startup-timeout",
        type=float,
        default=0.0,
        help="Seconds to wait for /healthz; 0 means no overall startup timeout (default).",
    )
    benchmark_caches.add_argument(
        "--http-request-timeout",
        type=float,
        default=0.0,
        help="Per-BFS HTTP timeout; 0 waits indefinitely so benchmark repetitions cannot overlap (default).",
    )
    benchmark_caches.add_argument(
        "--http-response-mode",
        choices=["summary", "full"],
        default="summary",
        help="summary measures BFS through HTTP without transferring the full distance map; full measures the complete API response.",
    )
    benchmark_caches.add_argument(
        "--http-request-plan",
        help=(
            "JSON schedule for HTTP BFS requests. Each request may set id, atMs relative to "
            "server readiness, startUuid, and HTTP BFS payload overrides."
        ),
    )
    benchmark_caches.add_argument("--output-dir")
    benchmark_caches.add_argument("--cache-root")
    benchmark_caches.add_argument("--planning-db")
    roots = benchmark_caches.add_mutually_exclusive_group()
    roots.add_argument("--start-uuid")
    roots.add_argument("--start-uuids-file")
    benchmark_caches.add_argument("--repetitions", type=int, default=5)
    benchmark_caches.add_argument("--max-depth", type=int, default=10)
    benchmark_caches.add_argument("--direction", choices=["directed", "undirected", "both"], default="both")
    benchmark_caches.add_argument("--cooldown-ms", type=int, default=0)
    benchmark_caches.add_argument("--thread-count", type=int)
    benchmark_caches.add_argument("--neighbor-batch-size", type=int)
    benchmark_caches.add_argument("--skip-redis-load", action="store_true")
    benchmark_caches.add_argument("--redis-no-reset", action="store_true")
    benchmark_caches.add_argument("--redis-batch-size", type=int, default=10000)
    benchmark_caches.add_argument("--overwrite", action="store_true")
    benchmark_caches.set_defaults(handler=benchmark_cache_configurations)

    benchmark_merge = commands.add_parser(
        "benchmark-merge",
        help="Merge partial or resumed benchmark result directories without rerunning them.",
    )
    benchmark_merge.add_argument(
        "--dataset",
        action="append",
        default=[],
        metavar="TARGET=PATH",
        help=(
            "Merge one dataset result directory into TARGET. Repeat to combine multiple "
            "partial/resume runs under the same dataset name."
        ),
    )
    benchmark_merge.add_argument(
        "--suite",
        action="append",
        default=[],
        help="Import every datasets/* result directory from an existing benchmark suite.",
    )
    benchmark_merge.add_argument("--output-dir", required=True)
    benchmark_merge.add_argument("--run-id", required=True)
    benchmark_merge.add_argument(
        "--conflict-policy",
        choices=["prefer-ok", "keep-first", "keep-last", "error"],
        default="prefer-ok",
        help=(
            "Resolve duplicate scenario/system/phase/repetition/start-ID records. "
            "prefer-ok replaces failed attempts with successful retries."
        ),
    )
    benchmark_merge.add_argument(
        "--reconstruct-logs",
        action="store_true",
        help="Regenerate reconstruction only when PolyGraph *.events.jsonl event files exist.",
    )
    benchmark_merge.set_defaults(handler=merge_benchmark_results_command)

    register = commands.add_parser("register", help="Register another domain manifest without recompiling.")
    register.add_argument("domain_id")
    register.add_argument("--schema", required=True)
    register.add_argument("--data", required=True)
    register.add_argument("--backends", required=True)
    register.add_argument(
        "--observability-config",
        help="Observability JSON referenced by domain.json; defaults to config/observability/observability.json.",
    )
    register.add_argument("--name")
    register.add_argument("--description")
    register.add_argument("--start-type")
    register.add_argument("--start-uuid")
    register.add_argument("--max-depth", type=int, default=7)
    register.add_argument("--direction", choices=["directed", "undirected", "both"], default="undirected")
    register.set_defaults(handler=register_domain)

    listing = commands.add_parser("list", help="List prepared runtime domains.")
    listing.set_defaults(handler=list_domains)

    quickstart = commands.add_parser("quickstart", help="Prepare, build, start databases, load, and optionally run.")
    quick_domains = quickstart.add_subparsers(dest="quick_domain", required=True)
    quick_university = quick_domains.add_parser("university")
    add_university_options(quick_university)
    quick_university.add_argument("--preset", default="gpu-bfs")
    quick_university.add_argument("--batch-size", type=int, default=5000)
    quick_university.add_argument("--timeout", type=int, default=180)
    quick_university.add_argument("--binary")
    quick_university.add_argument("--no-build", action="store_true")
    quick_university.add_argument("--no-run", action="store_true")
    quick_university.set_defaults(handler=quickstart_university)

    quick_ldbc = quick_domains.add_parser("ldbc")
    add_ldbc_options(quick_ldbc, require_input=False)
    add_datagen_options(quick_ldbc)
    quick_ldbc.add_argument("--generate", action="store_true", help="Run official Datagen instead of using --input.")
    quick_ldbc.add_argument("--preset", default="cpu-bfs")
    quick_ldbc.add_argument("--batch-size", type=int, default=5000)
    quick_ldbc.add_argument("--timeout", type=int, default=180)
    quick_ldbc.add_argument("--binary")
    quick_ldbc.add_argument("--no-build", action="store_true")
    quick_ldbc.add_argument("--no-run", action="store_true")
    quick_ldbc.set_defaults(handler=quickstart_ldbc)

    return parser


def main() -> int:
    parser = build_parser()
    argv = sys.argv[1:]
    bfs_args: list[str] = []
    if "--" in argv:
        separator = argv.index("--")
        bfs_args = argv[separator + 1 :]
        argv = argv[:separator]
    args = parser.parse_args(argv)
    args.bfs_args = bfs_args
    try:
        args.handler(args)
        return 0
    except (FileNotFoundError, ValueError, RuntimeError, TimeoutError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
