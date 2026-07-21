from __future__ import annotations

import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import IO, Mapping, Optional, Sequence


class CommandError(RuntimeError):
    pass


class CommandRunner:
    def __init__(
        self,
        repo_root: str | Path,
        *,
        dry_run: bool = False,
        verbose: bool = True,
        log_path: str | Path | None = None,
    ):
        self.repo_root = Path(repo_root)
        self.dry_run = dry_run
        self.verbose = verbose
        self.log_path = Path(log_path) if log_path else None

    @staticmethod
    def _bounded_text(value: str | None, limit: int = 65_536) -> str:
        text = value or ""
        if len(text) <= limit:
            return text
        return text[:limit] + f"\n...[truncated {len(text) - limit} characters]..."

    @staticmethod
    def _read_file_tail(path: str | Path | None, limit: int = 65_536) -> str:
        if not path:
            return ""
        file_path = Path(path)
        if not file_path.is_file():
            return ""
        with file_path.open("rb") as handle:
            handle.seek(0, os.SEEK_END)
            size = handle.tell()
            handle.seek(max(0, size - limit), os.SEEK_SET)
            data = handle.read()
        text = data.decode("utf-8", errors="replace")
        if size > limit:
            return f"...[showing final {limit} bytes]...\n{text}"
        return text

    def _write_log(self, payload: dict[str, object]) -> None:
        if self.log_path is None:
            return
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        with self.log_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(payload, sort_keys=True) + "\n")

    def run(
        self,
        cmd: Sequence[str | Path],
        *,
        stdout_path: str | Path | None = None,
        stderr_path: str | Path | None = None,
        env: Optional[Mapping[str, str]] = None,
        cwd: str | Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        normalized = [str(x) for x in cmd]
        effective_cwd = Path(cwd or self.repo_root)
        if self.verbose:
            print("+ " + " ".join(normalized), file=sys.stderr)
        if self.dry_run:
            self._write_log(
                {
                    "timestamp": datetime.now(timezone.utc).isoformat(),
                    "command": normalized,
                    "cwd": str(effective_cwd),
                    "dry_run": True,
                    "stdout_path": str(stdout_path or ""),
                    "stderr_path": str(stderr_path or ""),
                }
            )
            return subprocess.CompletedProcess(normalized, 0, "", "")

        stdout_handle: IO[str] | int | None = None
        stderr_handle: IO[str] | int | None = None
        try:
            if stdout_path:
                Path(stdout_path).parent.mkdir(parents=True, exist_ok=True)
                stdout_handle = open(stdout_path, "w", encoding="utf-8")
            if stderr_path:
                Path(stderr_path).parent.mkdir(parents=True, exist_ok=True)
                stderr_handle = open(stderr_path, "a", encoding="utf-8")

            completed = subprocess.run(
                normalized,
                cwd=str(effective_cwd),
                env={**os.environ, **dict(env or {})},
                stdout=stdout_handle if stdout_handle else subprocess.PIPE,
                stderr=stderr_handle if stderr_handle else subprocess.PIPE,
                text=True,
                check=False,
            )
            self._write_log(
                {
                    "timestamp": datetime.now(timezone.utc).isoformat(),
                    "command": normalized,
                    "cwd": str(effective_cwd),
                    "returncode": completed.returncode,
                    "stdout_path": str(stdout_path or ""),
                    "stderr_path": str(stderr_path or ""),
                    "stdout": self._bounded_text(completed.stdout),
                    "stderr": self._bounded_text(completed.stderr),
                }
            )
            if completed.returncode != 0:
                redirected_stderr = self._read_file_tail(stderr_path) if stderr_path else ""
                stderr_text = completed.stderr or redirected_stderr
                stderr_label = (
                    f"stderr (redirected to {Path(stderr_path)}):"
                    if stderr_path
                    else "stderr:"
                )
                raise CommandError(
                    f"Command failed with exit code {completed.returncode}: {' '.join(normalized)}\n"
                    f"stdout:\n{completed.stdout or ''}\n"
                    f"{stderr_label}\n{stderr_text}"
                )
            return completed
        finally:
            if stdout_handle and stdout_handle is not subprocess.PIPE:
                stdout_handle.close()
            if stderr_handle and stderr_handle is not subprocess.PIPE:
                stderr_handle.close()
