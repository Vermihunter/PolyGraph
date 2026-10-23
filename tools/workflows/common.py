from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable, Sequence

from tools.graphio.domain import load_domain_manifest
from tools.workflows.checkpoint import WorkflowCheckpoint

ROOT = Path(__file__).resolve().parents[2]
POLYGRAPH = ROOT / "tools" / "polygraph.py"
STORAGE_COMPOSE = ROOT / "storage_layer" / "docker-compose.yml"
STORAGE_ENV = ROOT / "storage_layer" / ".env"
REDIS_COMPOSE = ROOT / "storage_layer" / "redis" / "docker-compose.yml"
REDIS_ENV = ROOT / "storage_layer" / "redis" / ".env.sf1-benchmark"
SCHEMA_PLACEMENT = ROOT / "tools" / "ldbc_snb" / "placement.schema-3db.json"
WORKFLOW_STATE_ROOT = ROOT / ".polygraph" / "workflows"


def canonical_sf(value: str) -> str:
    text = value.strip().lower()
    if text.startswith("sf"):
        text = text[2:]
    text = text.replace("_", ".")
    parts = text.split(".")
    if len(parts) > 2 or not all(part.isdigit() for part in parts):
        raise ValueError(f"Invalid scale factor: {value!r}")
    whole = str(int(parts[0]))
    fraction = (parts[1].rstrip("0") or "0") if len(parts) == 2 else "0"
    return f"sf{whole}_{fraction}"


def sf_value(value: str) -> str:
    key = canonical_sf(value)[2:]
    whole, fraction = key.split("_", 1)
    return f"{whole}.{fraction}"


def domain_dir(sf: str, distribution: str = "schema-based") -> Path:
    return ROOT / "data" / "ldbc-snb" / canonical_sf(sf) / distribution


def domain_manifest(sf: str, distribution: str = "schema-based") -> Path:
    return domain_dir(sf, distribution) / "domain.json"


def raw_dir(sf: str) -> Path:
    return ROOT / "data" / "ldbc-snb" / canonical_sf(sf) / "raw"


def run(command: Sequence[str | Path], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    rendered = [str(item) for item in command]
    print("+", " ".join(rendered), flush=True)
    return subprocess.run(rendered, cwd=ROOT, text=True, check=check)


def polygraph(*args: str | Path) -> None:
    run([sys.executable, POLYGRAPH, *args])


def discover_cache_configs(config_dir: Path) -> list[Path]:
    configs = sorted(config_dir.glob("*.json"))
    if not configs:
        raise FileNotFoundError(f"No cache configs found in {config_dir}")
    return configs


def ensure_redis(*, env_file: Path | None = None) -> None:
    env = env_file or (REDIS_ENV if REDIS_ENV.is_file() else None)
    command: list[str | Path] = ["docker", "compose"]
    if env is not None:
        command += ["--env-file", env]
    command += ["-f", REDIS_COMPOSE, "up", "-d", "--wait"]
    run(command)


def remove_path(path: Path) -> None:
    if path.is_dir():
        print(f"[cleanup] removing {path}", flush=True)
        shutil.rmtree(path)
    elif path.exists():
        print(f"[cleanup] removing {path}", flush=True)
        path.unlink()


def load_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object: {path}")
    return value


def storage_projects() -> list[str]:
    """Discover Compose projects from both containers and persistent volumes.

    Looking only at containers misses an SF after somebody manually removed its
    containers while leaving the named Compose volumes behind.  Those volumes
    are exactly what a destructive ``--all`` reset is expected to remove.
    """
    projects: set[str] = set()

    completed = subprocess.run(
        [
            "docker", "ps", "-a",
            "--filter", "label=polygraph.scale-factor",
            "--format", '{{.Label "com.docker.compose.project"}}',
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    )
    projects.update(line.strip() for line in completed.stdout.splitlines() if line.strip())

    volumes = subprocess.run(
        [
            "docker", "volume", "ls", "-q",
            "--filter", "label=com.docker.compose.project",
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    )
    volume_names = [line.strip() for line in volumes.stdout.splitlines() if line.strip()]
    if volume_names:
        inspected = subprocess.run(
            [
                "docker", "volume", "inspect",
                "--format", '{{ index .Labels "com.docker.compose.project" }}',
                *volume_names,
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=True,
        )
        projects.update(line.strip() for line in inspected.stdout.splitlines() if line.strip())

    return sorted(projects)


def destroy_storage_project(project: str) -> None:
    run([
        "docker", "compose",
        "--env-file", STORAGE_ENV,
        "-f", STORAGE_COMPOSE,
        "--project-name", project,
        "down", "--volumes", "--remove-orphans",
    ])


def storage_marker(sf: str, distribution: str = "schema-based") -> Path:
    return domain_dir(sf, distribution) / ".storage-loaded.json"


def project_has_volumes(project: str) -> bool:
    completed = subprocess.run(
        [
            "docker", "volume", "ls", "-q",
            "--filter", f"label=com.docker.compose.project={project}",
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    )
    return any(line.strip() for line in completed.stdout.splitlines())


def write_storage_marker(sf: str, distribution: str, project: str, *, postgres_indexes: bool) -> Path:
    import time

    marker = storage_marker(sf, distribution)
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(
        json.dumps(
            {
                "project": project,
                "domain": str(domain_manifest(sf, distribution)),
                "postgresTraversalIndexes": postgres_indexes,
                "loadedAtUnix": time.time(),
            },
            indent=2,
        ) + "\n",
        encoding="utf-8",
    )
    return marker


def file_fingerprint(path: Path) -> dict[str, int | str]:
    resolved = path.expanduser().resolve()
    stat = resolved.stat()
    return {
        "path": str(resolved),
        "size": stat.st_size,
        "mtimeNs": stat.st_mtime_ns,
    }


def workflow_checkpoint_path(workflow: str, sf: str, distribution: str = "schema-based") -> Path:
    safe_distribution = "".join(
        character if character.isalnum() or character in "-_" else "-"
        for character in distribution.strip().lower()
    ).strip("-") or "distribution"
    return WORKFLOW_STATE_ROOT / f"{workflow}-{canonical_sf(sf)}-{safe_distribution}.json"


def storage_load_checkpoint_path(sf: str, distribution: str = "schema-based") -> Path:
    return workflow_checkpoint_path("storage-load", sf, distribution)


def clear_storage_load_checkpoint(sf: str, distribution: str = "schema-based") -> None:
    storage_load_checkpoint_path(sf, distribution).unlink(missing_ok=True)


def resumable_storage_load(
    sf: str,
    distribution: str,
    *,
    project: str,
    manifest: Path,
    postgres_indexes: bool,
    batch_size: int,
    workers: int,
    restart: bool = False,
) -> WorkflowCheckpoint:
    """Load persistent backends independently and checkpoint each successful source.

    This intentionally invokes the existing loader once per persistent backend. It costs an
    extra streaming pass over the prepared graph during a clean initialization, but means a
    failure in a later backend never forces an already loaded database to be reset/reloaded.
    The expensive database imports are therefore resumable across process failures and missing
    optional Python dependencies.
    """
    if not manifest.is_file():
        raise FileNotFoundError(f"Prepared domain is missing: {manifest}")
    # The canonical data source may be a huge JSONL file or the compact
    # graph.ldbc.json descriptor produced by fast_prepare.py.  Follow the
    # manifest rather than hard-coding graph.jsonl.
    graph = load_domain_manifest(manifest).paths.data
    if not graph.is_file():
        raise FileNotFoundError(f"Prepared graph source is missing: {graph}")

    identity: dict[str, Any] = {
        "scaleFactor": canonical_sf(sf),
        "distribution": distribution,
        "project": project,
        "manifest": file_fingerprint(manifest),
        "graph": file_fingerprint(graph),
        "postgresTraversalIndexes": bool(postgres_indexes),
    }
    checkpoint = WorkflowCheckpoint.open(
        storage_load_checkpoint_path(sf, distribution),
        workflow="storage-load",
        identity=identity,
        restart=restart,
    )

    sources = (
        ("Neo4J", "neo4j"),
        ("ArangoDB", "arangodb"),
        ("PostgreSQL", "postgresql"),
    )
    for source, service in sources:
        stage = f"backend:{source}"
        if checkpoint.is_done(stage):
            print(f"[resume] {source}: already loaded; skipping", flush=True)
            continue

        print(f"[resume] {source}: loading", flush=True)
        load_args: list[str | Path] = [
            "instance", sf, distribution, "load", manifest,
            "--batch-size", str(batch_size),
            "--workers", str(workers),
            "--only-source", source,
            "--services", service,
        ]
        if source == "PostgreSQL" and postgres_indexes:
            load_args.append("--postgres-indexes")
        try:
            polygraph(*load_args)
        except BaseException as error:
            checkpoint.mark_failed(stage, error)
            print(
                f"[resume] {source}: failed; successful earlier backends are checkpointed in "
                f"{checkpoint.path}",
                flush=True,
            )
            raise
        checkpoint.mark_done(stage)
        print(f"[resume] {source}: checkpoint saved", flush=True)

    write_storage_marker(
        sf,
        distribution,
        project,
        postgres_indexes=postgres_indexes,
    )
    checkpoint.mark_done("complete")
    return checkpoint

