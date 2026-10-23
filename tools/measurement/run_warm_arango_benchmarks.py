#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RESTART = ROOT / "tools" / "measurement" / "restart_arango_bfs_warm.sh"
BENCHMARK = ROOT / "tools" / "measurement" / "benchmark_steady_arango.py"


def _resolve_http_port(requested: int) -> int:
    if requested < 0 or requested > 65535:
        raise ValueError("--http-port must be between 0 and 65535")
    if requested != 0:
        return requested

    # benchmark_cache_configs.py requires a concrete port in [1, 65535].
    # Resolve port 0 here before delegating to benchmark_steady_arango.py.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("0.0.0.0", 0))
        return int(sock.getsockname()[1])


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark cache configurations one-by-one. Before every configuration, "
            "restart ArangoDB with the BFS cache profile and wait for index-cache "
            "prefill to quiesce, so every placement starts from the same warm DB state."
        )
    )
    parser.add_argument("--domain", required=True)
    parser.add_argument("--cache-configs", nargs="+", required=True)
    parser.add_argument("--cache-root", default="data/ldbc-snb/sf1_0/schema-based/caches")
    parser.add_argument("--request-plan", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--project", default="polygraph-sf1-0-schema-based")
    parser.add_argument("--warmup-rounds", type=int, default=1)
    parser.add_argument("--arango-database", default="ldbc_snb_sf1_0")
    parser.add_argument("--arango-user", default="root")
    parser.add_argument("--arango-password", default=os.environ.get("ARANGO_ROOT_PASSWORD", "rootpassword"))
    parser.add_argument("--http-port", type=int, default=0)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    output = Path(args.output_dir).expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)

    manifest: list[dict[str, object]] = []
    return_code = 0

    for index, config in enumerate(args.cache_configs, start=1):
        config_path = Path(config).expanduser().resolve()
        name = config_path.stem
        run_dir = output / f"{index:02d}-{name}"
        prefill_dir = run_dir / "arango-prefill"
        benchmark_dir = run_dir / "benchmark"
        prefill_dir.mkdir(parents=True, exist_ok=True)

        print()
        print(f"[{index}/{len(args.cache_configs)}] {name}")
        http_port = _resolve_http_port(args.http_port)
        print("  restarting ArangoDB and prefilling edge/index caches")
        print(f"  HTTP port: {http_port}")

        snapshot = prefill_dir / "prefill.json"
        restart = subprocess.run(
            [str(RESTART), args.project, str(snapshot)],
            cwd=ROOT,
            check=False,
        )
        if restart.returncode != 0:
            return restart.returncode

        command = [
            sys.executable,
            str(BENCHMARK),
            "--domain",
            args.domain,
            "--cache-configs",
            str(config_path),
            "--cache-root",
            args.cache_root,
            "--request-plan",
            args.request_plan,
            "--warmup-rounds",
            str(args.warmup_rounds),
            "--output-dir",
            str(benchmark_dir),
            "--arango-database",
            args.arango_database,
            "--arango-user",
            args.arango_user,
            "--arango-password",
            args.arango_password,
            "--http-port",
            str(http_port),
            "--overwrite",
        ]
        completed = subprocess.run(command, cwd=ROOT, check=False)
        return_code = return_code or completed.returncode

        manifest.append(
            {
                "index": index,
                "cacheConfig": str(config_path),
                "name": name,
                "prefillSnapshot": str(snapshot),
                "benchmarkOutput": str(benchmark_dir),
                "returncode": completed.returncode,
            }
        )

        if completed.returncode != 0:
            break

    (output / "warm-arango-benchmark-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
