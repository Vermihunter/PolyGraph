#!/usr/bin/env python3
"""Select an immutable example generation, then run PolyGraph's shared importer.

CSV parsing, identity allocation, normalization and storage output all remain in
C++ through MovieAdapter and the existing ingestion engine. This launcher never
starts services, resets databases, or deletes previous generations.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.graphio.workspace import GraphWorkspace, claim_output


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def fingerprint(paths: list[Path]) -> str:
    records = [(str(path), digest_file(path)) for path in sorted(set(paths))]
    return hashlib.sha256(
        json.dumps(records, ensure_ascii=False).encode("utf-8")
    ).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        type=Path,
        default=ROOT / "build/cpu-tests/bin/polygraph_movie_import",
    )
    parser.add_argument(
        "--manifest", type=Path, default=Path(__file__).with_name("domain.json")
    )
    parser.add_argument("--workspace-root", type=Path)
    args = parser.parse_args()
    binary, manifest = args.binary.resolve(), args.manifest.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"Build the linked example importer first: {binary}")
    document = json.loads(manifest.read_text())
    paths = {
        key: str((manifest.parent / value).resolve())
        for key, value in document["paths"].items()
    }
    source = Path(paths["sourceData"])
    dependencies = [manifest, binary, *sorted(source.glob("*.csv"))]
    dependencies.extend(
        Path(value)
        for key, value in paths.items()
        if key != "ingestionArtifacts" and Path(value).is_file()
    )
    generation = fingerprint(dependencies)
    workspace = GraphWorkspace.from_manifest(manifest, document)
    base = (
        args.workspace_root.resolve()
        if args.workspace_root
        else workspace.root.with_name(workspace.root.name + "-onboarding")
    )
    manifests = base / "manifests"
    claim_output(manifests, manifest, protected=(*dependencies, source))
    generated_manifest = manifests / (generation + ".json")
    paths["ingestionArtifacts"] = str(base / "generations" / generation / "ingestion")
    document["paths"] = paths
    document["workspace"] = {"root": str(base / "generations"), "id": generation}
    encoded = json.dumps(document, indent=2) + "\n"
    if generated_manifest.exists():
        if generated_manifest.read_text() != encoded:
            raise ValueError(
                "Generation manifest differs from immutable inputs; choose a fresh workspace root"
            )
    else:
        with generated_manifest.open("x", encoding="utf-8") as stream:
            stream.write(encoded)
    result = subprocess.run(
        [str(binary), "import", "--graph-manifest", str(generated_manifest)],
        stdout=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        return result.returncode
    if fingerprint(dependencies) != generation:
        raise ValueError(
            "Inputs changed during onboarding; rerun with immutable CSV/config files"
        )
    report = json.loads(result.stdout)
    report.update(
        graph_manifest=str(generated_manifest), generation_fingerprint=generation
    )
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError) as error:
        print(f"Movie onboarding: {error}", file=sys.stderr)
        raise SystemExit(2)
