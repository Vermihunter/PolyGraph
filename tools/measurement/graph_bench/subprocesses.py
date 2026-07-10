from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path
from typing import IO, Mapping, Optional, Sequence


class CommandError(RuntimeError):
    pass


class CommandRunner:
    def __init__(self, repo_root: str | Path, *, dry_run: bool = False, verbose: bool = True):
        self.repo_root = Path(repo_root)
        self.dry_run = dry_run
        self.verbose = verbose

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
        if self.verbose:
            print("+ " + " ".join(normalized), file=sys.stderr)
        if self.dry_run:
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
                cwd=str(cwd or self.repo_root),
                env={**os.environ, **dict(env or {})},
                stdout=stdout_handle if stdout_handle else subprocess.PIPE,
                stderr=stderr_handle if stderr_handle else subprocess.PIPE,
                text=True,
                check=False,
            )
            if completed.returncode != 0:
                raise CommandError(
                    f"Command failed with exit code {completed.returncode}: {' '.join(normalized)}\n"
                    f"stdout:\n{completed.stdout or ''}\n"
                    f"stderr:\n{completed.stderr or ''}"
                )
            return completed
        finally:
            if stdout_handle and stdout_handle is not subprocess.PIPE:
                stdout_handle.close()
            if stderr_handle and stderr_handle is not subprocess.PIPE:
                stderr_handle.close()
