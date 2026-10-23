from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.event_log_average import analyze_log_tree, normalize_event


class AverageReportIndexTests(unittest.TestCase):
    def test_current_sequence_elapsed_schema_generates_a_nonempty_index(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            logs = root / "logs" / "run-group"
            output = root / "reconstruction"
            logs.mkdir(parents=True)
            (logs / "1.jsonl").write_text(
                "\n".join([
                    '{"event":"work.start","sequence":1,"elapsed_ns":1000,"thread_id":7}',
                    '{"event":"work.end","sequence":2,"elapsed_ns":5000,"thread_id":7}',
                ]) + "\n",
                encoding="utf-8",
            )

            result = analyze_log_tree(logs.parent, output_root=output)
            index = Path(result["index"])
            rendered = index.read_text(encoding="utf-8")

            self.assertIn("Global — all logs", rendered)
            self.assertIn("run-group", rendered)
            self.assertIn("averages/Global/report.html", rendered)
            self.assertTrue((output / "averages" / "Global" / "report.html").is_file())

    def test_timestamp_unix_ns_is_accepted_as_a_timing_fallback(self) -> None:
        normalized = normalize_event(
            {
                "event": "work.start",
                "sequence": 9,
                "timestamp_unix_ns": 123456,
                "thread_id": 1,
            }
        )
        self.assertEqual(normalized["seq"], 9)
        self.assertEqual(normalized["steady_ns"], 123456)


if __name__ == "__main__":
    unittest.main()
