# Graph workspaces implementation plan (#37)

> **For agentic workers:** Use superpowers:executing-plans task by task.

**Goal:** Isolate generated artifacts for two graphs without changing source files or database deployments.
**Architecture:** Use stable graph IDs and an optional manifest-relative `workspace.root`/`workspace.id`. Default generated paths are `artifacts/<graph-id>/{ingestion,caches,index,gpu-storage,measurements}` beside the manifest. Preserve explicit paths; guard Python outputs and SQLite indexes with ownership markers. Native ingestion retains its existing input/configuration fingerprint and directory lock. No deployment manager or cleanup operation.
**Tech stack:** Existing Python graphio tools, native C++ ingestion, unittest/GTest/CTest.
**Spec:** GitHub #37, parent #26.

## Constraints and review focus

- Preserve legacy explicit configuration and read-only native validation.
- Reject path traversal, symlink aliases to inputs, conflicting owners, and shared SQLite index reuse across graphs before overwriting artifacts.
- Rebuilding one workspace must leave the other byte-for-byte unchanged.
- Keep sources, database containers and volumes untouched; no automatic cleanup.
- Existing unowned outputs require a fresh directory unless their existing metadata proves their owner; do not silently adopt unknown files.

## Tasks

- [x] Add workspace resolution and output/index ownership helpers in `tools/graphio/workspace.py`; unittest two graphs, stable reruns, aliases/collisions, unchanged sources and unknown outputs.
- [x] Extend `GraphManifest.cpp` with the additive workspace settings and default ingestion path. GTest two native configs, strict preparation/rerun isolation and conservative explicit-path preservation.
- [x] Route supported graph CLI cache/GPU/run and cache benchmark defaults through workspace paths. Guard explicit destinations and low-level cache/GPU artifact/index writes. Reuse current engines and indexes, with no changes to graph/storage formats.
- [x] Run focused regressions, full Python and CPU CTest, docs/link/format checks; review the diff and document migration, rebuild and scoped manual removal.

## Implementation notes

The existing native `PrepareOptions` gained optional workspace identity metadata so native and Python outputs share one reservation contract. Source-index lookup shares the native workspace path helper. Legacy generated-file guards cover temporary, report and render outputs; no deployment or ingestion framework was introduced.

Review found and corrected symlink descendant and ownership-layer bypasses. Additional regressions cover guarded benchmark overwrite, scoped legacy cache cleanup, complete legacy output targets and sanitized benchmark-suite directory collisions.
