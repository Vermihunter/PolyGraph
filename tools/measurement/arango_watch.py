#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import csv
import json
import os
import re
import signal
import subprocess
import time
import urllib.request
from pathlib import Path
from statistics import mean
from typing import Any


STOP = False
METRIC_KEYWORDS = (
    "rocksdb",
    "cache",
    "scheduler",
    "aql",
    "query",
    "http",
    "process_resident",
    "process_virtual",
)


def _signal_handler(signum, frame) -> None:
    del signum, frame
    global STOP
    STOP = True


def _run(*command: str, check: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=check)


def _find_container(explicit: str | None) -> str:
    if explicit:
        return explicit
    completed = _run("docker", "ps", "--format", "{{.Names}}")
    names = [line.strip() for line in completed.stdout.splitlines() if "arango" in line.lower()]
    if not names:
        raise RuntimeError("No running ArangoDB container found; pass --container")
    return names[0]


def _inspect(container: str) -> dict[str, Any]:
    completed = _run("docker", "inspect", container, check=True)
    return json.loads(completed.stdout)[0]


def _auth_header(user: str, password: str) -> str:
    token = base64.b64encode(f"{user}:{password}".encode()).decode()
    return f"Basic {token}"


def _http_get(url: str, user: str, password: str, timeout: float = 3.0) -> str:
    request = urllib.request.Request(
        url,
        headers={"Authorization": _auth_header(user, password)},
        method="GET",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8", errors="replace")


def _fetch_json(url: str, user: str, password: str) -> Any:
    return json.loads(_http_get(url, user, password))


def _proc_status(pid: int) -> dict[str, int]:
    result: dict[str, int] = {}
    path = Path(f"/proc/{pid}/status")
    if not path.is_file():
        return result
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return result
    for line in text.splitlines():
        if line.startswith(("VmRSS:", "VmSwap:", "VmSize:")):
            key, value, *_ = line.split()
            result[key.rstrip(":")] = int(value) * 1024
    return result


def _proc_io(pid: int, container: str) -> tuple[dict[str, int], str]:
    result: dict[str, int] = {}
    path = Path(f"/proc/{pid}/io")
    text: str | None = None
    if path.is_file():
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = None
    if text is None:
        completed = _run("docker", "exec", container, "cat", "/proc/1/io")
        if completed.returncode == 0:
            text = completed.stdout
            source = "docker-exec"
        else:
            return result, "unavailable"
    else:
        source = "host-proc"
    for line in text.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        try:
            result[key.strip()] = int(value.strip())
        except ValueError:
            pass
    return result, source


def _proc_cpu_seconds(pid: int) -> float | None:
    path = Path(f"/proc/{pid}/stat")
    if not path.is_file():
        return None
    try:
        fields = path.read_text(encoding="utf-8", errors="replace").split()
    except OSError:
        return None
    if len(fields) < 15:
        return None
    ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    return (int(fields[13]) + int(fields[14])) / float(ticks)


def _metric_key(name: str, labels: str | None) -> str:
    return name if not labels else f"{name}{{{labels}}}"


def _parse_metrics(text: str) -> dict[str, float]:
    result: dict[str, float] = {}
    pattern = re.compile(
        r"^([A-Za-z_:][A-Za-z0-9_:]*)(?:\{([^}]*)\})?\s+"
        r"([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?|NaN|Inf|-Inf)$"
    )
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        match = pattern.match(line.strip())
        if not match:
            continue
        name, labels, raw = match.groups()
        if not any(keyword in name.lower() for keyword in METRIC_KEYWORDS):
            continue
        try:
            value = float(raw)
        except ValueError:
            continue
        result[_metric_key(name, labels)] = value
    return result


def _snapshot_metadata(
    output: Path,
    *,
    container: str,
    inspect: dict[str, Any],
    base_url: str,
    database: str,
    user: str,
    password: str,
) -> None:
    (output / "docker-inspect.json").write_text(
        json.dumps(inspect, indent=2) + "\n",
        encoding="utf-8",
    )

    version = _run("docker", "exec", container, "arangod", "--version")
    (output / "arangod-version.txt").write_text(
        version.stdout + version.stderr,
        encoding="utf-8",
    )

    try:
        options = _fetch_json(f"{base_url}/_admin/options", user, password)
        (output / "effective-options.json").write_text(
            json.dumps(options, indent=2) + "\n",
            encoding="utf-8",
        )
    except Exception as exc:
        (output / "effective-options.error.txt").write_text(str(exc) + "\n", encoding="utf-8")

    db_url = f"{base_url}/_db/{database}"
    try:
        collections = _fetch_json(f"{db_url}/_api/collection", user, password)
        edge_collections = [
            item["name"]
            for item in collections.get("result", [])
            if isinstance(item, dict) and int(item.get("type", 0)) == 3 and not item.get("isSystem", False)
        ]
        indexes: dict[str, Any] = {}
        missing_edge_index: list[str] = []
        for collection in edge_collections:
            try:
                data = _fetch_json(
                    f"{db_url}/_api/index?collection={collection}",
                    user,
                    password,
                )
            except Exception as exc:
                indexes[collection] = {"error": str(exc)}
                continue
            indexes[collection] = data
            has_edge = any(
                isinstance(index, dict)
                and index.get("type") == "edge"
                and index.get("fields") == ["_from", "_to"]
                for index in data.get("indexes", [])
            )
            if not has_edge:
                missing_edge_index.append(collection)
        (output / "edge-indexes.json").write_text(
            json.dumps(
                {
                    "edgeCollections": edge_collections,
                    "missingNativeEdgeIndex": missing_edge_index,
                    "indexes": indexes,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
    except Exception as exc:
        (output / "edge-indexes.error.txt").write_text(str(exc) + "\n", encoding="utf-8")


def _write_summary(samples: list[dict[str, Any]], output: Path) -> dict[str, Any]:
    if not samples:
        summary = {"sampleCount": 0}
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        return summary

    rss = [int(row.get("rss_bytes", 0)) for row in samples]
    cpu = [float(row.get("cpu_percent", 0.0)) for row in samples if row.get("cpu_percent") is not None]
    first, last = samples[0], samples[-1]

    def delta(key: str) -> int:
        return int(last.get(key, 0)) - int(first.get(key, 0))

    read_delta = delta("read_bytes")
    write_delta = delta("write_bytes")
    diagnosis: list[str] = []
    if read_delta > 1 * 1024**3:
        diagnosis.append(
            "ArangoDB performed more than 1 GiB of physical reads during the run: cold RocksDB/Linux-cache reads are significant."
        )
    if cpu and mean(cpu) > 400.0 and read_delta < 256 * 1024**2:
        diagnosis.append(
            "ArangoDB is CPU-heavy while physical reads are modest: inspect AQL/index execution rather than storage latency first."
        )
    if read_delta < 64 * 1024**2:
        diagnosis.append(
            "Physical reads are low; a very slow BFS in this state is unlikely to be explained by SSD reads alone."
        )

    summary = {
        "sampleCount": len(samples),
        "durationSeconds": (int(last["timestamp_unix_ns"]) - int(first["timestamp_unix_ns"])) / 1e9,
        "rssBytesMin": min(rss),
        "rssBytesMax": max(rss),
        "cpuPercentMean": mean(cpu) if cpu else None,
        "cpuPercentMax": max(cpu) if cpu else None,
        "physicalReadBytesDelta": read_delta,
        "physicalWriteBytesDelta": write_delta,
        "diagnosis": diagnosis,
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# ArangoDB diagnostic summary",
        "",
        f"- samples: {summary['sampleCount']}",
        f"- duration: {summary['durationSeconds']:.1f} s",
        f"- RSS min/max: {summary['rssBytesMin']/1024**3:.3f} / {summary['rssBytesMax']/1024**3:.3f} GiB",
        f"- CPU mean/max: {summary['cpuPercentMean']:.1f}% / {summary['cpuPercentMax']:.1f}%" if cpu else "- CPU: unavailable",
        f"- physical reads: {read_delta/1024**3:.3f} GiB",
        f"- physical writes: {write_delta/1024**3:.3f} GiB",
        "",
        "## Diagnosis",
    ]
    lines.extend(f"- {item}" for item in diagnosis or ["No simple diagnostic threshold fired."])
    (output / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return summary


def watch(args: argparse.Namespace) -> int:
    output = Path(args.output_dir).expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    container = _find_container(args.container)
    inspect = _inspect(container)
    pid = int(inspect.get("State", {}).get("Pid", 0))
    if pid <= 0:
        raise RuntimeError(f"Could not determine host PID for {container}")

    base_url = args.base_url.rstrip("/")
    _snapshot_metadata(
        output,
        container=container,
        inspect=inspect,
        base_url=base_url,
        database=args.database,
        user=args.user,
        password=args.password,
    )

    fields = [
        "timestamp_unix_ns",
        "elapsed_s",
        "pid",
        "cpu_percent",
        "rss_bytes",
        "swap_bytes",
        "virtual_bytes",
        "read_bytes",
        "write_bytes",
        "rchar",
        "wchar",
        "syscr",
        "syscw",
        "metrics_ok",
        "io_source",
    ]
    samples: list[dict[str, Any]] = []
    metrics_path = output / "metrics.jsonl"
    samples_path = output / "process.csv"

    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    previous_cpu = _proc_cpu_seconds(pid)
    previous_wall = time.monotonic()
    started = previous_wall

    with samples_path.open("w", newline="", encoding="utf-8") as csv_handle, \
         metrics_path.open("w", encoding="utf-8") as metrics_handle:
        writer = csv.DictWriter(csv_handle, fieldnames=fields)
        writer.writeheader()

        while not STOP:
            now_wall = time.monotonic()
            now_ns = time.time_ns()
            status = _proc_status(pid)
            io, io_source = _proc_io(pid, container)
            cpu_seconds = _proc_cpu_seconds(pid)

            cpu_percent = None
            if cpu_seconds is not None and previous_cpu is not None and now_wall > previous_wall:
                cpu_percent = (cpu_seconds - previous_cpu) / (now_wall - previous_wall) * 100.0
            previous_cpu = cpu_seconds
            previous_wall = now_wall

            metrics: dict[str, float] = {}
            metrics_ok = False
            try:
                metrics = _parse_metrics(
                    _http_get(
                        f"{base_url}/_admin/metrics/v2",
                        args.user,
                        args.password,
                        timeout=max(1.0, min(args.interval, 3.0)),
                    )
                )
                metrics_ok = True
            except Exception:
                metrics_ok = False

            row = {
                "timestamp_unix_ns": now_ns,
                "elapsed_s": now_wall - started,
                "pid": pid,
                "cpu_percent": cpu_percent,
                "rss_bytes": status.get("VmRSS", 0),
                "swap_bytes": status.get("VmSwap", 0),
                "virtual_bytes": status.get("VmSize", 0),
                "read_bytes": io.get("read_bytes", 0),
                "write_bytes": io.get("write_bytes", 0),
                "rchar": io.get("rchar", 0),
                "wchar": io.get("wchar", 0),
                "syscr": io.get("syscr", 0),
                "syscw": io.get("syscw", 0),
                "metrics_ok": metrics_ok,
                "io_source": io_source,
            }
            writer.writerow(row)
            csv_handle.flush()
            samples.append(row)

            metrics_handle.write(
                json.dumps(
                    {
                        "timestamp_unix_ns": now_ns,
                        "metrics": metrics,
                    },
                    separators=(",", ":"),
                )
                + "\n"
            )
            metrics_handle.flush()

            deadline = now_wall + args.interval
            while not STOP and time.monotonic() < deadline:
                time.sleep(min(0.1, deadline - time.monotonic()))

    _write_summary(samples, output)
    return 0


def _load_process_samples(path: Path) -> list[dict[str, Any]]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    result: list[dict[str, Any]] = []
    for row in rows:
        converted: dict[str, Any] = dict(row)
        for field in (
            "timestamp_unix_ns",
            "pid",
            "rss_bytes",
            "swap_bytes",
            "virtual_bytes",
            "read_bytes",
            "write_bytes",
            "rchar",
            "wchar",
            "syscr",
            "syscw",
        ):
            converted[field] = int(float(row.get(field) or 0))
        converted["cpu_percent"] = float(row["cpu_percent"]) if row.get("cpu_percent") else None
        result.append(converted)
    return result


def _nearest_before(samples: list[dict[str, Any]], ts: int) -> dict[str, Any] | None:
    candidates = [sample for sample in samples if sample["timestamp_unix_ns"] <= ts]
    return candidates[-1] if candidates else (samples[0] if samples else None)


def _nearest_after(samples: list[dict[str, Any]], ts: int) -> dict[str, Any] | None:
    for sample in samples:
        if sample["timestamp_unix_ns"] >= ts:
            return sample
    return samples[-1] if samples else None


def analyze(args: argparse.Namespace) -> int:
    diagnostics = Path(args.diagnostics).expanduser().resolve()
    measurements = Path(args.measurements).expanduser().resolve()
    samples = _load_process_samples(diagnostics / "process.csv")

    with measurements.open(newline="", encoding="utf-8") as handle:
        requests = list(csv.DictReader(handle, delimiter=";"))

    rows: list[dict[str, Any]] = []
    for request in requests:
        if not request.get("started_unix_ns") or not request.get("finished_unix_ns"):
            continue
        start_ns = int(request["started_unix_ns"])
        finish_ns = int(request["finished_unix_ns"])
        before = _nearest_before(samples, start_ns)
        after = _nearest_after(samples, finish_ns)
        inside = [
            sample for sample in samples
            if start_ns <= sample["timestamp_unix_ns"] <= finish_ns
        ]
        if before is None or after is None:
            continue

        read_delta = after["read_bytes"] - before["read_bytes"]
        write_delta = after["write_bytes"] - before["write_bytes"]
        cpu = [sample["cpu_percent"] for sample in inside if sample["cpu_percent"] is not None]
        rss = [sample["rss_bytes"] for sample in inside]
        rows.append(
            {
                "request_id": request.get("request_id"),
                "start_uuid": request.get("start_uuid"),
                "measured": request.get("measured"),
                "server_bfs_ms": float(request.get("server_bfs_ms") or 0),
                "arango_physical_read_bytes": read_delta,
                "arango_physical_write_bytes": write_delta,
                "arango_cpu_percent_mean": mean(cpu) if cpu else None,
                "arango_cpu_percent_max": max(cpu) if cpu else None,
                "arango_rss_bytes_max": max(rss) if rss else None,
            }
        )

    output_json = diagnostics / "request-correlation.json"
    output_json.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")

    fields = [
        "request_id",
        "start_uuid",
        "measured",
        "server_bfs_ms",
        "arango_physical_read_bytes",
        "arango_physical_write_bytes",
        "arango_cpu_percent_mean",
        "arango_cpu_percent_max",
        "arango_rss_bytes_max",
    ]
    with (diagnostics / "request-correlation.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    md = [
        "# BFS ↔ ArangoDB correlation",
        "",
        "| Request | BFS ms | Arango disk read GiB | CPU mean | RSS max GiB |",
        "|---|---:|---:|---:|---:|",
    ]
    for row in rows:
        cpu = row["arango_cpu_percent_mean"]
        rss = row["arango_rss_bytes_max"]
        md.append(
            f"| {row['request_id']} | {row['server_bfs_ms']:.3f} | "
            f"{row['arango_physical_read_bytes']/1024**3:.3f} | "
            f"{'n/a' if cpu is None else f'{cpu:.1f}%'} | "
            f"{'n/a' if rss is None else f'{rss/1024**3:.3f}'} |"
        )

    md.extend(
        [
            "",
            "## How to read this",
            "",
            "- Slow + large physical-read delta: cold storage / cache miss path.",
            "- Same root slow again + large physical-read delta again: working set likely exceeds useful cache capacity (thrashing).",
            "- Slow + little physical I/O + high CPU: inspect AQL plan/index behavior and Arango scheduler/CPU saturation.",
            "- Fast second warmup + low physical I/O: strong warm-cache effect.",
        ]
    )
    (diagnostics / "request-correlation.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Collect and analyze ArangoDB benchmark diagnostics.")
    actions = parser.add_subparsers(dest="action", required=True)

    watch_parser = actions.add_parser("watch")
    watch_parser.add_argument("--output-dir", required=True)
    watch_parser.add_argument("--container")
    watch_parser.add_argument("--base-url", default="http://127.0.0.1:8529")
    watch_parser.add_argument("--database", default="ldbc_snb_sf1_0")
    watch_parser.add_argument("--user", default="root")
    watch_parser.add_argument("--password", default=os.environ.get("ARANGO_ROOT_PASSWORD", "rootpassword"))
    watch_parser.add_argument("--interval", type=float, default=1.0)
    watch_parser.set_defaults(handler=watch)

    analyze_parser = actions.add_parser("analyze")
    analyze_parser.add_argument("--diagnostics", required=True)
    analyze_parser.add_argument("--measurements", required=True)
    analyze_parser.set_defaults(handler=analyze)

    args = parser.parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
