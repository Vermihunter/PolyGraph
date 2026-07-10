from __future__ import annotations

import time
from pathlib import Path
from typing import Optional

import requests

from .config import BenchmarkConfig
from .subprocesses import CommandRunner


def wait_http(url: str, *, timeout_seconds: int, auth: Optional[tuple[str, str]] = None) -> None:
    deadline = time.time() + timeout_seconds
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            response = requests.get(url, auth=auth, timeout=5)
            if response.status_code < 500:
                return
        except Exception as exc:  # noqa: BLE001 - benchmark utility
            last_error = exc
        time.sleep(1)
    raise TimeoutError(f"Timed out waiting for {url}. Last error: {last_error}")


def start_services(config: BenchmarkConfig, runner: CommandRunner, *, timeout_seconds: int = 120) -> None:
    compose = config.paths.docker_compose
    if not compose:
        raise ValueError("paths.docker_compose is not configured")
    runner.run(["docker", "compose", "-f", compose, "up", "-d"])
    if not runner.dry_run:
        wait_http("http://localhost:7474", timeout_seconds=timeout_seconds)
        wait_http(
            f"{config.arango.base_url.rstrip('/')}/_api/version",
            timeout_seconds=timeout_seconds,
            auth=(config.arango.user, config.arango.password),
        )
