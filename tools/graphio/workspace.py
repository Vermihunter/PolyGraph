"""Deterministic graph artifact paths and conservative ownership checks.

This module never removes files or manages database deployments.
"""

from dataclasses import dataclass
import json
import hashlib
from pathlib import Path
import re
import sqlite3


def _component(value: str) -> str:
    if (
        not isinstance(value, str)
        or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", value)
        or value in (".", "..")
    ):
        raise ValueError(
            "Workspace ID must be a safe path component; set workspace.id explicitly"
        )
    return value


def _owner_record(source: Path, graph: Path | None = None) -> dict:
    record = {
        "format": "polygraph.artifact-owner",
        "version": 1,
        "source": str(source.resolve()),
    }
    if graph is not None:
        record["graph"] = str(graph.resolve())
    return record


def _owner(marker: Path, source: Path, graph: Path | None = None) -> None:
    expected = _owner_record(source, graph)
    if marker.exists():
        if json.loads(marker.read_text()) != expected:
            raise ValueError(
                f"Artifact ownership collision: {marker}; choose a different output directory"
            )
        return
    try:
        with marker.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps(expected, indent=2) + "\n")
    except FileExistsError:
        _owner(marker, source, graph)


def check_ancestors(
    directory: Path, source: Path, marker_name: str, graph: Path | None = None
) -> None:
    expected = _owner_record(source, graph)
    for parent in (directory, *directory.parents):
        markers = [parent / marker_name]
        if marker_name == ".polygraph-workspace.json":
            markers.append(parent.parent / ("." + parent.name + marker_name))
        for marker in markers:
            if marker.exists() and json.loads(marker.read_text()) != expected:
                raise ValueError(f"Artifact ownership collision: {marker}")
        if marker_name == ".polygraph-workspace.json" and graph is not None:
            artifact_marker = parent / ".polygraph-artifact-owner.json"
            if artifact_marker.exists() and json.loads(
                artifact_marker.read_text()
            ) != _owner_record(graph):
                raise ValueError(f"Artifact ownership collision: {artifact_marker}")
        if marker_name == ".polygraph-artifact-owner.json":
            # A planner knows its input graph, rather than the selecting manifest.
            # Workspace records link both identities so neither layer can be bypassed.
            for marker in (
                parent / ".polygraph-workspace.json",
                parent.parent / ("." + parent.name + ".polygraph-workspace.json"),
            ):
                if marker.exists() and json.loads(marker.read_text()).get(
                    "graph"
                ) != str(source.resolve()):
                    raise ValueError(f"Artifact ownership collision: {marker}")


def validate_output(
    directory: Path,
    source: Path,
    *,
    marker_name=".polygraph-artifact-owner.json",
    protected=(),
    graph=None,
) -> Path:
    directory, source = Path(directory).resolve(), Path(source).resolve()
    for path in (source, *protected):
        path = Path(path).resolve()
        if path == directory or path.is_relative_to(directory):
            raise ValueError(
                f"Artifact output contains source/configuration input: {path}"
            )
        if path.is_dir() and directory.is_relative_to(path):
            raise ValueError(f"Artifact output is inside source directory: {path}")
    check_ancestors(directory, source, marker_name, graph)
    if directory.exists() and any(path.is_symlink() for path in directory.rglob("*")):
        raise ValueError(f"Artifact directory contains symlinks: {directory}")
    marker = directory / marker_name
    if (
        directory.exists()
        and not marker.exists()
        and any(
            p.name
            not in {".polygraph-workspace.json", ".polygraph-artifact-owner.json"}
            for p in directory.iterdir()
        )
    ):
        raise ValueError(
            f"Unowned nonempty artifact directory: {directory}; use a fresh workspace"
        )
    return directory


def claim_output(
    directory: Path,
    source: Path,
    *,
    marker_name=".polygraph-artifact-owner.json",
    protected=(),
    graph=None,
) -> Path:
    directory = validate_output(
        directory, source, marker_name=marker_name, protected=protected, graph=graph
    )
    directory.mkdir(parents=True, exist_ok=True)
    _owner(directory / marker_name, Path(source), graph)
    return directory


def claim_index(path: Path, source: Path) -> Path:
    path, source = Path(path).resolve(), Path(source).resolve()
    if path == source:
        raise ValueError("Planning index cannot overwrite its graph input")
    check_ancestors(path.parent, source, ".polygraph-artifact-owner.json")
    marker = path.with_name(path.name + ".polygraph-owner.json")
    if path.exists() and not marker.exists():
        # Adopt legacy indexes only when their recorded graph path proves ownership.
        try:
            with sqlite3.connect(path.as_uri() + "?mode=ro", uri=True) as db:
                row = db.execute(
                    "SELECT value FROM meta WHERE key='graph.path'"
                ).fetchone()
            if row is None or Path(row[0]).resolve() != source:
                raise ValueError("Index belongs to a different graph")
        except (sqlite3.Error, ValueError) as error:
            raise ValueError(
                f"Unowned planning index: {path}; choose a fresh index path"
            ) from error
    path.parent.mkdir(parents=True, exist_ok=True)
    _owner(marker, source)
    return path


@dataclass(frozen=True)
class GraphWorkspace:
    root: Path
    manifest: Path
    graph: Path | None = None

    @classmethod
    def from_manifest(cls, manifest: Path, document: dict):
        manifest = Path(manifest).resolve()
        settings = document.get("workspace", {})
        if not isinstance(settings, dict) or set(settings) - {"root", "id"}:
            raise ValueError("workspace accepts only root and id")
        name = _component(settings.get("id", document.get("id", manifest.parent.name)))
        raw_root = settings.get("root", "artifacts")
        if not isinstance(raw_root, str) or not raw_root or "\0" in raw_root:
            raise ValueError("Workspace root must be a nonempty path without NUL")
        base = Path(raw_root)
        if not base.is_absolute():
            base = manifest.parent / base
        graph = document.get("paths", {}).get("data")
        graph_path = Path(graph) if graph else None
        if graph_path is not None and not graph_path.is_absolute():
            graph_path = manifest.parent / graph_path
        return cls(
            (base / name).resolve(),
            manifest,
            graph_path.resolve() if graph_path else None,
        )

    def path(self, kind: str) -> Path:
        return self.root / _component(kind)

    def claim(self, directory: Path | None = None, *, protected=()) -> Path:
        destination = Path(directory or self.root).resolve()
        validate_output(
            destination,
            self.manifest,
            marker_name=".polygraph-workspace.json",
            protected=protected,
            graph=self.graph,
        )
        # Reserve the namespace beside its root, including when native ingestion already
        # populated a sibling directory. Reservation never adopts or removes those files.
        if destination.is_relative_to(self.root):
            for source in (self.manifest, *protected):
                source = Path(source).resolve()
                if source.is_relative_to(self.root) or (
                    source.is_dir() and self.root.is_relative_to(source)
                ):
                    raise ValueError("Workspace overlaps source/configuration input")
            check_ancestors(
                self.root, self.manifest, ".polygraph-workspace.json", self.graph
            )
            self.root.parent.mkdir(parents=True, exist_ok=True)
            _owner(
                self.root.parent / ("." + self.root.name + ".polygraph-workspace.json"),
                self.manifest,
                self.graph,
            )
        return claim_output(
            destination,
            self.manifest,
            marker_name=".polygraph-workspace.json",
            protected=protected,
            graph=self.graph,
        )


def workspace_for(domain) -> GraphWorkspace:
    workspace = GraphWorkspace.from_manifest(
        domain.paths.manifest, json.loads(domain.paths.manifest.read_text())
    )
    return GraphWorkspace(workspace.root, workspace.manifest, domain.paths.data)


def _domain_inputs(domain):
    return tuple(
        path
        for path in (
            domain.paths.data,
            domain.paths.source_data,
            domain.paths.schema,
            domain.paths.backends,
            domain.paths.native_id_map,
            domain.paths.cache_config,
            domain.paths.gpu_distribution_config,
            domain.paths.observability,
        )
        if path is not None
    )


def claim_domain_output(domain, directory: Path, *, protected=()) -> Path:
    workspace = workspace_for(domain)
    # A destination is claimed directly; no unrelated default root is created for overrides.
    return workspace.claim(directory, protected=(*_domain_inputs(domain), *protected))


def claim_domain_index(domain, path: Path) -> Path:
    path = Path(path).resolve()
    for source in (domain.paths.manifest, *_domain_inputs(domain)):
        source = Path(source).resolve()
        if path == source or (source.is_dir() and path.is_relative_to(source)):
            raise ValueError(
                f"Planning index would overwrite source/configuration: {source}"
            )
    check_ancestors(
        path.parent,
        domain.paths.manifest,
        ".polygraph-workspace.json",
        domain.paths.data,
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    marker = path.with_name(path.name + ".polygraph-workspace.json")
    if marker.exists() and json.loads(marker.read_text()) != _owner_record(
        domain.paths.manifest, domain.paths.data
    ):
        raise ValueError(f"Artifact ownership collision: {marker}")
    claim_index(path, domain.paths.data)
    _owner(marker, domain.paths.manifest, domain.paths.data)
    return path


def default_planning_index(graph: Path) -> Path:
    graph = Path(graph).resolve()
    key = hashlib.sha256(str(graph).encode("utf-8")).hexdigest()[:24]
    return graph.parent / ".polygraph" / key / "index" / "cache-plan.sqlite3"


def claim_generated_files(
    directory: Path, source: Path, names: tuple[str, ...], *, protected=()
) -> None:
    """Guard legacy preparation's generated files without claiming its raw-source tree."""
    directory, source = Path(directory).resolve(), Path(source).resolve()
    marker = directory / ".polygraph-preparation.json"
    if directory.exists():
        for entry in directory.rglob("*"):
            if source.is_dir() and entry.is_relative_to(source):
                continue  # Raw input links belong to the reader, never to the output writer.
            if entry.is_symlink():
                raise ValueError(f"Generated output contains symlinks: {entry}")
    for name in names:
        if Path(name).name != name or name in (".", ".."):
            raise ValueError("Generated artifact name must be a filename")
        target = directory / name
        if target.is_symlink():
            raise ValueError(f"Generated artifact aliases source input: {target}")
        for input_path in (source, *protected):
            input_path = Path(input_path).resolve()
            if target.resolve() == input_path or (
                input_path.is_dir() and target.resolve().is_relative_to(input_path)
            ):
                raise ValueError(f"Generated artifact aliases source input: {target}")
        if target.exists() and not marker.exists():
            raise ValueError(
                f"Unowned generated artifact: {target}; use a fresh --output-dir"
            )
    directory.mkdir(parents=True, exist_ok=True)
    _owner(marker, source)


def workspace_id(value: str) -> str:
    return _component(value)
