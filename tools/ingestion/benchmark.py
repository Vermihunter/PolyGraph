#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Measure native preparation and legacy Python conversion on the same raw fixture.

python3 tools/ingestion/benchmark.py --binary build/cpu-tests/bin/polygraph_bulk_import \
  --output-dir build/ingestion-benchmark/run-001 --nodes-per-backend 10000 --repetitions 3

Optional native/Python import command JSON files contain explicit argv arrays targeting
separately provisioned fresh disposable databases. --confirm-execute is required.
No reset/drop/cleanup commands are inferred or run by this tool.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


def measured(argv, directory, label):
    metrics = directory / f'{label}.metrics'
    with (directory / f'{label}.stdout.log').open('w') as stdout, (directory / f'{label}.stderr.log').open('w') as stderr:
        subprocess.run(['/usr/bin/time', '-f', '%e %M', '-o', str(metrics), *map(str, argv)], check=True, stdout=stdout, stderr=stderr)
    elapsed, rss = metrics.read_text().strip().split()
    return {'elapsed_seconds': float(elapsed), 'peak_rss_kb': int(rss), 'command': list(map(str, argv))}


def dataset(path, count):
    path.mkdir()
    for prefix, columns in [('person', ['id', 'firstName', 'birthday']), ('post', ['id', 'content', 'length']), ('place', ['id', 'name', 'url'])]:
        with (path / f'{prefix}_0_0.csv').open('w', newline='') as f:
            writer = csv.writer(f, delimiter='|')
            writer.writerow(columns)
            for i in range(count):
                writer.writerow([i, f'Unicode Žilina {i}, "content"', '2000-01-01' if prefix == 'person' else 64 if prefix == 'post' else 'https://example.org'])
    for prefix in ('person_likes_post', 'post_isLocatedIn_place', 'post_hasCreator_person'):
        with (path / f'{prefix}_0_0.csv').open('w', newline='') as f:
            writer = csv.writer(f, delimiter='|')
            writer.writerow(['from', 'to'])
            for i in range(count):
                writer.writerow([i, i])


def main():
    if len(sys.argv) == 5 and sys.argv[1] == '--python-worker':
        from tools.ldbc_snb.convert import convert_dataset
        input_path, placement, output = map(Path, sys.argv[2:])
        convert_dataset(input_dir=input_path, output=output / 'graph.jsonl', schema_output=output / 'schema.json',
                        placement_config=str(placement), render_placement_graph=False)
        return
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True, help='New directory under ignored build/output paths')
    parser.add_argument('--nodes-per-backend', type=int, default=10000)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--threads', type=int, default=3)
    parser.add_argument('--memory-limit-mb', type=int, default=256)
    parser.add_argument('--native-import-command', type=Path, help='JSON argv array; use fresh target, e.g. an isolated driver')
    parser.add_argument('--python-load-command', type=Path, help='JSON argv array for legacy load on equivalent fresh targets')
    parser.add_argument('--confirm-execute', action='store_true')
    args = parser.parse_args()
    if args.nodes_per_backend < 1 or args.repetitions < 1:
        parser.error('positive sizes and repetitions are required')
    if (args.native_import_command or args.python_load_command) and not args.confirm_execute:
        parser.error('--confirm-execute is required for import commands')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    raw = output / 'raw'
    dataset(raw, args.nodes_per_backend)
    placement = ROOT / 'tests/fixtures/ingestion/placement.json'
    native, legacy = [], []
    for iteration in range(args.repetitions):
        folder = output / str(iteration)
        folder.mkdir()
        artifacts = folder / 'native'
        result = measured([args.binary.resolve(), 'prepare', '--adapter', 'ldbc-snb', '--input', raw, '--placement-config', placement,
                           '--output-dir', artifacts, '--threads', args.threads, '--memory-limit-mb', args.memory_limit_mb], folder, 'native-prepare')
        manifest = json.loads((artifacts / 'manifest.json').read_text())
        result['timings'] = manifest['timings']
        result['backends'] = {n: {k: b[k] for k in ('counts', 'bytes', 'seconds', 'bytes_per_second')} for n, b in manifest['backends'].items()}
        result['partitioning'] = manifest['partitioning']
        native.append(result)
        converted = folder / 'python'
        converted.mkdir()
        legacy.append(measured([sys.executable, Path(__file__).resolve(), '--python-worker', raw, placement, converted], folder, 'python-convert'))
    imports = {}
    for name, command_file in [('native', args.native_import_command), ('python_loader', args.python_load_command)]:
        if command_file:
            command = json.loads(command_file.read_text())
            if not isinstance(command, list) or not command or not all(isinstance(a, str) for a in command):
                parser.error('import command must be a nonempty JSON array of strings')
            imports[name] = measured(command, output, name + '-import')
    summarize = lambda samples: {'mean_seconds': statistics.mean(r['elapsed_seconds'] for r in samples),
                                'stdev_seconds': statistics.stdev(r['elapsed_seconds'] for r in samples) if len(samples) > 1 else None,
                                'max_peak_rss_kb': max(r['peak_rss_kb'] for r in samples)}
    report = {'dataset': {'nodes': args.nodes_per_backend * 3, 'edges': args.nodes_per_backend * 3,
                          'files': [{'name': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(raw.glob('*.csv'))]},
              'native_prepare': native, 'legacy_python_convert': legacy, 'imports': imports,
              'summary': {'native_prepare': summarize(native), 'legacy_python_convert': summarize(legacy)},
              'caveats': ['Native preparation writes three backend artifacts and a disk index; legacy conversion writes graph JSONL/schema/reports. These stages do different work.',
                          'No import comparison is claimed unless both import commands were measured using equivalent isolated targets/settings/cleanup.',
                          'Warm-cache sequential runs; no cache dropping. Debug/Release build is controlled by the caller.']}
    (output / 'benchmark.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['summary'], indent=2))


if __name__ == '__main__':
    main()
