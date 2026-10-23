#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.measurement.benchmark_cache_configs import _write_comparison  # noqa: E402


def resolve_plans(args: argparse.Namespace) -> list[Path]:
    if args.cache_plans:
        plans = [Path(v).expanduser().resolve() for v in args.cache_plans]
    else:
        root = Path(args.cache_root).expanduser().resolve()
        plans = []
        for config in args.cache_configs:
            config_path = Path(config).expanduser().resolve()
            plan = root / config_path.stem / 'cache-plan.json'
            if not plan.is_file():
                raise FileNotFoundError(f'Existing cache plan not found: {plan}')
            plans.append(plan.resolve())
    missing = [p for p in plans if not p.is_file()]
    if missing:
        raise FileNotFoundError('Missing cache plan(s):\n  ' + '\n  '.join(map(str, missing)))
    return plans


def build_request_plan(source: Path, repetitions: int) -> tuple[dict[str, Any], dict[str, Any]]:
    raw = json.loads(source.read_text(encoding='utf-8'))
    if not isinstance(raw, dict) or not isinstance(raw.get('requests'), list):
        raise ValueError(f'Invalid request plan: {source}')

    defaults = dict(raw.get('defaults') or {})
    measured = [dict(r) for r in raw['requests'] if isinstance(r, dict) and bool(r.get('measure', True))]
    if not measured:
        raise ValueError('Request plan contains no measured request templates')

    def start_of(r: dict[str, Any]) -> str:
        return str(r.get('startUuid', defaults.get('startUuid', '$DOMAIN_START_UUID')))

    representatives: dict[str, dict[str, Any]] = {}
    root_order: list[str] = []
    for request in measured:
        root = start_of(request)
        if root not in representatives:
            representatives[root] = request
            root_order.append(root)

    requests: list[dict[str, Any]] = []

    # Exactly one complete warmup round, excluded from benchmark aggregates.
    for i, root in enumerate(root_order, start=1):
        template = representatives[root]
        item: dict[str, Any] = {
            'id': f"warmup-{i:02d}-{template.get('id', 'request')}",
            'startUuid': root,
            'measure': False,
            'repetition': 0,
        }
        # Warm-up requests are excluded from performance aggregates, so use them
        # for correctness validation.  Request the full distance map exactly once
        # per unique BFS root/category while measured repetitions remain summary-only.
        payload = dict(template.get('payload') or {})
        payload['responseMode'] = 'full'
        item['payload'] = payload
        requests.append(item)

    # N complete measured rounds. atMs is removed because this variant is serial.
    for rep in range(1, repetitions + 1):
        for i, template in enumerate(measured, start=1):
            item = dict(template)
            original_id = str(template.get('id', f'request-{i:02d}'))
            item['id'] = f'r{rep:03d}-{original_id}'
            item['measure'] = True
            item['repetition'] = rep
            item.pop('atMs', None)
            requests.append(item)

    result = dict(raw)
    result['mode'] = 'serial'
    result['requests'] = requests
    metadata = {
        'sourceRequestPlan': str(source),
        'warmupRounds': 1,
        'warmupRoots': root_order,
        'measuredTemplatesPerRound': len(measured),
        'repetitions': repetitions,
        'warmupRequestCount': len(root_order),
        'measuredRequestCount': repetitions * len(measured),
        'totalRequestCount': len(requests),
    }
    return result, metadata


def ensure_free_port(port: int) -> None:
    if not 1 <= port <= 65535:
        raise ValueError('--http-port must be between 1 and 65535')
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        # A just-stopped HTTP server can leave accepted TCP connections in
        # TIME_WAIT. A plain bind() without SO_REUSEADDR may then report
        # EADDRINUSE even though no process is LISTENing on the port.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(('0.0.0.0', port))
        except OSError as exc:
            raise RuntimeError(
                f'HTTP port {port} has an active listener. Stop the stale server or choose another port. '
                'For Prometheus metrics, use the port Prometheus is configured to scrape (normally 8080).'
            ) from exc


def cache_name(plan: Path) -> str:
    return plan.parent.name if plan.name == 'cache-plan.json' else plan.stem


def run_arango_prefill(project: str, snapshot: Path) -> None:
    script = ROOT / 'tools/measurement/restart_arango_bfs_warm.sh'
    if not script.is_file():
        raise FileNotFoundError(
            f'{script} is missing. Apply the Arango prefill patch first or omit '
            '--restart-arango-before-each-cache.'
        )
    subprocess.run([str(script), project, str(snapshot)], cwd=ROOT, check=True)


def restart_arango_cold(project: str, snapshot: Path, timeout_seconds: float = 240.0) -> None:
    """Restart only the existing ArangoDB container.

    This preserves the persistent Docker volumes and the container's exact command/
    environment, but replaces the ArangoDB process. Consequently Arango's in-process
    caches (edge/index cache, RocksDB block cache, AQL plan cache, etc.) start empty.
    The host Linux page cache is deliberately not dropped because that would also
    perturb MMAP/Neo4j/PostgreSQL and make a heterogeneous-cache comparison less fair.
    """

    find = subprocess.run(
        [
            'docker', 'ps', '-aq',
            '--filter', f'label=com.docker.compose.project={project}',
            '--filter', 'label=com.docker.compose.service=arangodb',
        ],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    if find.returncode != 0:
        raise RuntimeError(find.stderr.strip() or 'docker ps failed while locating ArangoDB')
    containers = [line.strip() for line in find.stdout.splitlines() if line.strip()]
    if len(containers) != 1:
        raise RuntimeError(
            f'Expected exactly one ArangoDB container for Compose project {project!r}; '
            f'found {len(containers)}: {containers}'
        )
    container = containers[0]

    def inspect() -> dict[str, Any]:
        result = subprocess.run(
            ['docker', 'inspect', container], cwd=ROOT, text=True,
            capture_output=True, check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or f'docker inspect failed for {container}')
        return json.loads(result.stdout)[0]

    before = inspect()
    before_state = before.get('State', {}) or {}
    before_pid = int(before_state.get('Pid') or 0)
    before_started_at = str(before_state.get('StartedAt') or '')
    command = before.get('Config', {}).get('Cmd', []) or []

    # ArangoDB defaults this startup prefill to false. If the current container
    # explicitly enables it, a plain process restart would immediately warm the
    # edge/index caches again and would not be a valid cold-cache reset. Refuse
    # that situation rather than silently producing misleading measurements.
    auto_fill = False
    for item in map(str, command):
        prefix = '--rocksdb.auto-fill-index-caches-on-startup='
        if item.startswith(prefix):
            auto_fill = item.split('=', 1)[1].strip().lower() in {'1', 'true', 'yes', 'on'}
            break
    if auto_fill:
        raise RuntimeError(
            'Cold Arango reset requested, but the running container has '
            '--rocksdb.auto-fill-index-caches-on-startup=true. Recreate Arango with '
            'that option disabled before running the cold-cache benchmark.'
        )

    print(
        f'[arango-cold] restarting container={before.get("Name", container)} '
        f'pid={before_pid}; volumes/config are preserved',
        flush=True,
    )
    subprocess.run(['docker', 'restart', container], cwd=ROOT, check=True)

    deadline = time.monotonic() + timeout_seconds
    after: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        after = inspect()
        state = after.get('State', {}) or {}
        status = str(state.get('Status') or '')
        pid = int(state.get('Pid') or 0)
        health_obj = state.get('Health') or {}
        health = str(health_obj.get('Status') or 'none')
        started_at = str(state.get('StartedAt') or '')
        if (
            status == 'running'
            and pid > 0
            and started_at
            and started_at != before_started_at
            and health in {'healthy', 'none'}
        ):
            break
        if status in {'dead', 'exited'}:
            raise RuntimeError(
                f'ArangoDB container stopped during cold restart: status={status} health={health}'
            )
        time.sleep(1.0)
    else:
        raise TimeoutError(
            f'ArangoDB did not become healthy within {timeout_seconds:.0f}s after restart'
        )

    assert after is not None
    after_state = after.get('State', {}) or {}
    result = {
        'mode': 'process-cold',
        'composeProject': project,
        'containerId': container,
        'containerName': str(after.get('Name', '')).lstrip('/'),
        'pidBefore': before_pid,
        'pidAfter': int(after_state.get('Pid') or 0),
        'startedAtBefore': before_started_at,
        'startedAtAfter': after_state.get('StartedAt'),
        'autoFillIndexCachesOnStartup': auto_fill,
        'statusAfter': after_state.get('Status'),
        'healthAfter': (after_state.get('Health') or {}).get('Status', 'none'),
        'command': command,
        'persistentVolumesPreserved': True,
        'hostPageCacheDropped': False,
        'clears': [
            'Arango edge/index cache',
            'RocksDB process block cache',
            'AQL query-plan cache',
            'other ArangoDB process-local state',
        ],
    }
    snapshot.parent.mkdir(parents=True, exist_ok=True)
    snapshot.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(
        f'[arango-cold] ready pid={result["pidAfter"]} health={result["healthAfter"]}; '
        'host page cache intentionally preserved',
        flush=True,
    )


def append_timeline(source: Path, destination: Path) -> None:
    if not source.is_file():
        return
    with source.open(encoding='utf-8') as src, destination.open('a', encoding='utf-8') as dst:
        for line in src:
            if line.strip():
                dst.write(line if line.endswith('\n') else line + '\n')


def main() -> int:
    parser = argparse.ArgumentParser(description='Compact repeated PolyGraph cache benchmark.')
    parser.add_argument('--domain', required=True)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--cache-plans', nargs='+')
    inputs.add_argument('--cache-configs', nargs='+')
    parser.add_argument('--cache-root', default='data/ldbc-snb/sf1_0/schema-based/caches')
    parser.add_argument('--request-plan', required=True)
    parser.add_argument('--repetitions', type=int, required=True)
    parser.add_argument('--output-dir', required=True)
    parser.add_argument('--http-port', type=int, default=8080)
    parser.add_argument('--http-server-config')
    parser.add_argument('--http-server-binary')
    parser.add_argument('--http-startup-timeout', type=float, default=0.0)
    parser.add_argument('--http-request-timeout', type=float, default=0.0)
    parser.add_argument(
        '--cold-prime-request-timeout',
        type=float,
        default=0.0,
        help=(
            'Optional timeout for one unmeasured BFS issued immediately after the HTTP server '
            'becomes ready. This primes a freshly restarted ArangoDB before the normal warmup '
            'and measured requests. 0 disables the cold-prime request.'
        ),
    )
    parser.add_argument('--redis-batch-size', type=int, default=10000)
    parser.add_argument('--skip-redis-load', action='store_true')
    parser.add_argument('--restart-arango-before-each-cache', action='store_true')
    parser.add_argument(
        '--cold-restart-arango-before-each-cache',
        action='store_true',
        help=(
            'Restart only the existing ArangoDB container before each cache configuration. '
            'Persistent volumes and container settings are preserved; Arango process-local '
            'caches are emptied. Linux host page cache is not dropped.'
        ),
    )
    parser.add_argument('--project', default='polygraph-sf1-0-schema-based')
    parser.add_argument('--overwrite', action='store_true')
    args = parser.parse_args()

    if args.repetitions < 1:
        raise ValueError('--repetitions must be >= 1')
    if args.cold_prime_request_timeout < 0:
        raise ValueError('--cold-prime-request-timeout must be >= 0')
    if args.restart_arango_before_each_cache and args.cold_restart_arango_before_each_cache:
        raise ValueError(
            '--restart-arango-before-each-cache and '
            '--cold-restart-arango-before-each-cache are mutually exclusive'
        )
    ensure_free_port(args.http_port)
    plans = resolve_plans(args)

    output = Path(args.output_dir).expanduser().resolve()
    if output.exists() and args.overwrite:
        shutil.rmtree(output)
    elif output.exists() and any(output.iterdir()):
        raise ValueError(f'Output directory is not empty: {output}; use --overwrite')
    output.mkdir(parents=True, exist_ok=True)

    control = output / '_benchmark'
    staging = control / 'staging'
    staging.mkdir(parents=True, exist_ok=True)

    source_plan = Path(args.request_plan).expanduser().resolve()
    generated, metadata = build_request_plan(source_plan, args.repetitions)
    generated_path = control / 'request-plan.json'
    generated_path.write_text(json.dumps(generated, indent=2) + '\n', encoding='utf-8')
    (control / 'request-plan.metadata.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')

    print(f"[compact] warmup: 1 round x {metadata['warmupRequestCount']} unique roots", flush=True)
    print(f"[compact] measured: {args.repetitions} rounds x {metadata['measuredTemplatesPerRound']} templates", flush=True)
    print('[compact] cache planning: SKIPPED', flush=True)

    comparison_rows: list[dict[str, Any]] = []
    runs: list[dict[str, Any]] = []
    timeline = output / 'benchmark.timeline.jsonl'

    for index, plan in enumerate(plans, start=1):
        name = cache_name(plan)
        stage = staging / f'{index:02d}-{name}'
        final = output / name
        print(f'\n[{index}/{len(plans)}] {name}', flush=True)

        arango_snapshot: Path | None = None
        arango_snapshot_name: str | None = None
        if args.cold_restart_arango_before_each_cache:
            arango_snapshot = control / f'{name}.arango-cold-reset.json'
            arango_snapshot_name = 'arango-cold-reset.json'
            restart_arango_cold(args.project, arango_snapshot)
        elif args.restart_arango_before_each_cache:
            arango_snapshot = control / f'{name}.arango-prefill.json'
            arango_snapshot_name = 'arango-prefill.json'
            run_arango_prefill(args.project, arango_snapshot)

        command = [
            sys.executable,
            str(ROOT / 'tools/measurement/benchmark_cache_configs.py'),
            '--domain', str(Path(args.domain).expanduser().resolve()),
            '--cache-plans', str(plan),
            '--transport', 'http',
            '--http-port', str(args.http_port),
            '--http-startup-timeout', str(args.http_startup_timeout),
            '--http-request-timeout', str(args.http_request_timeout),
            '--cold-prime-request-timeout', str(args.cold_prime_request_timeout),
            '--http-response-mode', 'summary',
            '--http-request-plan', str(generated_path),
            '--repetitions', str(args.repetitions),
            '--redis-batch-size', str(args.redis_batch_size),
            '--output-dir', str(stage),
            '--overwrite',
        ]
        if args.http_server_config:
            command += ['--http-server-config', args.http_server_config]
        if args.http_server_binary:
            command += ['--http-server-binary', args.http_server_binary]
        if args.skip_redis_load:
            command.append('--skip-redis-load')

        print('+ ' + ' '.join(command), flush=True)
        completed = subprocess.run(command, cwd=ROOT, check=False)
        if completed.returncode != 0:
            print(f'[compact] failed; staging kept at {stage}', file=sys.stderr)
            return completed.returncode or 1

        rows = json.loads((stage / 'comparison.json').read_text(encoding='utf-8'))
        if not isinstance(rows, list) or len(rows) != 1:
            raise ValueError(f'Expected one comparison row in {stage / "comparison.json"}')
        row = dict(rows[0])
        generated_name = str(row.get('cache_name') or name)
        source_run = stage / generated_name
        if not source_run.is_dir():
            dirs = [p for p in stage.iterdir() if p.is_dir()]
            if len(dirs) != 1:
                raise RuntimeError(f'Could not identify cache output under {stage}')
            source_run = dirs[0]

        if final.exists():
            shutil.rmtree(final)
        shutil.move(str(source_run), str(final))
        if (
            arango_snapshot is not None
            and arango_snapshot_name is not None
            and arango_snapshot.is_file()
        ):
            shutil.copy2(arango_snapshot, final / arango_snapshot_name)

        append_timeline(stage / 'benchmark.timeline.jsonl', timeline)
        comparison_rows.append(row)
        runs.append({'cacheName': name, 'cachePlan': str(plan), 'output': str(final), 'status': 'success'})
        shutil.rmtree(stage)

    _write_comparison(output, comparison_rows)
    (output / 'benchmark.json').write_text(
        json.dumps({
            'format': 'polygraph.compact-repeated-cache-benchmark',
            'version': 1,
            'domain': str(Path(args.domain).expanduser().resolve()),
            'requestPlan': str(generated_path),
            'repetitions': args.repetitions,
            'restartArangoBeforeEachCache': args.restart_arango_before_each_cache,
            'coldRestartArangoBeforeEachCache': args.cold_restart_arango_before_each_cache,
            'coldPrimeRequestTimeoutSeconds': args.cold_prime_request_timeout,
            'runs': runs,
        }, indent=2) + '\n', encoding='utf-8'
    )
    if staging.exists() and not any(staging.iterdir()):
        staging.rmdir()

    print(f'\n[compact] comparison: {output / "comparison.csv"}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
