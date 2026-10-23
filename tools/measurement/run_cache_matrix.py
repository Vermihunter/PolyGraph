#!/usr/bin/env python3
from __future__ import annotations

import argparse
import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.workflows.common import ensure_redis, load_json, polygraph, run


def repo_path(value: str) -> Path:
    path = Path(value).expanduser()
    return path.resolve() if path.is_absolute() else (ROOT / path).resolve()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a cache benchmark matrix N times using existing cache plans."
    )
    parser.add_argument(
        "--config", default=str(ROOT / "config/benchmarks/sf1-cache-matrix.json")
    )
    parser.add_argument("--repetitions", type=int, help="Override repetitions from the config.")
    parser.add_argument("--output-dir", help="Override outputDir from the config.")
    parser.add_argument("--skip-activate", action="store_true")
    parser.add_argument("--skip-redis-start", action="store_true")
    parser.add_argument("--build-plans", action="store_true", help="Rebuild cache plans before benchmarking.")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    cfg_path = Path(args.config).expanduser().resolve()
    cfg = load_json(cfg_path)
    repetitions = args.repetitions or int(cfg.get("repetitions", 10))
    if repetitions < 1:
        raise ValueError("repetitions must be >= 1")

    sf = str(cfg.get("scaleFactor", "1"))
    distribution = str(cfg.get("distribution", "schema-based"))
    domain = repo_path(str(cfg["domain"]))
    cache_root = repo_path(str(cfg["cacheRoot"]))
    request_plan = repo_path(str(cfg["requestPlan"]))
    output_dir = repo_path(args.output_dir or str(cfg["outputDir"]))
    config_pattern = str(cfg["cacheConfigGlob"])
    pattern = config_pattern if Path(config_pattern).is_absolute() else str(ROOT / config_pattern)
    cache_configs = [Path(value).resolve() for value in sorted(glob.glob(pattern))]
    configured_names = cfg.get("cacheConfigNames")
    if configured_names is not None:
        if not isinstance(configured_names, list) or not all(isinstance(value, str) for value in configured_names):
            raise ValueError("cacheConfigNames must be an array of cache-config stems")
        by_stem = {path.stem: path for path in cache_configs}
        missing_names = [name for name in configured_names if name not in by_stem]
        if missing_names:
            raise ValueError(
                "Configured cacheConfigNames were not found under cacheConfigGlob: "
                + ", ".join(missing_names)
            )
        cache_configs = [by_stem[name] for name in configured_names]
    if bool(cfg.get("reverseCacheOrder", False)):
        cache_configs.reverse()
    #if len(cache_configs) != 8:
     #   raise ValueError(f"Expected exactly 8 cache configs, found {len(cache_configs)} from {config_pattern}")

    if not args.skip_activate:
        polygraph("instance", sf, distribution, "activate")

    if args.build_plans:
        run([
            sys.executable, ROOT / "tools/cache/build_benchmark_plans.py",
            "--sf", sf,
            "--distribution", distribution,
            "--clean",
        ])

    plans = [cache_root / config.stem / "cache-plan.json" for config in cache_configs]
    missing = [path for path in plans if not path.is_file()]
    if missing:
        raise FileNotFoundError(
            "Cache plans are missing. Run tools/cache/build_benchmark_plans.py first, "
            "or pass --build-plans. Missing:\n  " + "\n  ".join(str(path) for path in missing)
        )

    if not args.skip_redis_start:
        env_value = cfg.get("redisEnvFile")
        ensure_redis(env_file=repo_path(str(env_value)) if env_value else None)

    command: list[str | Path] = [
        sys.executable,
        ROOT / "tools/measurement/benchmark_compact_repeated.py",
        "--domain", domain,
        "--cache-plans", *plans,
        "--request-plan", request_plan,
        "--repetitions", str(repetitions),
        "--output-dir", output_dir,
        "--http-port", str(int(cfg.get("httpPort", 8080))),
        "--http-startup-timeout", str(float(cfg.get("httpStartupTimeoutSeconds", 0.0))),
        "--http-request-timeout", str(float(cfg.get("httpRequestTimeoutSeconds", 0.0))),
        "--cold-prime-request-timeout", str(float(cfg.get("arangoColdPrimeTimeoutSeconds", 0.0))),
    ]
    warm_restart = bool(cfg.get('restartArangoBeforeEachCache', False))
    cold_restart = bool(cfg.get('coldRestartArangoBeforeEachCache', False))
    if warm_restart and cold_restart:
        raise ValueError(
            'Benchmark config cannot enable both restartArangoBeforeEachCache and '
            'coldRestartArangoBeforeEachCache'
        )
    if warm_restart:
        command.append('--restart-arango-before-each-cache')
    if cold_restart:
        command.append('--cold-restart-arango-before-each-cache')
    if args.overwrite:
        command.append("--overwrite")
    run(command)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
