"""Deterministic tests for benchmark metric units and sampling boundaries."""
import unittest
import signal
import subprocess
import sys
import io

from tools.ingestion.benchmark_ldbc import ResourceSamples, parse_time


class BenchmarkMetricsTest(unittest.TestCase):
    def test_gnu_time_cpu_and_rss_units(self):
        metrics = parse_time('10.00 12.50 2.50 2048\n')
        self.assertEqual(metrics['wall_seconds'], 10)
        self.assertEqual(metrics['cpu_user_seconds'], 12.5)
        self.assertEqual(metrics['cpu_system_seconds'], 2.5)
        self.assertEqual(metrics['average_cpu_percent'], 150)
        self.assertEqual(metrics['peak_rss_bytes'], 2097152)

    def test_bad_time_metrics_are_not_reported_as_zero(self):
        for text in ('', '1 2 3', '0 0 0 0', 'nan 1 2 3', '1 -2 3 4'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_time(text)

    def test_round_memory_peak_is_not_sum_of_independent_peaks(self):
        samples = ResourceSamples()
        def stat(cpu, memory):
            return {'cpu_stats': {'cpu_usage': {'total_usage': cpu}}, 'memory_stats': {'usage': memory}}
        samples.add_round({'pg': stat(1000000000, 100), 'neo': stat(2000000000, 500)})
        samples.add_round({'pg': stat(3000000000, 400), 'neo': stat(4000000000, 100)})
        self.assertEqual(samples.peak_round_memory_bytes, 600)
        self.assertEqual(samples.roles['pg']['sampled_peak_memory_bytes'], 400)
        self.assertEqual(samples.roles['neo']['sampled_peak_memory_bytes'], 500)
        self.assertEqual(samples.roles['pg']['cpu_seconds_to_last_sample'], 3)

    def test_short_lived_importer_metrics_survive_disappearance(self):
        samples = ResourceSamples()
        samples.add_round({'admin': {'cpu_stats': {'cpu_usage': {'total_usage': 2000000000}},
                                    'memory_stats': {'usage': 900}}})
        samples.add_round({})
        self.assertEqual(samples.roles['admin']['cpu_seconds_to_last_sample'], 2)
        self.assertEqual(samples.roles['admin']['sampled_peak_memory_bytes'], 900)
        self.assertEqual(samples.rounds, 2)

    def test_missing_stats_are_explicitly_rejected(self):
        with self.assertRaises(ValueError):
            ResourceSamples().add_round({'pg': {'cpu_stats': {}, 'memory_stats': {}}})

    def test_exiting_importer_without_memory_counters_is_an_explicit_missing_observation(self):
        from tools.ingestion.benchmark_ldbc import DockerStats, OWNER_LABEL
        monitor = DockerStats.__new__(DockerStats)
        monitor.unavailable_container_samples = 0
        def get(path):
            if path.startswith('/containers/json?'):
                return [{'Id': 'importer', 'Names': ['/fresh-admin'],
                         'Labels': {OWNER_LABEL: 'fresh'}}]
            return {'cpu_stats': {'cpu_usage': {'total_usage': 123}}, 'memory_stats': {}}
        monitor.get = get
        self.assertEqual(monitor.sample({'generation': 'fresh', 'names': {}}), {})
        self.assertEqual(monitor.unavailable_container_samples, 1)

    def test_transient_monitor_failure_does_not_cancel_the_ingestion_workload(self):
        from tools.ingestion.benchmark_ldbc import sample_resources
        class Monitor:
            failed = False
            def sample(self, state):
                if not self.failed:
                    self.failed = True
                    raise TimeoutError('temporary Docker stats timeout')
                return {'pg': {'cpu_stats': {'cpu_usage': {'total_usage': 2000000000}},
                               'memory_stats': {'usage': 1024}}}
        monitor, samples, output = Monitor(), ResourceSamples(), io.StringIO()
        sample_resources(monitor, {}, samples, output)
        sample_resources(monitor, {}, samples, output)
        self.assertEqual(samples.sampling_errors, 1)
        self.assertEqual(samples.rounds, 1)
        self.assertEqual(samples.roles['pg']['cpu_seconds_to_last_sample'], 2)
        self.assertIn('temporary Docker stats timeout', output.getvalue())

    def test_initial_cancellation_during_cleanup_does_not_leave_resources(self):
        from tools.ingestion.benchmark_ldbc import cleanup_environment
        events = []
        previous = signal.getsignal(signal.SIGINT)
        class Environment:
            def capture_logs(self):
                events.append('logs')
            def cleanup(self):
                signal.raise_signal(signal.SIGINT)
                signal.raise_signal(signal.SIGTERM)
                events.append('removed')
        cleanup_environment(Environment())
        self.assertEqual(events, ['logs', 'removed'])
        self.assertEqual(signal.getsignal(signal.SIGINT), previous)

    def test_cancellation_waits_for_controller_before_cleanup(self):
        from tools.ingestion.benchmark_ldbc import cancel_controller
        child = "import signal,sys,time\nsignal.signal(signal.SIGINT,lambda *_:sys.exit(42))\nprint('ready',flush=True)\ntime.sleep(60)"
        with subprocess.Popen([sys.executable, '-c', child], stdout=subprocess.PIPE,
                              text=True, start_new_session=True) as process:
            try:
                self.assertEqual(process.stdout.readline().strip(), 'ready')
                cancel_controller(process)
                self.assertEqual(process.poll(), 42)
            finally:
                if process.poll() is None:
                    process.kill()


if __name__ == '__main__':
    unittest.main()
