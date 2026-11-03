"""Incremental gate regressions, including real Git ranges and content caches."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('incremental_quality', Path(__file__).with_name('quality.py'))
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)


class IncrementalTests(unittest.TestCase):
    def test_output_can_be_recovered_from_command(self):
        with tempfile.TemporaryDirectory(prefix='compile path ') as tmp:
            root = Path(tmp)
            build = root / 'build/cpu-tests'
            build.mkdir(parents=True)
            (build / 'compile_commands.json').write_text(json.dumps([{
                'directory': str(build), 'file': str(root / 'src/a.cpp'),
                'command': "c++ -o 'a file.o' -c a.cpp"}]))
            self.assertEqual(q.compile_units(root), [(root / 'src/a.cpp', build / 'a file.o')])

    def test_tidy_key_tracks_dependencies_flags_policy_and_tool(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, header = root / 'a.cpp', root / 'b.hpp'
            src.write_text('#include "b.hpp"\n')
            header.write_text('int a;\n')
            key = q.content_key(['flags', 'tool', 'policy'], {src, header})
            header.write_text('int b;\n')
            self.assertNotEqual(key, q.content_key(['flags', 'tool', 'policy'], {src, header}))
            header.write_text('int a;\n')
            self.assertEqual(key, q.content_key(['flags', 'tool', 'policy'], {src, header}))
            self.assertNotEqual(key, q.content_key(['new flags', 'tool', 'policy'], {src, header}))
            header.unlink()
            self.assertNotEqual(key, q.content_key(['flags', 'tool', 'policy'], {src, header}))

    def test_tidy_reuses_only_successful_unchanged_dependency_results(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'src').mkdir()
            source, header, output = root / 'src/a.cpp', root / 'src/a.hpp', root / 'a.o'
            source.write_text('int a;')
            header.write_text('int b;')
            with patch.object(q, 'selected_units', return_value=[source]), \
                 patch.object(q, 'clang_tool', return_value='clang-tidy-21'), \
                 patch.object(q, 'ninja_dependencies', return_value={output: {source, header}}), \
                 patch.object(q, 'compile_units', return_value=[(source, output)]), \
                 patch.object(q, 'analysis_state', return_value='toolchain/config1'), \
                 patch.object(q, 'run') as run:
                q.check_tidy(root, {'src/a.cpp'})
                q.check_tidy(root, {'src/a.cpp'})
                self.assertEqual(run.call_count, 1)
                header.write_text('int changed;')
                run.side_effect = q.GateError('analysis failure')
                with self.assertRaises(q.GateError):
                    q.check_tidy(root, {'src/a.cpp'})
                self.assertNotIn('src/a.cpp', q.read_json(root / '.polygraph/tidy-cache.json'))
                run.side_effect = None
                q.check_tidy(root, {'src/a.cpp'})
                q.check_tidy(root, {'src/a.cpp'}, full=True)
                self.assertEqual(run.call_count, 4)  # explicit full never uses the hit

    def test_tool_implementation_includes_shared_libraries_and_resources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            executable = root / 'bin/clang-tidy'
            resource = root / 'lib/clang/21/include/stddef.h'
            library = root / 'lib/libclang.so'
            for path in (executable, resource, library):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('implementation')
            result = subprocess.CompletedProcess([], 0, f'libclang.so => {library} (0xabc)\n'.encode(), b'')
            with patch.object(q, 'run', return_value=result):
                files = q.tool_implementation(root, {executable})
            self.assertTrue({executable, resource, library} <= files)
            old = q.content_key([], files)
            library.write_text('updated implementation, same driver/version')
            self.assertNotEqual(old, q.content_key([], files))

    def test_smoke_requires_every_expected_test_and_no_disabled_tests(self):
        tests = [{'name': n, 'properties': []} for n in q.SMOKE_TESTS]
        q.validate_smoke(tests)
        with self.assertRaisesRegex(q.GateError, 'missing'):
            q.validate_smoke(tests[:-1])
        tests[0]['properties'] = [{'name': 'DISABLED', 'value': True}]
        with self.assertRaisesRegex(q.GateError, 'disabled'):
            q.validate_smoke(tests)

    def test_explicit_full_test_profile_cannot_select_smoke(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(q.sys, 'argv', ['quality.py', 'test', '--profile', 'full']), \
                 patch.object(q, 'git', return_value=os.fsencode(tmp)), \
                 patch.object(q, 'fast_tests') as smoke, patch.object(q, 'full_tests') as full:
                self.assertEqual(q.main(), 0)
                full.assert_called_once()
                smoke.assert_not_called()

    def test_sequence_builds_once_before_tidy_and_full_never_uses_smoke(self):
        calls = []
        with patch.object(q, 'check_format'), patch.object(q, 'changed_files', return_value=set()), patch.object(q, 'ensure_configured'), \
             patch.object(q, 'build', side_effect=lambda *_: calls.append('build')), \
             patch.object(q, 'check_tidy', side_effect=lambda *_, **kw: calls.append('tidy')), \
             patch.object(q, 'fast_tests', side_effect=lambda *_: calls.append('smoke')), \
             patch.object(q, 'full_tests', side_effect=lambda *_: calls.append('full')):
            q.sequence(Path('/tmp/project'), set(), True)
        self.assertEqual(calls, ['build', 'tidy', 'full'])


class GitRangeTests(unittest.TestCase):
    """Synthetic objects in disposable repos; never commit or push the checkout."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='push range ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
        self.env.update(GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='test@example.invalid',
                        GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='test@example.invalid')
        self.git('init', '-q')
        (self.root / '.git/info/exclude').write_text('.polygraph/\ncertificates/\n')
        self.base = self.revision('src/base.cpp', 'base')
        self.a = self.revision('src/space name.cpp', 'first')
        self.b = self.revision('include/header.hpp', 'second')

    def git(self, *args, data=None):
        return subprocess.check_output(['git', *args], cwd=self.root, env=self.env, input=data).decode().strip()

    def revision(self, name, text):
        path = self.root / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_text(text)
        self.git('add', '--', name)
        tree = self.git('write-tree')
        parent = subprocess.run(['git', 'rev-parse', '--verify', 'HEAD'], cwd=self.root,
                                env=self.env, capture_output=True, text=True)
        args = ['commit-tree', tree]
        if parent.returncode == 0:
            args += ['-p', parent.stdout.strip()]
        oid = self.git(*args, data=b'fixture\n')
        self.git('update-ref', 'HEAD', oid)
        return oid

    def scope(self, protocol):
        return q.push_scopes(self.root, protocol, 'origin')

    def test_multiple_commits_and_different_tips(self):
        scopes = self.scope(f'refs/heads/a {self.a} refs/heads/a {self.base}\n'
                            f'refs/heads/b {self.b} refs/heads/b {self.base}\n')
        self.assertEqual(scopes[self.a], {'src/space name.cpp'})
        self.assertEqual(scopes[self.b], {'src/space name.cpp', 'include/header.hpp'})

    def test_new_branch_never_trusts_possibly_stale_remote_tracking(self):
        self.git('update-ref', 'refs/remotes/origin/main', self.base)
        scopes = self.scope(f'refs/heads/new {self.b} refs/heads/new {"0"*40}\n')
        self.assertEqual(scopes[self.b], {'src/base.cpp', 'src/space name.cpp', 'include/header.hpp'})

    def test_unknown_remote_falls_back_visibly_to_all_files(self):
        with patch('builtins.print') as output:
            scopes = self.scope(f'refs/heads/a {self.b} refs/heads/a {"f"*40}\n')
        self.assertIn('src/base.cpp', scopes[self.b])
        self.assertTrue(any('fallback' in str(call).lower() for call in output.call_args_list))

    def test_rename_and_deletion_and_reverted_change(self):
        self.git('mv', 'src/space name.cpp', 'src/renamed.cpp')
        self.git('rm', 'src/base.cpp')
        tip = self.revision('include/header.hpp', 'reverted')
        scopes = self.scope(f'refs/heads/a {tip} refs/heads/a {self.base}\n')
        self.assertTrue({'src/base.cpp', 'src/space name.cpp', 'src/renamed.cpp', 'include/header.hpp'} <= scopes[tip])

    def test_additional_tip_checkout_is_isolated_and_reused(self):
        before = (self.root / 'include/header.hpp').read_bytes()
        checkout = q.revision_checkout(self.root, self.a)
        self.assertNotEqual(checkout, self.root)
        self.assertFalse((checkout / 'include/header.hpp').exists())
        self.assertEqual((self.root / 'include/header.hpp').read_bytes(), before)
        with q.gate_lock(checkout):
            q.verify_clean_revision(checkout, self.a)
        self.assertEqual(q.revision_checkout(self.root, self.a), checkout)

    def test_deletions_and_empty_protocol(self):
        self.assertEqual(self.scope(f'(delete) {"0"*40} refs/heads/a {self.base}\n'), {})
        self.assertEqual(self.scope(''), {})

    def test_certificate_is_bound_to_revision_scope_and_state(self):
        with patch.object(q, 'verification_state', return_value='state1'):
            record = q.make_certificate(self.root, self.b, {'src/a.cpp'}, 'fast')
            self.assertTrue(q.certificate_valid(self.root, record, self.b, {'src/a.cpp'}, 'fast'))
            self.assertFalse(q.certificate_valid(self.root, record, self.a, {'src/a.cpp'}, 'fast'))
            self.assertFalse(q.certificate_valid(self.root, record, self.b, {'src/b.cpp'}, 'fast'))
            self.assertFalse(q.certificate_valid(self.root, record, self.b, {'src/a.cpp'}, 'full'))
        with patch.object(q, 'verification_state', return_value='state2'):
            self.assertFalse(q.certificate_valid(self.root, record, self.b, {'src/a.cpp'}, 'fast'))

class VerifiedPushTests(GitRangeTests):
    def test_preflight_precedes_connection_and_hook_reuses_exact_success(self):
        events = []
        original_run = q.subprocess.run
        protocol = f'refs/heads/a {self.b} refs/heads/a {self.base}\n'
        self.git('remote', 'add', 'origin', str(self.root))
        self.git('update-ref', 'refs/remotes/origin/a', self.base)

        def subprocess_run(args, *pos, **kw):
            if args[:4] == ['git', '-c', 'core.hooksPath=.githooks', 'push']:
                events.append('connect')
                with patch.dict(os.environ, kw['env']):
                    q.verify_push(self.root, protocol, 'origin', 'fast')
                return subprocess.CompletedProcess(args, 0)
            return original_run(args, *pos, **kw)

        def sequence(*args, **kw):
            events.append('verify')
            return 'state'

        with patch.object(q, 'hook_status'), patch.object(q, 'revision_checkout', return_value=self.root), \
             patch.object(q, 'sequence', side_effect=sequence), \
             patch.object(q, 'verification_state', return_value='state'), \
             patch.object(q.subprocess, 'run', side_effect=subprocess_run):
            q.verified_push(self.root, 'origin', ['HEAD:refs/heads/a'], 'fast')
        self.assertEqual(events, ['verify', 'connect'])

    def test_stale_certificate_fails_without_running_gate(self):
        directory = self.root / 'certificates'
        directory.mkdir()
        protocol = f'refs/heads/a {self.b} refs/heads/a {self.base}\n'
        with patch.object(q, 'revision_checkout', return_value=self.root), \
             patch.object(q, 'sequence') as sequence, \
             patch.dict(os.environ, POLYGRAPH_VERIFIED_DIRECTORY=str(directory)):
            with self.assertRaisesRegex(q.GateError, 'stale'):
                q.verify_push(self.root, protocol, 'origin', 'fast')
            sequence.assert_not_called()


if __name__ == '__main__':
    unittest.main()
