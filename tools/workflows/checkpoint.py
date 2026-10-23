from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


class CheckpointMismatchError(RuntimeError):
    pass


def _atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _identity_differences(actual: dict[str, Any], expected: dict[str, Any]) -> list[str]:
    keys = sorted(set(actual) | set(expected))
    return [
        f"{key}: checkpoint={actual.get(key)!r}, requested={expected.get(key)!r}"
        for key in keys
        if actual.get(key) != expected.get(key)
    ]


@dataclass
class WorkflowCheckpoint:
    path: Path
    state: dict[str, Any]

    VERSION = 1

    @classmethod
    def open(
        cls,
        path: Path,
        *,
        workflow: str,
        identity: dict[str, Any],
        restart: bool = False,
    ) -> "WorkflowCheckpoint":
        path = path.expanduser().resolve()
        if restart:
            path.unlink(missing_ok=True)

        if path.is_file():
            raw = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(raw, dict):
                raise CheckpointMismatchError(f"Invalid checkpoint JSON object: {path}")
            if raw.get("version") != cls.VERSION:
                raise CheckpointMismatchError(
                    f"Checkpoint version mismatch in {path}; pass --restart to start a new run."
                )
            if raw.get("workflow") != workflow:
                raise CheckpointMismatchError(
                    f"Checkpoint workflow mismatch in {path}; pass --restart to start a new run."
                )
            actual_identity = raw.get("identity")
            if not isinstance(actual_identity, dict):
                raise CheckpointMismatchError(
                    f"Checkpoint has no valid identity: {path}; pass --restart to start a new run."
                )
            differences = _identity_differences(actual_identity, identity)
            if differences:
                rendered = "\n  - ".join(differences)
                raise CheckpointMismatchError(
                    "Existing checkpoint belongs to a different workflow configuration:\n"
                    f"  - {rendered}\n"
                    "Pass --restart if you intentionally want a fresh run."
                )
            raw.setdefault("stages", {})
            return cls(path=path, state=raw)

        state: dict[str, Any] = {
            "version": cls.VERSION,
            "workflow": workflow,
            "identity": identity,
            "createdAtUnix": time.time(),
            "updatedAtUnix": time.time(),
            "stages": {},
        }
        checkpoint = cls(path=path, state=state)
        checkpoint.save()
        return checkpoint

    def save(self) -> None:
        self.state["updatedAtUnix"] = time.time()
        _atomic_write_json(self.path, self.state)

    def is_done(self, stage: str) -> bool:
        value = self.state.setdefault("stages", {}).get(stage)
        return isinstance(value, dict) and value.get("status") == "done"

    def stage(self, stage: str) -> dict[str, Any] | None:
        value = self.state.setdefault("stages", {}).get(stage)
        return value if isinstance(value, dict) else None

    def mark_done(self, stage: str, **details: Any) -> None:
        payload: dict[str, Any] = {
            "status": "done",
            "completedAtUnix": time.time(),
        }
        if details:
            payload["details"] = details
        self.state.setdefault("stages", {})[stage] = payload
        self.save()

    def mark_failed(self, stage: str, error: BaseException) -> None:
        self.state.setdefault("stages", {})[stage] = {
            "status": "failed",
            "failedAtUnix": time.time(),
            "error": f"{type(error).__name__}: {error}",
        }
        self.save()

    def clear_stages(self, stages: Iterable[str]) -> None:
        stored = self.state.setdefault("stages", {})
        changed = False
        for stage in stages:
            if stage in stored:
                del stored[stage]
                changed = True
        if changed:
            self.save()

    def completed_stages(self) -> list[str]:
        return sorted(
            stage
            for stage, value in self.state.setdefault("stages", {}).items()
            if isinstance(value, dict) and value.get("status") == "done"
        )
