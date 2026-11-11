#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Shared, ownership-checked Docker lifecycle. Database insertion stays in the C++ engine."""
from __future__ import annotations
import base64
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
OWNER_LABEL = 'org.polygraph.ingestion.generation'
PASSWORD_ENV = 'POLYGRAPH_LOAD_PASSWORD'
DEFAULT_IMAGES = {'neo': ('polygraph-neo4j:5.26-enterprise', 'neo4j:5.26-enterprise'),
                  'arango': ('polygraph-arangodb:3.12', 'arangodb:3.12'), 'pg': ('postgres:17',)}


class WorkflowInterrupted(BaseException):
    """Cancellation must escape readiness retries before cleanup begins."""


class WorkflowError(RuntimeError):
    pass


def private_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp-' + secrets.token_hex(4))
    try:
        with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as f:
            json.dump(value, f, indent=2)
            f.write('\n'); f.flush(); os.fsync(f.fileno())
        os.replace(temporary, path)
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        temporary.unlink(missing_ok=True)


def run(argv, **kwargs):
    return subprocess.run(list(map(str, argv)), check=True, text=True, **kwargs)


def request(url, username, password, body=None):
    data = None if body is None else json.dumps(body).encode('utf-8')
    auth = base64.b64encode(f'{username}:{password}'.encode()).decode()
    req = urllib.request.Request(url, data=data, headers={'Authorization': 'Basic ' + auth, 'Content-Type': 'application/json'})
    # All lifecycle API calls target owned loopback services; never send credentials through an environment proxy.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(req, timeout=10) as response:
        result = json.load(response)
    if result.get('error') or result.get('errors'):
        raise WorkflowError('Database API error: ' + json.dumps(result.get('errors', result.get('errorMessage'))))
    return result


def wait_for(check, seconds=180):
    deadline = time.monotonic() + seconds
    while True:
        try:
            return check()
        except Exception as exc:
            if time.monotonic() >= deadline:
                raise WorkflowError(f'Readiness timed out after {seconds}s: {exc}') from exc
            time.sleep(1)


def available_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class DockerEnvironment:
    def __init__(self, output, generation, *, images=None, runner=subprocess.run, timeout=180, memory_mb=512):
        if not re.fullmatch(r'[a-z][a-z0-9-]{2,55}', generation):
            raise WorkflowError('Generation name must be 3..56 lowercase letters/digits/dashes, starting with a letter')
        self.output = Path(output).resolve()
        if any(c in str(self.output) for c in ',\r\n'):
            raise WorkflowError('Docker bind-mount output paths cannot contain commas or newlines')
        if self.output.exists() and any(self.output.iterdir()):
            raise WorkflowError(f'Output directory is not empty: {self.output}. Use a fresh output directory/generation; imports are never retried implicitly.')
        self.output.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.chmod(self.output, 0o700)
        (self.output / 'logs').mkdir(mode=0o700)
        self.generation = generation
        self.database = generation
        self.names = {k: generation + '-' + k for k in ('neo', 'arango', 'pg')}
        self.ports = {k: available_port() for k in ('neo', 'bolt', 'arango', 'pg')}
        while len(set(self.ports.values())) != 4:
            self.ports = {k: available_port() for k in self.ports}
        self.artifacts = self.output / 'artifacts'
        self.state_path = self.output / 'environment.json'
        self.target_path = self.output / 'targets.json'
        self.credentials_path = self.output / 'credentials.json'
        self.runtime_config_path = self.output / 'runtime.secret.json'
        self.password = "polygraph_" + secrets.token_urlsafe(32)
        self.runner = runner
        self.timeout = timeout
        self.memory_mb = memory_mb
        self.image_options = images or {}
        self.state = {'schema_version': 1, 'generation': generation, 'database': self.database, 'output': str(self.output),
                      'containers': {}, 'network': None, 'status': 'created', 'ports': self.ports, 'names': self.names,
                      'images': {}, 'timeout': timeout, 'memory_mb': memory_mb}
        self._secrets_environment()
        self.targets = self._targets()
        private_json(self.credentials_path, {name: {'username': target['username'], 'password': self.password,
                     'database': self.database, 'endpoint': target.get('endpoint', f'127.0.0.1:{self.ports["pg"]}')}
                     for name, target in self.targets['targets'].items()})
        private_json(self.target_path, self.targets)
        self.save()

    @classmethod
    def from_state(cls, path, *, runner=subprocess.run):
        obj = cls.__new__(cls)
        obj.state_path = Path(path).resolve()
        obj.state = json.loads(obj.state_path.read_text())
        if obj.state.get('schema_version') != 1 or Path(obj.state['output']).resolve() != obj.state_path.parent:
            raise WorkflowError('Invalid Docker environment state/path')
        obj.output = obj.state_path.parent
        obj.generation, obj.database = obj.state['generation'], obj.state['database']
        if not re.fullmatch(r'[a-z][a-z0-9-]{2,55}', obj.generation) or obj.database != obj.generation:
            raise WorkflowError('Invalid generated database identity in environment state')
        obj.names, obj.ports = obj.state['names'], obj.state['ports']
        obj.artifacts = obj.output / 'artifacts'
        obj.target_path, obj.credentials_path = obj.output / 'targets.json', obj.output / 'credentials.json'
        obj.runtime_config_path = obj.output / 'runtime.secret.json'
        obj.password = json.loads(obj.credentials_path.read_text())['Neo4J']['password']
        obj.runner, obj.timeout, obj.memory_mb = runner, obj.state['timeout'], obj.state['memory_mb']
        obj.image_options = {}
        obj.targets = json.loads(obj.target_path.read_text())
        obj._secrets_environment()
        return obj

    def _secrets_environment(self):
        self.env = os.environ.copy()
        bypass = self.env.get('NO_PROXY', self.env.get('no_proxy', ''))
        self.env['NO_PROXY'] = self.env['no_proxy'] = ','.join(filter(None, [bypass, '127.0.0.1', 'localhost']))
        self.env.update(POSTGRES_PASSWORD=self.password, ARANGO_ROOT_PASSWORD=self.password,
                        NEO4J_AUTH='neo4j/' + self.password, **{PASSWORD_ENV: self.password})

    def save(self):
        private_json(self.state_path, self.state)

    def _targets(self):
        helper = [sys.executable, str(Path(__file__).resolve())]
        return {'targets': {
            'PostgreSQL': {'database': self.database, 'host': '127.0.0.1', 'port': self.ports['pg'], 'username': 'postgres', 'password_env': PASSWORD_ENV},
            'ArangoDB': {'database': self.database, 'endpoint': f'http://127.0.0.1:{self.ports["arango"]}', 'username': 'root', 'password_env': PASSWORD_ENV},
            'Neo4J': {'database': self.database, 'offline': True, 'endpoint': f'http://127.0.0.1:{self.ports["neo"]}', 'username': 'neo4j', 'password_env': PASSWORD_ENV,
                      'artifact_root': '/artifacts', 'runner': [*helper, '_neo-admin', str(self.state_path)],
                      'preflight_command': [*helper, '_neo-preflight', str(self.state_path)],
                      'start_command': [*helper, '_neo-start', str(self.state_path)], 'startup_attempts': self.timeout}}}

    def docker(self, args, *, allow_failure=False, private_output=False):
        argv = ['docker', *map(str, args)]
        log = self.output / 'logs' / f'docker-{time.time_ns()}-{args[0]}.log'
        try:
            result = self.runner(argv, text=True, capture_output=True, env=self.env)
        except OSError as exc:
            raise WorkflowError(f'Cannot invoke Docker: {exc}. Install Docker and check daemon permissions.') from exc
        with os.fdopen(os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as f:
            # Inspect contains Docker Config.Env, including passwords. Never copy it to a diagnostic log.
            f.write(('Response withheld (private inspection)\n' if private_output else result.stdout) + result.stderr)
        if result.returncode and not allow_failure:
            raise WorkflowError(f'Docker {args[0]} failed (exit {result.returncode}): {result.stderr.strip()[-2000:]}; log: {log}')
        return result

    def select_images(self):
        self.docker(['info', '--format', '{{.ServerVersion}}'])
        for key, candidates in DEFAULT_IMAGES.items():
            candidates = (self.image_options[key],) if self.image_options.get(key) else candidates
            selected = None
            for image in candidates:
                result = self.docker(['image', 'inspect', image], allow_failure=True, private_output=True)
                if result.returncode == 0:
                    selected = (image, json.loads(result.stdout)[0]['Id']); break
            if selected is None:
                image = candidates[-1]
                self.docker(['pull', image])
                selected = (image, json.loads(self.docker(['image', 'inspect', image], private_output=True).stdout)[0]['Id'])
            self.state['images'][key] = {'name': selected[0], 'id': selected[1]}
        self.save()

    def _create(self, key, args):
        result = self.docker(['create', '--pull=never', '--name', self.names[key], '--label', f'{OWNER_LABEL}={self.generation}',
                              '--network', self.state['network']['id'], *args])
        cid = result.stdout.strip()
        if not re.fullmatch(r'[a-f0-9]{64}', cid):
            raise WorkflowError('Docker create did not return a container ID')
        self.state['containers'][key] = {'id': cid, 'name': self.names[key]}
        self.save()  # Record ownership before attempting start.
        return cid

    def owned_container(self, key):
        item = self.state['containers'][key]
        result = self.docker(['inspect', item['id']], allow_failure=True, private_output=True)
        if result.returncode:
            if 'no such' in result.stderr.lower() or 'not found' in result.stderr.lower():
                return None
            raise WorkflowError(f'Cannot inspect owned container: {result.stderr}')
        inspected = json.loads(result.stdout)[0]
        if inspected['Id'] != item['id'] or inspected['Config'].get('Labels', {}).get(OWNER_LABEL) != self.generation:
            raise WorkflowError('Container ownership mismatch; refusing to touch ' + item['id'])
        return inspected

    def provision(self):
        self.select_images()
        network_name = self.generation + '-network'
        network = self.docker(['network', 'create', '--label', f'{OWNER_LABEL}={self.generation}', network_name]).stdout.strip()
        self.state['network'] = {'id': network, 'name': network_name}; self.save()
        data = self.output / 'docker'
        mounts = {}
        for key, folders in {'pg': ('data',), 'arango': ('data', 'apps'), 'neo': ('data', 'logs')}.items():
            mounts[key] = {}
            for folder in folders:
                path = data / key / folder
                path.mkdir(parents=True, mode=0o700)
                mounts[key][folder] = str(path)
        self.state['mounts'] = mounts; self.save()
        pg = self._create('pg', ['-p', f'127.0.0.1:{self.ports["pg"]}:5432', '-e', 'POSTGRES_PASSWORD', '-e', f'POSTGRES_DB={self.database}',
            '--mount', f'type=bind,source={mounts["pg"]["data"]},target=/var/lib/postgresql/data', self.state['images']['pg']['id']])
        self.docker(['start', pg])
        arango = self._create('arango', ['-p', f'127.0.0.1:{self.ports["arango"]}:8529', '-e', 'ARANGO_ROOT_PASSWORD',
            '--mount', f'type=bind,source={mounts["arango"]["data"]},target=/var/lib/arangodb3',
            '--mount', f'type=bind,source={mounts["arango"]["apps"]},target=/var/lib/arangodb3-apps', self.state['images']['arango']['id'],
            '--rocksdb.block-cache-size=67108864', '--cache.size=33554432'])
        self.docker(['start', arango])
        self._create('neo', ['-p', f'127.0.0.1:{self.ports["neo"]}:7474', '-p', f'127.0.0.1:{self.ports["bolt"]}:7687',
            '-e', 'NEO4J_AUTH', '-e', 'NEO4J_ACCEPT_LICENSE_AGREEMENT=yes', '-e', f'NEO4J_initial_dbms_default__database={self.database}',
            '-e', 'NEO4J_server_memory_heap_initial__size=256m', '-e', 'NEO4J_server_memory_heap_max__size=512m', '-e', 'NEO4J_server_memory_pagecache_size=128m',
            '-e', f'NEO4J_server_bolt_advertised__address=127.0.0.1:{self.ports["bolt"]}',
            '--mount', f'type=bind,source={mounts["neo"]["data"]},target=/data',
            '--mount', f'type=bind,source={mounts["neo"]["logs"]},target=/logs', self.state['images']['neo']['id']])
        self.artifacts.mkdir(exist_ok=True)
        (self.artifacts / 'import.conf').write_text('server.directories.data=/data\nserver.directories.logs=/logs\nserver.directories.transaction.logs.root=/data/transactions\n')
        wait_for(lambda: self.docker(['exec', pg, 'pg_isready', '-U', 'postgres', '-d', self.database]), self.timeout)
        arango_endpoint = self.targets['targets']['ArangoDB']['endpoint']
        wait_for(lambda: request(arango_endpoint + '/_api/version', 'root', self.password), self.timeout)
        request(arango_endpoint + '/_api/database', 'root', self.password, {'name': self.database})
        self.state['status'] = 'provisioned'; self.save()

    def neo_preflight(self):
        inspected = self.owned_container('neo')
        if inspected is None:
            raise WorkflowError('Owned Neo4j container is missing')
        data = Path(self.state['mounts']['neo']['data'])
        return {'database': self.database, 'offline': not inspected['State']['Running'],
                'empty': not (data / 'databases' / self.database).exists() and not (data / 'transactions' / self.database).exists()}

    def neo_admin(self, arguments):
        if self.neo_preflight() != {'database': self.database, 'offline': True, 'empty': True}:
            raise WorkflowError('Neo4j import requires stopped fresh owned storage; uncertain stores cannot be reused')
        mounts = self.state['mounts']['neo']
        result = self.docker(['run', '--rm', '--pull=never', '--network=none', '--hostname=localhost', '-w', '/data',
            '--name', self.generation + '-admin', '--label', f'{OWNER_LABEL}={self.generation}', '--user', f'{os.getuid()}:{os.getgid()}',
            '-e', 'NEO4J_ACCEPT_LICENSE_AGREEMENT=yes', '-e', 'HEAP_SIZE=512m',
            '--mount', f'type=bind,source={mounts["data"]},target=/data', '--mount', f'type=bind,source={mounts["logs"]},target=/logs',
            '--mount', f'type=bind,source={self.artifacts},target=/artifacts,readonly', '--entrypoint', 'neo4j-admin',
            self.state['images']['neo']['id'], *arguments, '--additional-config=/artifacts/import.conf',
            f'--max-off-heap-memory={max(256, min(self.memory_mb, 2048))}m'])
        print(result.stdout, end=''); print(result.stderr, end='', file=sys.stderr)

    def neo_start(self):
        inspected = self.owned_container('neo')
        if inspected is None:
            raise WorkflowError('Owned Neo4j container disappeared')
        self.docker(['start', inspected['Id']])
        endpoint = self.targets['targets']['Neo4J']['endpoint']
        def cypher(database, statement, parameters=None):
            return request(endpoint + f'/db/{database}/tx/commit', 'neo4j', self.password,
                           {'statements': [{'statement': statement, 'parameters': parameters or {}}]})
        wait_for(lambda: cypher('system', 'SHOW DATABASES YIELD name RETURN name'), self.timeout)
        registered = cypher('system', 'SHOW DATABASES YIELD name WHERE name=$database RETURN name', {'database': self.database})
        if not registered['results'][0]['data']:
            # Only register this generated database on its own fresh DBMS. Never overwrite.
            cypher('system', f'CREATE DATABASE `{self.database}` IF NOT EXISTS WAIT 120 SECONDS')
        wait_for(lambda: cypher(self.database, 'RETURN 1'), self.timeout)

    def discover_owned(self):
        # Recover the narrow create->state-write interruption window, including ephemeral admin runners.
        expected_names = {**self.names, 'admin': self.generation + '-admin'}
        for key, name in expected_names.items():
            if key in self.state['containers']:
                continue
            result = self.docker(['inspect', name], allow_failure=True, private_output=True)
            if result.returncode:
                if 'no such' in result.stderr.lower() or 'not found' in result.stderr.lower():
                    continue
                raise WorkflowError('Cannot discover owned resource: ' + result.stderr)
            inspected = json.loads(result.stdout)[0]
            if inspected.get('Name') != '/' + name or inspected['Config'].get('Labels', {}).get(OWNER_LABEL) != self.generation:
                raise WorkflowError('Unrecorded container has a different owner; refusing to touch ' + name)
            self.state['containers'][key] = {'id': inspected['Id'], 'name': name}
        if self.state.get('network') is None:
            name = self.generation + '-network'
            result = self.docker(['network', 'inspect', name], allow_failure=True, private_output=True)
            if result.returncode == 0:
                inspected = json.loads(result.stdout)[0]
                if inspected.get('Name') != name or inspected.get('Labels', {}).get(OWNER_LABEL) != self.generation:
                    raise WorkflowError('Unrecorded network has a different owner; refusing cleanup')
                self.state['network'] = {'id': inspected['Id'], 'name': name}
            elif 'no such' not in result.stderr.lower() and 'not found' not in result.stderr.lower():
                raise WorkflowError('Cannot discover owned network: ' + result.stderr)
        self.save()

    def capture_logs(self):
        self.discover_owned()
        for key in self.state['containers']:
            inspected = self.owned_container(key)
            if inspected is not None:
                result = self.docker(['logs', '--tail', '10000', inspected['Id']], allow_failure=True)
                path = self.output / 'logs' / (key + '-server.log')
                with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), 'w') as f:
                    f.write(result.stdout + result.stderr)

    def cleanup(self, *, remove=True):
        # Validate every resource before the first mutation; exact IDs, not reusable names.
        self.discover_owned()
        owned = [(key, self.owned_container(key)) for key in self.state['containers']]
        network = self.state.get('network')
        network_exists = False
        if network and remove:
            result = self.docker(['network', 'inspect', network['id']], allow_failure=True, private_output=True)
            if result.returncode == 0:
                network_exists = True
                value = json.loads(result.stdout)[0]
                if value.get('Labels', {}).get(OWNER_LABEL) != self.generation:
                    raise WorkflowError('Network ownership mismatch; refusing cleanup')
            elif 'no such' not in result.stderr.lower() and 'not found' not in result.stderr.lower():
                raise WorkflowError('Cannot inspect owned network: ' + result.stderr)
        for _, inspected in reversed(owned):
            if inspected is None:
                continue
            if inspected['State']['Running']:
                self.docker(['stop', '-t', '20', inspected['Id']])
            if remove:
                self.docker(['rm', '--volumes', inspected['Id']])
        if network and remove:
            if network_exists:
                self.docker(['network', 'rm', network['id']], allow_failure=False)
            self.state['network'] = None
        self.state['status'] = 'removed' if remove else 'stopped'; self.save()

    def runtime_config(self, backends):
        sys.path.insert(0, str(ROOT))
        from tools.graphio.domain import _backend_entry
        from tools.graphio.id_codec import canonical_backend_name
        backends = [canonical_backend_name(name) for name in backends]
        entries = [_backend_entry(name, True, self.database) for name in backends]
        for entry in entries:
            target = self.targets['targets'][entry['id']]
            connection = entry['connection']
            connection['database'] = self.database
            connection['credentials'] = {'username': target['username'], 'password': self.password}
            connection['endpoint'] = (f'127.0.0.1:{self.ports["pg"]}' if entry['id'] == 'PostgreSQL' else
                target['endpoint'] + (f'/_db/{self.database}/_api/cursor' if entry['id'] == 'ArangoDB' else f'/db/{self.database}/tx/commit'))
        private_json(self.runtime_config_path, {'idRouting': backends, 'backends': entries})
        return self.runtime_config_path


def helper_main():
    if len(sys.argv) < 3:
        raise WorkflowError('Expected helper mode and environment state path')
    environment = DockerEnvironment.from_state(sys.argv[2])
    mode = sys.argv[1]
    if mode == '_neo-preflight':
        print(json.dumps(environment.neo_preflight()))
    elif mode == '_neo-admin':
        environment.neo_admin(sys.argv[3:])
    elif mode == '_neo-start':
        environment.neo_start()
    else:
        raise WorkflowError('Unknown Docker helper mode: ' + mode)


if __name__ == '__main__':
    try:
        helper_main()
    except (WorkflowError, OSError, ValueError) as error:
        print('Docker lifecycle: ' + str(error), file=sys.stderr)
        sys.exit(4)
