#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import json
import os
import signal
import subprocess
import time
import urllib.request
from pathlib import Path
from statistics import mean
from typing import Any


def _run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, text=True, capture_output=True, check=False)


def _find_container(explicit: str | None) -> str:
    if explicit:
        return explicit
    result = _run("docker", "ps", "--format", "{{.Names}}")
    names = [line.strip() for line in result.stdout.splitlines() if "arango" in line.lower()]
    if not names:
        raise RuntimeError("No running ArangoDB container found")
    return names[0]


def _inspect(container: str) -> dict[str, Any]:
    result = _run("docker", "inspect", container)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or f"docker inspect failed for {container}")
    return json.loads(result.stdout)[0]


def _http_get(url: str, user: str, password: str, timeout: float = 3.0) -> str:
    token = base64.b64encode(f"{user}:{password}".encode()).decode()
    request = urllib.request.Request(
        url,
        headers={"Authorization": f"Basic {token}"},
        method="GET",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8", errors="replace")


def _cpu_seconds(pid: int) -> float | None:
    path = Path(f"/proc/{pid}/stat")
    if not path.exists():
        return None
    fields = path.read_text(encoding="utf-8", errors="replace").split()
    if len(fields) < 15:
        return None
    ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    return (int(fields[13]) + int(fields[14])) / float(ticks)


def _rss_bytes(pid: int) -> int:
    path = Path(f"/proc/{pid}/status")
    if not path.exists():
        return 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024
    return 0


def _parse_metrics(text: str) -> dict[str, float]:
    metrics: dict[str, float] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or " " not in line:
            continue
        left, raw_value = line.rsplit(None, 1)
        name = left.split("{", 1)[0]
        lowered = name.lower()
        if "cache" not in lowered and "rocksdb" not in lowered:
            continue
        try:
            metrics[left] = float(raw_value)
        except ValueError:
            continue
    return metrics


def _important_metrics(metrics: dict[str, float]) -> dict[str, float]:
    preferred_fragments = (
        "cache_alloc",
        "cache_size",
        "cache_limit",
        "cache_usage",
        "cache_hit",
        "cache_miss",
        "block_cache",
        "edge",
        "fill",
    )
    selected = {
        name: value
        for name, value in metrics.items()
        if any(fragment in name.lower() for fragment in preferred_fragments)
    }
    if selected:
        return selected
    return metrics


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Wait until ArangoDB startup/index-cache prefill has gone quiescent. "
            "This deliberately waits on process activity after health succeeds, "
            "because startup cache filling may continue after the HTTP endpoint is ready."
        )
    )
    parser.add_argument("--container")
    parser.add_argument("--base-url", default="http://127.0.0.1:8529")
    parser.add_argument("--user", default="root")
    parser.add_argument("--password", default=os.environ.get("ARANGO_ROOT_PASSWORD", "rootpassword"))
    parser.add_argument("--poll-seconds", type=float, default=2.0)
    parser.add_argument("--minimum-wait-seconds", type=float, default=10.0)
    parser.add_argument("--stable-samples", type=int, default=5)
    parser.add_argument("--cpu-threshold-percent", type=float, default=75.0)
    parser.add_argument("--rss-growth-threshold-mib", type=float, default=8.0)
    parser.add_argument("--timeout-seconds", type=float, default=900.0)
    parser.add_argument("--snapshot")
    args = parser.parse_args()

    container = _find_container(args.container)
    inspect = _inspect(container)
    pid = int(inspect.get("State", {}).get("Pid", 0))
    if pid <= 0:
        raise RuntimeError(f"Unable to determine host PID for {container}")

    command = inspect.get("Config", {}).get("Cmd", []) or []
    required = {
        "--rocksdb.auto-fill-index-caches-on-startup": "true",
        "--cache.size": None,
        "--cache.high-water-multiplier": None,
    }
    for option, expected in required.items():
        values = [str(item) for item in command if str(item).startswith(option + "=")]
        if not values:
            raise RuntimeError(f"ArangoDB was not started with required option {option}")
        if expected is not None and values[0].split("=", 1)[1].lower() != expected:
            raise RuntimeError(f"{option} must be {expected}; found {values[0]}")

    print(f"[arango-prefill] container={container} pid={pid}")
    for item in command:
        if any(
            token in str(item)
            for token in (
                "--cache.size=",
                "--cache.high-water-multiplier=",
                "--cache.max-cache-value-size=",
                "--cache.min-value-size-for-edge-compression=",
                "--rocksdb.auto-fill-index-caches-on-startup=",
                "--rocksdb.max-concurrent-index-fill-tasks=",
                "--rocksdb.block-cache-size=",
            )
        ):
            print(f"[arango-prefill] {item}")

    started = time.monotonic()
    previous_wall = time.monotonic()
    previous_cpu = _cpu_seconds(pid)
    previous_rss = _rss_bytes(pid)
    stable = 0
    samples: list[dict[str, Any]] = []
    last_metrics: dict[str, float] = {}

    while True:
        now = time.monotonic()
        if now - started > args.timeout_seconds:
            raise TimeoutError(
                f"ArangoDB cache prefill did not become quiescent within {args.timeout_seconds:.0f}s"
            )

        cpu_now = _cpu_seconds(pid)
        rss_now = _rss_bytes(pid)
        elapsed = max(now - previous_wall, 1e-9)
        cpu_percent = (
            ((cpu_now - previous_cpu) / elapsed * 100.0)
            if cpu_now is not None and previous_cpu is not None
            else 0.0
        )
        rss_growth = max(0, rss_now - previous_rss)

        metrics_ok = False
        try:
            last_metrics = _parse_metrics(
                _http_get(
                    args.base_url.rstrip("/") + "/_admin/metrics/v2",
                    args.user,
                    args.password,
                    timeout=max(2.0, args.poll_seconds),
                )
            )
            metrics_ok = True
        except Exception:
            # During the first seconds after recreation, HTTP may not be ready.
            metrics_ok = False

        sample = {
            "elapsedSeconds": now - started,
            "cpuPercent": cpu_percent,
            "rssBytes": rss_now,
            "rssGrowthBytes": rss_growth,
            "metricsOk": metrics_ok,
        }
        samples.append(sample)

        quiet = (
            metrics_ok
            and now - started >= args.minimum_wait_seconds
            and cpu_percent <= args.cpu_threshold_percent
            and rss_growth <= args.rss_growth_threshold_mib * 1024 * 1024
        )
        stable = stable + 1 if quiet else 0

        print(
            "[arango-prefill] "
            f"elapsed={now-started:6.1f}s "
            f"cpu={cpu_percent:7.1f}% "
            f"rss={rss_now/1024**3:6.3f}GiB "
            f"rss+={rss_growth/1024**2:7.1f}MiB "
            f"stable={stable}/{args.stable_samples}",
            flush=True,
        )

        if stable >= args.stable_samples:
            break

        previous_wall = now
        previous_cpu = cpu_now
        previous_rss = rss_now
        time.sleep(args.poll_seconds)

    selected = _important_metrics(last_metrics)
    result = {
        "container": container,
        "pid": pid,
        "elapsedSeconds": time.monotonic() - started,
        "finalRssBytes": _rss_bytes(pid),
        "importantCacheMetrics": selected,
        "samples": samples,
    }

    if args.snapshot:
        path = Path(args.snapshot)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    print("[arango-prefill] startup/index-cache activity is quiescent")
    print(f"[arango-prefill] final RSS={result['finalRssBytes']/1024**3:.3f} GiB")
    for name, value in sorted(selected.items())[:30]:
        print(f"[arango-prefill] metric {name}={value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
