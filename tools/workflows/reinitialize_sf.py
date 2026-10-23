#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.polygraph import resolve_compose_instance
from tools.workflows.checkpoint import WorkflowCheckpoint
from tools.workflows.common import (
    canonical_sf,
    clear_storage_load_checkpoint,
    domain_manifest,
    file_fingerprint,
    project_has_volumes,
    raw_dir,
    resumable_storage_load,
    polygraph,
    run,
    storage_load_checkpoint_path,
    workflow_checkpoint_path,
)

RESET_STAGE = "storage-reset"
PREPARE_STAGE = "distribution-prepared"
LOAD_STAGE = "storage-loaded"
CACHE_STAGE = "cache-plans-built"
COMPLETE_STAGE = "complete"


def _input_path(sf: str, override: str | None) -> Path:
    return Path(override).expanduser().resolve() if override else raw_dir(sf).resolve()


def _resume_identity(args: argparse.Namespace, project: str) -> dict[str, object]:
    return {
        "scaleFactor": canonical_sf(args.sf),
        "distribution": args.distribution,
        "project": project,
        "input": str(_input_path(args.sf, args.input)),
        "rootCount": args.root_count,
        "postgresTraversalIndexes": bool(args.postgres_indexes),
    }


def _invalidate_if_prepared_graph_changed(
    checkpoint: WorkflowCheckpoint,
    manifest: Path,
    *,
    sf: str,
    distribution: str,
) -> None:
    if not checkpoint.is_done(PREPARE_STAGE):
        return
    graph = manifest.parent / "graph.jsonl"
    if not manifest.is_file() or not graph.is_file():
        print("[resume] prepared files disappeared; preparation will be repeated", flush=True)
        checkpoint.clear_stages(
            [PREPARE_STAGE, LOAD_STAGE, CACHE_STAGE, COMPLETE_STAGE]
        )
        clear_storage_load_checkpoint(sf, distribution)
        return

    stage = checkpoint.stage(PREPARE_STAGE) or {}
    details = stage.get("details") if isinstance(stage.get("details"), dict) else {}
    recorded = details.get("graph") if isinstance(details, dict) else None
    current = file_fingerprint(graph)
    if recorded is not None and recorded != current:
        print(
            "[resume] prepared graph changed since the checkpoint; keeping the prepared graph "
            "but invalidating storage/cache stages",
            flush=True,
        )
        checkpoint.clear_stages([LOAD_STAGE, CACHE_STAGE, COMPLETE_STAGE])
        clear_storage_load_checkpoint(sf, distribution)
        checkpoint.mark_done(PREPARE_STAGE, graph=current)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Freshly rebuild one schema-based SF with crash-safe checkpoints. Re-run the same "
            "command after a failure to resume from the last completed stage."
        )
    )
    parser.add_argument("--sf", default="1")
    parser.add_argument("--distribution", default="schema-based")
    parser.add_argument("--input", help="Optional raw LDBC input override.")
    parser.add_argument(
        "--purge-all-storage", action="store_true",
        help="Delete every discovered PolyGraph SF storage project before rebuilding this SF.",
    )
    parser.add_argument(
        "--restart", action="store_true",
        help="Discard the resume checkpoint and intentionally start this rebuild from zero.",
    )
    parser.add_argument(
        "--postgres-indexes", action="store_true",
        help="Opt into PostgreSQL traversal indexes. Default is the index-free benchmark baseline.",
    )
    parser.add_argument("--batch-size", type=int, default=20000)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--root-count", type=int, default=100)
    args = parser.parse_args()

    instance = resolve_compose_instance(args.sf, args.distribution)
    checkpoint = WorkflowCheckpoint.open(
        workflow_checkpoint_path("reinitialize", args.sf, args.distribution),
        workflow="reinitialize-sf",
        identity=_resume_identity(args, instance.project),
        restart=args.restart,
    )
    manifest = domain_manifest(args.sf, args.distribution)
    completed = checkpoint.completed_stages()
    print(f"[resume] checkpoint: {checkpoint.path}", flush=True)
    print(
        "[resume] completed stages: " + (", ".join(completed) if completed else "none"),
        flush=True,
    )

    _invalidate_if_prepared_graph_changed(
        checkpoint,
        manifest,
        sf=args.sf,
        distribution=args.distribution,
    )

    # If somebody deleted the Compose volumes outside this workflow, backend checkpoints
    # cannot be trusted even though the prepared graph/cache plans are still valid.
    if checkpoint.is_done(LOAD_STAGE) and not project_has_volumes(instance.project):
        print("[resume] storage volumes are missing; persistent backends will be reloaded", flush=True)
        checkpoint.clear_stages([LOAD_STAGE, COMPLETE_STAGE])
        clear_storage_load_checkpoint(args.sf, args.distribution)

    if checkpoint.is_done(RESET_STAGE):
        reset_details = (checkpoint.stage(RESET_STAGE) or {}).get("details", {})
        previous_scope = reset_details.get("scope") if isinstance(reset_details, dict) else None
        if args.purge_all_storage and previous_scope != "all":
            raise RuntimeError(
                "This resumed run already completed a target-only storage reset. "
                "Use --restart --purge-all-storage if you now want the broader destructive reset."
            )
        print("[resume] storage reset: already completed; skipping", flush=True)
    else:
        reset = [
            sys.executable, ROOT / "tools/storage/reset.py",
            "--sf", args.sf,
            "--distribution", args.distribution,
            "--yes",
        ]
        if args.purge_all_storage:
            reset.append("--all")
        run(reset)
        clear_storage_load_checkpoint(args.sf, args.distribution)
        checkpoint.mark_done(RESET_STAGE, scope="all" if args.purge_all_storage else "target")

    if checkpoint.is_done(PREPARE_STAGE):
        print("[resume] distribution preparation: already completed; skipping", flush=True)
    else:
        prepare = [
            sys.executable, ROOT / "tools/ldbc_snb/prepare_schema.py",
            "--sf", args.sf,
            "--root-count", str(args.root_count),
            "--clean",
        ]
        if args.input:
            prepare += ["--input", args.input]
        try:
            run(prepare)
        except BaseException as error:
            checkpoint.mark_failed(PREPARE_STAGE, error)
            raise
        graph = manifest.parent / "graph.jsonl"
        checkpoint.mark_done(PREPARE_STAGE, graph=file_fingerprint(graph))
        clear_storage_load_checkpoint(args.sf, args.distribution)

    # Activation is cheap and deliberately repeated on resume so this SF becomes the active one.
    polygraph("instance", args.sf, args.distribution, "activate")

    if checkpoint.is_done(LOAD_STAGE):
        print("[resume] persistent storage load: already completed; skipping", flush=True)
    else:
        try:
            load_checkpoint = resumable_storage_load(
                args.sf,
                args.distribution,
                project=instance.project,
                manifest=manifest,
                postgres_indexes=args.postgres_indexes,
                batch_size=args.batch_size,
                workers=args.workers,
                restart=False,
            )
        except BaseException as error:
            checkpoint.mark_failed(LOAD_STAGE, error)
            raise
        checkpoint.mark_done(LOAD_STAGE, checkpoint=str(load_checkpoint.path))

    if checkpoint.is_done(CACHE_STAGE):
        print("[resume] cache plans: already completed; skipping", flush=True)
    else:
        try:
            run([
                sys.executable, ROOT / "tools/cache/build_benchmark_plans.py",
                "--sf", args.sf,
                "--distribution", args.distribution,
                "--clean",
            ])
        except BaseException as error:
            checkpoint.mark_failed(CACHE_STAGE, error)
            raise
        checkpoint.mark_done(CACHE_STAGE)

    checkpoint.mark_done(COMPLETE_STAGE)
    print("\n[ready] storage loaded and all benchmark cache plans generated")
    print(f"[ready] domain: {manifest}")
    print(f"[ready] resume checkpoint: {checkpoint.path}")
    print(f"[ready] backend checkpoint: {storage_load_checkpoint_path(args.sf, args.distribution)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
