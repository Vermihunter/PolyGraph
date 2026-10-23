#!/usr/bin/env python3
"""Generate fair 1/N-GPU PolyGraph cache + benchmark configs.

The default fixed-total mode preserves the aggregate GPU memory budget from the
base cache config and divides it across N devices. This is the useful strong-
scaling comparison: approximately the same cacheable graph region, more GPUs.

The optional per-gpu mode gives every GPU the base per-device budget. That tests
capacity scaling as well as compute/communication scaling and therefore answers a
different question.
"""

from __future__ import annotations

import argparse
import copy
import json
import re
from pathlib import Path
from typing import Any

UNITS = {
    "b": 1,
    "kb": 1000,
    "mb": 1000**2,
    "gb": 1000**3,
    "kib": 1024,
    "mib": 1024**2,
    "gib": 1024**3,
}


def parse_bytes(value: Any) -> int:
    if isinstance(value, int):
        if value <= 0:
            raise ValueError("GPU memory must be positive")
        return value
    if not isinstance(value, str):
        raise ValueError(f"unsupported GPU memory value: {value!r}")
    text = value.strip().lower()
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)\s*([kmgt]?i?b)?", text)
    if not match:
        raise ValueError(f"cannot parse GPU memory value: {value!r}")
    number = float(match.group(1))
    unit = match.group(2) or "b"
    multiplier = UNITS.get(unit)
    if multiplier is None:
        raise ValueError(f"unsupported memory unit in {value!r}")
    result = int(number * multiplier)
    if result <= 0:
        raise ValueError("GPU memory must be positive")
    return result


def format_like(value: Any, bytes_value: int) -> Any:
    if isinstance(value, int):
        return bytes_value
    # MiB is exact, simple, and accepted anywhere a GiB-style value is used.
    mib = max(1, bytes_value // (1024**2))
    return f"{mib}MiB"


def find_gpu_list(root: Any) -> tuple[list[Any], str]:
    matches: list[tuple[list[Any], str]] = []

    def walk(value: Any, path: str) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                child_path = f"{path}.{key}" if path else key
                if key == "gpus" and isinstance(child, list):
                    matches.append((child, child_path))
                walk(child, child_path)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                walk(child, f"{path}[{index}]")

    walk(root, "")
    if not matches:
        raise ValueError("cache config has no 'gpus' array")
    if len(matches) > 1:
        paths = ", ".join(path for _, path in matches)
        raise ValueError(f"cache config has multiple 'gpus' arrays: {paths}")
    return matches[0]


def replace_gpu_list(root: Any, new_gpus: list[dict[str, Any]]) -> None:
    if isinstance(root, dict):
        for key, child in root.items():
            if key == "gpus" and isinstance(child, list):
                root[key] = new_gpus
                return
            try:
                replace_gpu_list(child, new_gpus)
                return
            except LookupError:
                pass
    elif isinstance(root, list):
        for child in root:
            try:
                replace_gpu_list(child, new_gpus)
                return
            except LookupError:
                pass
    raise LookupError


def make_cache(base: dict[str, Any], count: int, memory_mode: str) -> dict[str, Any]:
    original, _ = find_gpu_list(base)
    if not original:
        raise ValueError("base cache config has an empty gpus array")
    if not all(isinstance(gpu, dict) for gpu in original):
        raise ValueError("every gpus[] entry must be an object")
    if not all("memory" in gpu for gpu in original):
        raise ValueError("every base gpus[] entry must contain memory")

    base_total = sum(parse_bytes(gpu["memory"]) for gpu in original)
    template = copy.deepcopy(original[0])
    per_device = parse_bytes(template["memory"])
    if memory_mode == "fixed-total":
        per_device = max(1, base_total // count)

    generated: list[dict[str, Any]] = []
    for device in range(count):
        entry = copy.deepcopy(template)
        entry["device_id"] = device
        entry["memory"] = format_like(template["memory"], per_device)
        generated.append(entry)

    result = copy.deepcopy(base)
    replace_gpu_list(result, generated)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-cache", required=True, type=Path)
    parser.add_argument("--base-benchmark", required=True, type=Path)
    parser.add_argument("--counts", default="1,2,4")
    parser.add_argument("--memory-mode", choices=("fixed-total", "per-gpu"), default="fixed-total")
    parser.add_argument("--cache-output-dir", type=Path,
                        default=Path("config/cache/benchmark/gpu-scale"))
    parser.add_argument("--benchmark-output-dir", type=Path,
                        default=Path("config/benchmarks/generated"))
    parser.add_argument("--measurements-dir", type=Path,
                        default=Path("measurements/sf1-gpu-scale"))
    args = parser.parse_args()

    base_cache = json.loads(args.base_cache.read_text())
    base_benchmark = json.loads(args.base_benchmark.read_text())
    counts = [int(value) for value in args.counts.split(",") if value.strip()]
    if not counts or any(value <= 0 for value in counts):
        raise SystemExit("--counts must contain positive integers")

    args.cache_output_dir.mkdir(parents=True, exist_ok=True)
    args.benchmark_output_dir.mkdir(parents=True, exist_ok=True)

    for count in counts:
        cache = make_cache(base_cache, count, args.memory_mode)
        cache_name = f"{args.base_cache.stem}-g{count}-{args.memory_mode}.json"
        cache_path = args.cache_output_dir / cache_name
        cache_path.write_text(json.dumps(cache, indent=2) + "\n")

        benchmark = copy.deepcopy(base_benchmark)
        benchmark["cacheConfigGlob"] = str(cache_path)
        benchmark["outputDir"] = str(args.measurements_dir / f"g{count}-{args.memory_mode}")
        bench_path = args.benchmark_output_dir / f"sf1-gpu-scale-g{count}-{args.memory_mode}.json"
        bench_path.write_text(json.dumps(benchmark, indent=2) + "\n")
        print(f"{count}\t{cache_path}\t{bench_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
