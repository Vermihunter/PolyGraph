#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Measure one real LDBC Docker load; repeat with new directories for other scales.

Uses the existing native CLI and ownership-checked Docker lifecycle. GNU time
measures the local process family; Docker Engine stats measure only this run's
containers, including the short-lived Neo4j importer. Raw/private output belongs
under build/. The final benchmark.json contains no generated credentials.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import platform
import signal
import socket
import subprocess
import sys
import time
from urllib.parse import quote, unquote

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.ingestion.docker_environment import DockerEnvironment, OWNER_LABEL, WorkflowInterrupted, private_json
from tools.ingestion.docker_load import cancellation_scope


def parse_time(text):
    wall, user, system, rss_kib = map(float, text.split())
    if not all(math.isfinite(v) and v >= 0 for v in (wall, user, system, rss_kib)) or wall == 0:
        raise ValueError('Invalid GNU time metrics')
    return {'wall_seconds': wall, 'cpu_user_seconds': user, 'cpu_system_seconds': system,
            'average_cpu_percent': 100 * (user + system) / wall, 'peak_rss_bytes': int(rss_kib * 1024)}


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


class ResourceSamples:
    def __init__(self):
        self.roles = {}
        self.rounds = 0
        self.peak_round_memory_bytes = 0

    def add_round(self, values):
        memory_sum = 0
        for role, value in values.items():
            cpu = value.get('cpu_stats', {}).get('cpu_usage', {}).get('total_usage')
            memory = value.get('memory_stats', {}).get('usage')
            if cpu is None or memory is None:
                raise ValueError('Docker did not supply CPU/memory counters for ' + role)
            entry = self.roles.setdefault(role, {'samples': 0, 'cpu_seconds_to_last_sample': 0,
                                                 'sampled_peak_memory_bytes': 0})
            entry['samples'] += 1
            entry['cpu_seconds_to_last_sample'] = max(entry['cpu_seconds_to_last_sample'], cpu / 1e9)
            entry['sampled_peak_memory_bytes'] = max(entry['sampled_peak_memory_bytes'], memory)
            memory_sum += memory
        self.rounds += 1
        self.peak_round_memory_bytes = max(self.peak_round_memory_bytes, memory_sum)


class DockerStats:
    def __init__(self):
        endpoint = os.environ.get('DOCKER_HOST') or subprocess.check_output(
            ['docker', 'context', 'inspect', '--format', '{{.Endpoints.docker.Host}}'], text=True).strip()
        if not endpoint.startswith('unix://'):
            raise ValueError('Resource measurement requires a local Unix-socket Docker context')
        self.path = unquote(endpoint[len('unix://'):])
        self.get('/_ping', raw=True)

    def get(self, path, *, raw=False):
        connection = http.client.HTTPConnection('localhost', timeout=10)
        connection.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.sock.settimeout(10)
        try:
            connection.sock.connect(self.path)
            connection.request('GET', path)
            response = connection.getresponse()
            body = response.read()
            if response.status == 404:
                return None  # A --rm admin importer may exit between discovery and sampling.
            if response.status != 200:
                raise RuntimeError(f'Docker stats API returned HTTP {response.status}')
            return body if raw else json.loads(body)
        finally:
            connection.close()

    def sample(self, state):
        generation = state['generation']
        filters = quote(json.dumps({'label': [OWNER_LABEL + '=' + generation]}))
        expected = {name: role for role, name in state['names'].items()}
        expected[generation + '-admin'] = 'admin'
        values = {}
        for container in self.get('/containers/json?filters=' + filters):
            name = container['Names'][0].lstrip('/')
            if name not in expected or container.get('Labels', {}).get(OWNER_LABEL) != generation:
                raise RuntimeError('Unexpected container identity in filtered resource samples')
            stats = self.get('/containers/' + container['Id'] + '/stats?stream=false&one-shot=true')
            if stats and stats.get('cpu_stats', {}).get('cpu_usage', {}).get('total_usage') is not None:
                values[expected[name]] = stats
        return values


def disk_usage(environment):
    def kib(output):
        return sum(int(line.split()[0]) for line in output.splitlines()) * 1024
    artifacts = kib(subprocess.check_output(['du', '-sk', str(environment.artifacts)], text=True))
    paths = {'pg': ['/var/lib/postgresql/data'], 'arango': ['/var/lib/arangodb3', '/var/lib/arangodb3-apps'],
             'neo': ['/data', '/logs']}
    stores = {}
    for role, folders in paths.items():
        owned = environment.owned_container(role)
        if owned is None or not owned['State']['Running']:
            raise RuntimeError('Cannot measure missing/stopped owned database: ' + role)
        # Read only the fresh owned database directories, inside their own containers.
        output = environment.docker(['exec', '--user', '0', owned['Id'], 'du', '-sk', *folders]).stdout
        stores[role] = kib(output)
    return {'artifacts_and_index_allocated_bytes': artifacts, 'database_binds_allocated_bytes': stores,
            'database_binds_total_allocated_bytes': sum(stores.values()),
            'method': 'du -sk, KiB converted to bytes; final allocated blocks, not peak disk or I/O volume'}


def host_info():
    memory = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        if key in ('MemTotal', 'MemAvailable', 'SwapTotal', 'SwapFree'):
            memory[key + '_bytes'] = int(value.split()[0]) * 1024
    cpu = next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
               if line.startswith('model name'))
    governor = Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor')
    return {'kernel': platform.release(), 'architecture': platform.machine(), 'cpu_model': cpu,
            'logical_cpus': os.cpu_count(), 'cpu_governor': governor.read_text().strip() if governor.exists() else None,
            'memory': memory, 'load_average': os.getloadavg()}


def cancel_controller(process):
    """Wait for load's graceful child termination before touching its Docker resources.

    No forced/finite shutdown bound is claimed: the controller's Docker cleanup
    may itself wait on an unresponsive daemon. Killing just its local process
    group would not prove that independently spawned native helpers have stopped.
    """
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGINT)
        except ProcessLookupError:
            pass
    process.wait()


def cleanup_environment(environment):
    # Also protect the first cancellation signal arriving during post-measurement cleanup.
    previous = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        try:
            environment.capture_logs()
        finally:
            environment.cleanup()
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--placement-config', type=Path, required=True)
    parser.add_argument('--scale-factor', type=float, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--docker', action='store_true', required=True)
    parser.add_argument('--sample-interval-seconds', type=float, default=2)
    parser.add_argument('--threads', type=int, default=3)
    parser.add_argument('--memory-limit-mb', type=int, default=256)
    args = parser.parse_args()
    if not math.isfinite(args.scale_factor) or args.scale_factor <= 0 or not 0.1 <= args.sample_interval_seconds <= 60:
        parser.error('Scale factor must be positive and sampling interval must be 0.1..60s')
    binary = args.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK) or not Path('/usr/bin/time').is_file():
        parser.error('An executable native CLI and GNU /usr/bin/time are required')
    docker = DockerStats()  # Check monitoring access before provisioning anything.
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    load = output / 'load'
    command = [str(binary), 'load', '--adapter', 'ldbc-snb', '--input', str(args.input.resolve()),
               '--placement-config', str(args.placement_config.resolve()), '--docker', '--keep-running',
               '--skip-verification', '--output-dir', str(load), '--threads', str(args.threads),
               '--memory-limit-mb', str(args.memory_limit_mb)]
    samples = ResourceSamples()
    before = host_info()
    started_at = datetime.now(timezone.utc).isoformat()
    script_digest = file_sha256(Path(__file__).resolve())
    binary_digest = file_sha256(binary)
    revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    environment = None
    try:
        with (output / 'load.stdout.log').open('w') as stdout, (output / 'load.stderr.log').open('w') as stderr, \
             (output / 'docker-stats.jsonl').open('w') as stream:
            with subprocess.Popen(['/usr/bin/time', '-f', '%e %U %S %M', '-o', str(output / 'client.metrics'),
                                   *command], stdout=stdout, stderr=stderr, start_new_session=True) as process:
                try:
                    while True:
                        state_path = load / 'environment.json'
                        if state_path.exists():
                            state = json.loads(state_path.read_text())
                            values = docker.sample(state)
                            samples.add_round(values)
                            stream.write(json.dumps({'at': time.time(), 'values': values}) + '\n'); stream.flush()
                        if process.poll() is not None:
                            break
                        time.sleep(args.sample_interval_seconds)
                except BaseException:
                    cancel_controller(process)
                    raise
                if process.returncode:
                    raise RuntimeError(f'Load failed (exit {process.returncode}); see {output / "load.stderr.log"}')
        environment = DockerEnvironment.from_state(load / 'environment.json')
        for role in ('pg', 'arango', 'neo'):
            if role not in samples.roles:
                raise RuntimeError('No resource samples for required service: ' + role)
        manifest = json.loads((environment.artifacts / 'manifest.json').read_text())
        execution = json.loads((environment.artifacts / 'execution-state.json').read_text())
        result = json.loads((load / 'load-result.json').read_text())
        if result['state'] != 'imported' or execution['verification'] != 'skipped':
            raise RuntimeError('The measured workload did not complete as an unchecked import')
        client = parse_time((output / 'client.metrics').read_text())
        counts = manifest['counts']
        entities = counts['nodes'] + counts['edges']
        source_files = [{'path': str(Path(f['path']).relative_to(args.input.resolve())), 'bytes': f['bytes'],
                         'sha256': f['sha256']} for f in manifest['source']['files']]
        cache = binary.parent.parent / 'CMakeCache.txt'
        entries = cache.read_text().splitlines() if cache.exists() else []
        build_type = next((line.split('=', 1)[1] for line in entries if line.startswith('CMAKE_BUILD_TYPE:')), None)
        compiler = next((line.split('=', 1)[1] for line in entries if line.startswith('CMAKE_CXX_COMPILER:')), None)
        version = docker.get('/version')
        report = {'schema_version': 1, 'started_at_utc': started_at, 'repetitions': 1,
            'engine_revision': revision,
            'native_binary_sha256': binary_digest, 'benchmark_script_sha256': script_digest,
            'build_type': build_type, 'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0] if compiler else None,
            'docker': {k: version[k] for k in ('Version', 'ApiVersion', 'Os', 'Arch')},
            'host_before': before, 'host_after': host_info(),
            'dataset': {'name': 'LDBC SNB CsvBasic snapshot', 'scale_factor': args.scale_factor, **counts,
                        'snapshot_bytes': sum(f['bytes'] for f in source_files), 'files': source_files},
            'settings': {'verification': 'skipped', 'partitioner': 'none', 'threads': args.threads,
                         'memory_limit_mb': args.memory_limit_mb,
                         'placement': manifest['configuration']['placement'], 'images': environment.state['images']},
            'preparation': {'timings': manifest['timings'], 'peak_client_rss_bytes': manifest['peak_rss_kb'] * 1024,
                'entities_per_second': entities / manifest['timings']['prepare_seconds'],
                'writers': {name: {k: b[k] for k in ('bytes', 'seconds', 'bytes_per_second', 'counts')}
                            for name, b in manifest['backends'].items()}},
            'native_execution': {'wall_seconds': execution['seconds'], 'peak_client_rss_bytes': execution['peak_rss_kb'] * 1024,
                                 'physical_records_per_second': sum(sum(b['counts'].values()) for b in manifest['backends'].values()) / execution['seconds'],
                                 'backends': execution['backends']},
            'end_to_end': {**client, 'load_controller_seconds': result['duration_seconds'],
                           'entities_per_second': entities / client['wall_seconds']},
            'containers': {'sample_interval_seconds': args.sample_interval_seconds, 'rounds': samples.rounds,
                           'sampled_peak_round_memory_bytes': samples.peak_round_memory_bytes, 'roles': samples.roles},
            'disk': disk_usage(environment),
            'limitations': ['Single warm/uncontrolled-cache run on a shared host; not a scaling curve or speedup claim.',
                'Verification and cleanup excluded from the timed load. No uncertain imports are retried.',
                'Client CPU/RSS exclude Docker daemon/container processes. RSS is maximum process RSS, not summed concurrent RSS.',
                'Docker memory includes cgroup page cache/kernel usage and is sampled, so between-sample peaks may be missed.',
                'Container CPU is cumulative through its last sample; admin is auto-removed, so its final CPU tail may be missed.',
                'Final allocated disk usage is measured while services are running; it is not peak temporary disk or I/O bytes.',
                'Preparation timer excludes its final backend checksum pass/manifest publication; execution timer excludes initial integrity checks/preflight.',
                'Neo4j backend timer covers only admin import; startup/indexes/property repairs are included in execution wall time, not that backend rate.']}
    finally:
        state_path = load / 'environment.json'
        if state_path.exists():
            environment = environment or DockerEnvironment.from_state(state_path)
            cleanup_environment(environment)  # Exact IDs/labels; raw artifacts and bind directories remain.
    report['cleanup'] = 'Owned containers/network removed after measurement; artifacts and database binds retained'
    private_json(output / 'benchmark.json', report)
    print(json.dumps({'benchmark': str(output / 'benchmark.json'), 'seconds': client['wall_seconds']}), flush=True)


if __name__ == '__main__':
    try:
        with cancellation_scope():
            main()
    except WorkflowInterrupted:
        print('LDBC benchmark cancelled; inspect the retained run directory.', file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print('LDBC benchmark: ' + str(exc), file=sys.stderr)
        sys.exit(1)
