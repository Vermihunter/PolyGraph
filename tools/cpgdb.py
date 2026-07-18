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
from pathlib import Path
from typing import Any, Sequence

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.graphio.domain import (
    discover_domain_manifests,
    find_first_node,
    generate_sample_graph,
    load_domain_manifest,
    materialize_graph_jsonl,
    resolve_domain,
    select_roots,
    validate_graph,
    write_backend_config_for_sources,
    write_domain_manifest,
    write_roots_csv,
)
from tools.graphio.loaders import load_graph
from tools.graphio.reader import open_graph_reader
from tools.ldbc_snb.convert import convert_dataset

DATA_ROOT = ROOT / "data"
COMPOSE_FILE = ROOT / "storage_layer" / "docker-compose.yml"
UNIVERSITY_SCHEMA = DATA_ROOT / "university" / "schema" / "sample-university-domain.graph-shape.json"
DATAGEN_DOCKERFILE = ROOT / "tools" / "ldbc_snb" / "Dockerfile.datagen"


def step(message: str) -> None:
    print(f"\n[cpgdb] {message}", flush=True)


def run(command: Sequence[str | os.PathLike[str]], *, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    rendered = [str(item) for item in command]
    print("+", " ".join(rendered), flush=True)
    subprocess.run(rendered, cwd=cwd, env=env, check=True)


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
    return f"sf{scale_factor.replace('.', '_')}"


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
        graph_routing = open_graph_reader(graph).metadata().get("uuidRouting")
        if not graph_routing:
            raise ValueError("Imported graph has no uuidRouting metadata")
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
    graph_routing = graph_metadata.get("uuidRouting")
    if not graph_routing:
        raise ValueError("Generated graph has no uuidRouting metadata")
    routing_backends = tuple(str(value) for value in graph_routing)

    backends = output_root / "backends.json"
    database = args.arango_database or "university"
    write_backend_config_for_sources(routing_backends, backends, arango_database=database)

    validation = validate_graph(graph)
    write_json(output_root / "validation.json", validation)
    if not validation["valid"]:
        raise RuntimeError(f"University graph validation failed: {validation['errors'][:3]}")

    source = find_first_node(graph, args.start_type)
    manifest = write_domain_manifest(
        output_root / "domain.json",
        domain_id="university",
        name="University Ecosystem",
        description="University graph domain using the existing graph-shape schema.",
        schema=schema,
        data=graph,
        backends=backends,
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
    )
    print(json.dumps(result, indent=2))

    validation = validate_graph(graph)
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
        start_uuid=roots[0].uuid,
        max_depth=args.max_depth,
        direction=args.direction,
    )
    write_benchmark_config(domain_id, schema=schema, data=graph, backends=backends)
    write_json(
        output_root / "dataset-manifest.json",
        {
            "dataset": "LDBC SNB Interactive v1",
            "scale_factor": scale_factor,
            "input": str(social_network),
            "input_sections": ["static", "dynamic"],
            "update_streams_loaded": False,
            "outputs": {
                "domain": str(manifest),
                "graph": str(graph),
                "schema": str(schema),
                "backends": str(backends),
                "roots": str(roots_path),
                "validation": str(validation_path),
            },
            "sha256": {
                "graph": sha256(graph),
                "schema": sha256(schema),
                "backends": sha256(backends),
            },
        },
    )
    step("LDBC SNB domain is ready")
    print(f"domain id: {domain_id}\nmanifest:  {manifest}\ngraph:     {graph}\nschema:    {schema}")
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

    image = f"cpgdb/ldbc-snb-datagen:{tag}"
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


def wait_for_neo4j(timeout: int) -> None:
    url = "http://127.0.0.1:7474/db/neo4j/tx/commit"
    payload = json.dumps({"statements": [{"statement": "RETURN 1 AS ready"}]}).encode("utf-8")
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None

    while time.monotonic() < deadline:
        request = urllib.request.Request(url, data=payload, method="POST")
        request.add_header("Accept", "application/json")
        request.add_header("Content-Type", "application/json")
        request.add_header("Authorization", _basic_auth("neo4j", "password"))
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


def wait_for_arangodb(timeout: int) -> None:
    url = "http://127.0.0.1:8529/_api/version"
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None

    while time.monotonic() < deadline:
        request = urllib.request.Request(url, method="GET")
        request.add_header("Accept", "application/json")
        request.add_header("Authorization", _basic_auth("root", "rootpassword"))
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
    step("Starting Neo4j and ArangoDB")
    run(["docker", "compose", "-f", COMPOSE_FILE, "up", "-d"])
    # A listening TCP port is not sufficient: Neo4j can accept a connection
    # before its database and transaction endpoint are ready, or briefly reset
    # connections while startup/plugin initialization completes.
    wait_for_port("127.0.0.1", 7474, "Neo4j", args.timeout)
    wait_for_neo4j(args.timeout)
    wait_for_port("127.0.0.1", 8529, "ArangoDB", args.timeout)
    wait_for_arangodb(args.timeout)


def services_stop(args: argparse.Namespace) -> None:
    require("docker")
    command: list[str | os.PathLike[str]] = ["docker", "compose", "-f", COMPOSE_FILE, "down"]
    if args.volumes:
        command.append("--volumes")
    run(command)


def load_domain(args: argparse.Namespace) -> None:
    domain = resolve_domain(args.domain, DATA_ROOT)
    step(f"Loading domain {domain.domain_id}")
    selected = args.only_source or None
    result = load_graph(
        domain.paths.data,
        domain.paths.backends,
        reset=args.reset,
        batch_size=args.batch_size,
        only_sources=selected,
    )
    print(json.dumps(result, indent=2))


def backend_config_requires_gpu(config_path: Path) -> bool:
    config = json.loads(config_path.read_text(encoding="utf-8"))
    routing = {str(value) for value in config.get("uuidRouting", [])}
    enabled = {
        str(entry.get("id", ""))
        for entry in config.get("backends", [])
        if entry.get("enabled", True)
    }
    return "GPU" in routing and "GPU" in enabled


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
    candidates = [ROOT / "build" / name / "bin" / "bfs" for name in names]
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
        f"No {requirement}BFS executable was found. Run: python3 tools/cpgdb.py build "
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
        raise FileNotFoundError(f"Start UUID file does not exist: {source}")

    values = [
        line.strip()
        for line in source.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not values:
        raise ValueError(f"Start UUID file is empty: {source}")
    return values


def run_domain(args: argparse.Namespace) -> None:
    domain = resolve_domain(args.domain, DATA_ROOT)
    binary = find_bfs_binary(domain.paths.backends, getattr(args, "binary", None))
    extras = list(getattr(args, "bfs_args", None) or [])
    if extras and extras[0] == "--":
        extras.pop(0)

    if has_cli_option(extras, "--log-file"):
        raise ValueError(
            "Do not pass --log-file directly. Use --run-id; cpgdb.py creates "
            "measurements/<run-id>/logs/<start-uuid>/<repetition>.jsonl."
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
        "--logical-graph-schema",
        domain.paths.schema,
        "--data",
        domain.paths.data,
        "--backend-config",
        domain.paths.backends,
    ]

    if not has_cli_option(extras, "--max-depth"):
        common_command.extend(["--max-depth", str(domain.max_depth)])
    if not has_cli_option(extras, "--direction"):
        common_command.extend(["--direction", domain.direction])

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
        "log_file",
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
            uuid_directory = safe_path_component(start_uuid, label="start UUID")
            for repetition in range(1, repetitions + 1):
                current_run += 1
                log_file = measurement_root / "logs" / uuid_directory / f"{repetition}.jsonl"
                output_csv = measurement_root / "outputs" / uuid_directory / f"{repetition}.csv"
                log_file.parent.mkdir(parents=True, exist_ok=True)
                output_csv.parent.mkdir(parents=True, exist_ok=True)

                existing = [path for path in (log_file, output_csv) if path.exists()]
                if existing and not overwrite_logs:
                    rendered = ", ".join(str(path) for path in existing)
                    raise ValueError(
                        f"Run artifacts already exist: {rendered}. "
                        "Use a different --run-id or pass --overwrite-logs."
                    )
                if overwrite_logs:
                    log_file.unlink(missing_ok=True)
                    output_csv.unlink(missing_ok=True)

                command = [
                    *common_command,
                    "--start-uuid",
                    start_uuid,
                    "--log-file",
                    log_file,
                    *extras,
                ]

                step(
                    f"Running BFS for {domain.domain_id}: UUID {start_uuid}, "
                    f"repetition {repetition}/{repetitions} ({current_run}/{total_runs})"
                )
                rendered_command = [str(item) for item in command]
                print("+", " ".join(rendered_command), flush=True)

                started = time.perf_counter()
                with output_csv.open("wb") as output_handle:
                    completed = subprocess.run(
                        rendered_command,
                        cwd=ROOT,
                        stdout=output_handle,
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
                        "log_file": log_file.relative_to(measurement_root),
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


def register_domain(args: argparse.Namespace) -> None:
    schema = Path(args.schema).expanduser().resolve()
    data = Path(args.data).expanduser().resolve()
    backends = Path(args.backends).expanduser().resolve()
    for path in (schema, data, backends):
        if not path.is_file():
            raise FileNotFoundError(path)
    source = find_first_node(data, args.start_type)
    output = DATA_ROOT / "domains" / args.domain_id / "domain.json"
    write_domain_manifest(
        output,
        domain_id=args.domain_id,
        name=args.name or args.domain_id,
        description=args.description or "Runtime-registered CPGDB domain.",
        schema=schema,
        data=data,
        backends=backends,
        start_uuid=args.start_uuid or source.uuid,
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
    )


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
            "distributes individual objects across the UUID routing layout. The strategy "
            "is also used when automatically migrating legacy --input graphs."
        ),
    )
    parser.add_argument(
        "--reroute-input",
        action="store_true",
        help=(
            "Regenerate UUID suffix routing and source fields for --input even when the "
            "input already contains uuidRouting metadata."
        ),
    )
    parser.add_argument(
        "--storage-backends",
        nargs="+",
        choices=["Neo4J", "ArangoDB", "GPU"],
        default=None,
        help=(
            "Optional ordered UUID routing layout. By default it is read from uuidRouting/"
            "storageBackends in the schema, or derived from the schema type sources."
        ),
    )


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
        "--storage-backends",
        nargs="+",
        choices=["Neo4J", "ArangoDB", "GPU"],
        default=["Neo4J", "ArangoDB", "GPU"],
        help="Ordered UUID storage layout used for deterministic per-object routing.",
    )
    parser.add_argument(
        "--default-source",
        choices=["Neo4J", "ArangoDB", "GPU"],
        default="Neo4J",
        help="Legacy fallback used only by an explicit placement config.",
    )
    parser.add_argument("--arango-database")
    parser.add_argument("--start-type", default="Person")
    parser.add_argument("--root-count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--max-depth", type=int, default=7)
    parser.add_argument("--direction", choices=["directed", "undirected", "both"], default="undirected")


def add_datagen_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--tag", default="v1.0.0")
    parser.add_argument("--memory", default="6G")
    parser.add_argument("--update-partitions", type=int, default=1)
    parser.add_argument("--base-image", default="eclipse-temurin:8-jdk-jammy")
    parser.add_argument("--force", action="store_true")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="One Python entry point for preparing, loading, building, and running CPGDB domains."
    )
    commands = parser.add_subparsers(dest="command", required=True)

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

    load = commands.add_parser("load", help="Load a prepared domain into its persistent backends.")
    load.add_argument("domain")
    load.add_argument("--reset", action="store_true")
    load.add_argument("--batch-size", type=int, default=5000)
    load.add_argument(
        "--only-source", choices=["Neo4J", "ArangoDB", "GPU"], action="append", default=[]
    )
    load.set_defaults(handler=load_domain)

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
    roots.add_argument("--start-uuid", help="Run from one UUID instead of the domain default.")
    roots.add_argument(
        "--start-uuids-file",
        help="Run every non-empty, non-comment UUID in this text file.",
    )
    run_parser.add_argument(
        "--repetitions",
        type=int,
        default=1,
        help="Number of BFS executions per start UUID (default: 1).",
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
    benchmark.set_defaults(handler=run_benchmark)

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
            "Resolve duplicate scenario/system/phase/repetition/start-UUID records. "
            "prefer-ok replaces failed attempts with successful retries."
        ),
    )
    benchmark_merge.add_argument(
        "--reconstruct-logs",
        action="store_true",
        help="Regenerate reconstruction only when CPGDB *.events.jsonl logs exist.",
    )
    benchmark_merge.set_defaults(handler=merge_benchmark_results_command)

    register = commands.add_parser("register", help="Register another schema/data/backend triple without recompiling.")
    register.add_argument("domain_id")
    register.add_argument("--schema", required=True)
    register.add_argument("--data", required=True)
    register.add_argument("--backends", required=True)
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
