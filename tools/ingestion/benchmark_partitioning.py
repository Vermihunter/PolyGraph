#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Measure optional KaMinPar preparation overhead on identical snapshot input.

Every trial calls the native prepare/strict verify commands. No databases or
Docker resources are accessed. --discard-artifacts removes only each freshly
created trial's verified preparation directory, retaining manifests, assignments
and metrics; these reduced trial directories cannot subsequently be executed.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sqlite3
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.ingestion.benchmark_ldbc import cancel_controller, engine_source_fingerprint, file_sha256, host_info, parse_time
from tools.ingestion.docker_environment import WorkflowInterrupted, private_json
from tools.ingestion.docker_load import cancellation_scope


def graph_fingerprint(path):
    """Hash original properties, IDs, endpoints and placement with bounded memory."""
    digest = hashlib.sha256()
    queries = {
        'nodes': 'SELECT id,original,key,type,owner,residency,properties FROM nodes ORDER BY id',
        'edges': 'SELECT id,original,type,owner,from_id,to_id,properties FROM edges ORDER BY id',
        'proxies': 'SELECT backend,node_id FROM proxies ORDER BY backend,node_id',
        'replicas': 'SELECT backend,edge_id FROM replicas ORDER BY backend,edge_id',
    }
    with sqlite3.connect(f'{Path(path).resolve().as_uri()}?mode=ro', uri=True) as db:
        for table, query in queries.items():
            digest.update(table.encode() + b'\0')
            for row in db.execute(query):
                values = list(row)
                if table in ('nodes', 'edges'):
                    properties = json.loads(values[-1])
                    for field in (('partition_id',) if table == 'nodes' else ('from_partition_id', 'to_partition_id', 'is_partition_cut')):
                        properties.pop(field, None)
                    values[-1] = properties
                encoded = json.dumps(values, sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode()
                digest.update(len(encoded).to_bytes(8, 'big'))
                digest.update(encoded)
    return digest.hexdigest()


def summarize(trials):
    groups = {}
    for trial in trials:
        groups.setdefault(trial['variant'], []).append(trial['client']['wall_seconds'])
    baseline = statistics.mean(groups['none'])
    return {name: {'repetitions': len(values), 'mean_prepare_wall_seconds': statistics.mean(values),
                   'min_prepare_wall_seconds': min(values), 'max_prepare_wall_seconds': max(values),
                   'sample_standard_deviation': statistics.stdev(values) if len(values) > 1 else None,
                   'overhead_seconds': statistics.mean(values) - baseline,
                   'overhead_percent': 100 * (statistics.mean(values) / baseline - 1)}
            for name, values in groups.items()}


def run(command, output, stem):
    with (output / (stem + '.stdout.log')).open('w') as stdout, (output / (stem + '.stderr.log')).open('w') as stderr:
        with subprocess.Popen(list(map(str, command)), stdout=stdout, stderr=stderr, start_new_session=True) as process:
            try:
                code = process.wait()
            except BaseException:
                cancel_controller(process)
                raise
            if code:
                raise RuntimeError(f'{stem} failed (exit {code}); inspect {output / (stem + ".stderr.log")}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--placement-config', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--partitions', type=int, nargs='+', default=[2, 8])
    parser.add_argument('--repetitions', type=int, default=2)
    parser.add_argument('--partition-threads', type=int, default=1)
    parser.add_argument('--partition-imbalance', type=float, default=0.03)
    parser.add_argument('--partition-preset', choices=['default', 'eco', 'strong'], default='default')
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--threads', type=int, default=3)
    parser.add_argument('--memory-limit-mb', type=int, default=256)
    parser.add_argument('--discard-artifacts', action='store_true')
    args = parser.parse_args()
    if (args.repetitions < 1 or any(not 2 <= count < 2**32 - 1 for count in args.partitions) or
            len(set(args.partitions)) != len(args.partitions) or not 1 <= args.partition_threads <= 256 or
            not math.isfinite(args.partition_imbalance) or not 0 <= args.partition_imbalance <= 1 or
            not 0 <= args.seed <= 2**31 - 1 or not 1 <= args.threads <= 256 or args.memory_limit_mb < 32):
        parser.error('Invalid repetition/partition/seed/thread/memory settings; memory must be at least 32 MiB')
    binary = args.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK) or not Path('/usr/bin/time').is_file():
        parser.error('An executable partition-enabled native binary and GNU time are required')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    report = {'schema_version': 1, 'started_at_utc': datetime.now(timezone.utc).isoformat(),
              'engine_revision': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
              'engine_source_sha256': engine_source_fingerprint(), 'native_binary_sha256': file_sha256(binary),
              'collector_sha256': file_sha256(Path(__file__).resolve()), 'host_before': host_info(),
              'settings': {'threads': args.threads, 'memory_limit_mb': args.memory_limit_mb,
                           'partition_threads': args.partition_threads, 'imbalance': args.partition_imbalance,
                           'preset': args.partition_preset, 'seed': args.seed, 'partitions': args.partitions,
                           'repetitions': args.repetitions, 'discard_artifacts': args.discard_artifacts},
              'trials': [], 'limitations': ['Preparation only: no native database load or traversal performance claim.',
                'Shared host, uncontrolled caches/swap; client maximum process RSS is not a hard memory budget.',
                'Strict verification and graph fingerprinting are outside GNU-time preparation measurements.',
                'Final allocated disk is measured before optional discard, not peak temporary disk or I/O bytes.']}
    cache = binary.parent.parent / 'CMakeCache.txt'
    lines = cache.read_text().splitlines() if cache.exists() else []
    report['build_type'] = next((line.split('=', 1)[1] for line in lines if line.startswith('CMAKE_BUILD_TYPE:')), None)
    compiler = next((line.split('=', 1)[1] for line in lines if line.startswith('CMAKE_CXX_COMPILER:')), None)
    report['compiler'] = subprocess.check_output([compiler, '--version'], text=True).splitlines()[0] if compiler else None
    baseline = None
    assignments = {}
    variants = [('none', None), *[(f'k{count}', count) for count in args.partitions]]
    for repetition in range(1, args.repetitions + 1):
        for variant, count in variants:
            trial = output / f'{repetition}-{variant}'
            trial.mkdir(mode=0o700)
            artifacts = trial / 'artifacts'
            command = [binary, 'prepare', '--adapter', 'ldbc-snb', '--input', args.input.resolve(),
                       '--placement-config', args.placement_config.resolve(), '--output-dir', artifacts,
                       '--threads', str(args.threads), '--memory-limit-mb', str(args.memory_limit_mb),
                       '--max-record-mb', str(min(8, args.memory_limit_mb // 4)), '--seed', str(args.seed)]
            if count is not None:
                command += ['--partitioner', 'kaminpar', '--partitions', str(count), '--partition-threads', str(args.partition_threads),
                            '--partition-imbalance', str(args.partition_imbalance), '--partition-preset', args.partition_preset]
            print(f'prepare: repetition {repetition}, {variant}; {trial}', flush=True)
            run(['/usr/bin/time', '-f', '%e %U %S %M', '-o', trial / 'client.metrics', *command], trial, 'prepare')
            manifest = json.loads((artifacts / 'manifest.json').read_text())
            verification_start = time.monotonic()
            run([binary, 'verify', '--manifest', artifacts / 'manifest.json', '--strict'], trial, 'verify-strict')
            identity = graph_fingerprint(artifacts / manifest['index']['path'])
            verification_seconds = time.monotonic() - verification_start
            if baseline is None:
                baseline = identity
                report['source'] = manifest['source']
                report['counts'] = manifest['counts']
                report['placement'] = manifest['configuration']['placement']
            if identity != baseline or manifest['counts'] != report['counts'] or manifest['source'] != report['source']:
                raise RuntimeError('Partitioning changed original IDs, properties, endpoints, placement or source identity')
            partitioning = manifest['partitioning']
            if count is not None:
                digest = partitioning['assignment_artifact']['sha256']
                if args.partition_threads == 1 and variant in assignments and digest != assignments[variant]:
                    raise RuntimeError('Sequential fixed-seed assignments changed across repetitions')
                assignments[variant] = digest
                shutil.copy2(artifacts / partitioning['assignment_artifact']['path'], trial / 'partitions.jsonl')
            client = parse_time((trial / 'client.metrics').read_text())
            record = {'variant': variant, 'repetition': repetition, 'client': client,
                      'timings': manifest['timings'], 'partitioning': partitioning,
                      'backend_writers': {name: {key: value[key] for key in ('counts', 'bytes', 'seconds', 'bytes_per_second')}
                                          for name, value in manifest['backends'].items()},
                      'allocated_artifact_bytes': int(subprocess.check_output(['du', '-sk', str(artifacts)], text=True).split()[0]) * 1024,
                      'graph_identity_sha256': identity, 'strict_verified': True, 'verification_seconds': verification_seconds,
                      'artifacts_retained': not args.discard_artifacts}
            private_json(trial / 'manifest.json', manifest)
            private_json(trial / 'measurement.json', record)
            report['trials'].append(record)
            print(f'completed: {variant}, {client["wall_seconds"]:.3f}s, {client["peak_rss_bytes"] / 2**20:.2f} MiB RSS', flush=True)
            if args.discard_artifacts:
                shutil.rmtree(artifacts)
    report['summary'] = summarize(report['trials'])
    report['host_after'] = host_info()
    private_json(output / 'benchmark.json', report)
    print(json.dumps({'benchmark': str(output / 'benchmark.json'), 'summary': report['summary']}, indent=2))


if __name__ == '__main__':
    try:
        with cancellation_scope():
            main()
    except WorkflowInterrupted:
        print('Partition benchmark interrupted; completed measurements/logs retained.', file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print('Partition benchmark: ' + str(error), file=sys.stderr)
        sys.exit(1)
