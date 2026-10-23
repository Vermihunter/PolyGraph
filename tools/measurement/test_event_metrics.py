from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from tools.measurement.compare_only import load_samples, write_compare_dashboard
from tools.measurement.event_metrics import discover_intervals, export_metrics, load_config, visualize_metrics


def _event(name: str, seq: int, timestamp_ns: int, *, thread: int = 1) -> dict:
    return {
        "event": name,
        "seq": seq,
        "steady_ns": timestamp_ns,
        "timestamp_unix_ns": timestamp_ns,
        "thread_id": thread,
    }


def _write_jsonl(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


def _make_run(root: Path, name: str, *, offset: int) -> Path:
    run = root / name
    req = run / "events" / "requests" / "r1.events.jsonl"
    start = 1_700_000_000_000_000_000 + offset
    end = start + 2_000_000_000
    rows = [
        _event("full_bfs.start", 1, start),
        _event("neo4j.query.start", 2, start + 100_000_000),
        _event("neo4j.query.end", 3, start + 700_000_000),
        _event("full_bfs.end", 4, end),
    ]
    _write_jsonl(req, rows)
    _write_jsonl(run / "events" / "server.events.jsonl", rows)
    (run / "events" / "requests" / "index.json").write_text(
        json.dumps([
            {
                "request_id": "r1",
                "run_index": 1,
                "started_unix_ns": start - 50_000_000,
                "finished_unix_ns": end + 50_000_000,
                "measured": True,
                "events_jsonl": str(req.resolve()),
            }
        ]),
        encoding="utf-8",
    )
    return run


class _PrometheusHandler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        parsed = urlparse(self.path)
        assert parsed.path == "/api/v1/query_range"
        query = parse_qs(parsed.query)
        start = float(query["start"][0])
        end = float(query["end"][0])
        mid = (start + end) / 2.0
        body = {
            "status": "success",
            "data": {
                "resultType": "matrix",
                "result": [
                    {
                        "metric": {"instance": "test"},
                        "values": [[start, "1"], [mid, "2"], [end, "3"]],
                    }
                ],
            },
        }
        payload = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):  # noqa: A003
        return


def test_discovery_export_visualize_and_compare(tmp_path: Path) -> None:
    _make_run(tmp_path, "01-gpu", offset=0)
    _make_run(tmp_path, "08-no-cache", offset=10_000_000_000)

    server = ThreadingHTTPServer(("127.0.0.1", 0), _PrometheusHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        config_path = tmp_path / "metrics.json"
        config_path.write_text(
            json.dumps(
                {
                    "prometheus": {"url": f"http://127.0.0.1:{server.server_port}", "defaultStepSeconds": 1},
                    "ranges": {"includeServer": True, "includeRequests": True, "beforeMs": 0, "afterMs": 0},
                    "metrics": [
                        {"id": "rss", "query": "polygraph_process_resident_memory_bytes", "label": "RSS", "unit": "bytes", "panel": "Memory", "required": True},
                        {"id": "http_request_rate", "query": "sum(rate(polygraph_http_requests_total[30s]))", "label": "HTTP request rate", "unit": "req/s", "panel": "HTTP workload", "defaultVisible": False}
                    ],
                    "visualization": {"eventOverlays": ["full_bfs", "neo4j"]},
                }
            ),
            encoding="utf-8",
        )
        cfg = load_config(config_path)
        intervals = discover_intervals(tmp_path, cfg)
        assert len(intervals) == 4
        assert {i.kind for i in intervals} == {"request", "server"}
        request = next(i for i in intervals if i.run_type == "01-gpu" and i.kind == "request")
        # Request range comes from index.json, not the narrower event-file min/max.
        assert request.start_unix_ns == 1_699_999_999_950_000_000
        assert request.end_unix_ns == 1_700_000_002_050_000_000

        manifest = export_metrics(tmp_path, config_path, tmp_path / "export")
        raw = json.loads(manifest.read_text())
        assert raw["intervalCount"] == 4
        assert raw["queryCount"] == 8
        assert raw["sampleCount"] == 24
        index = visualize_metrics(manifest, config_path=config_path)
        assert index.is_file()
        pages = list((index.parent / "intervals").rglob("*.html"))
        assert len(pages) == 4
        page_html = pages[0].read_text(encoding="utf-8")
        assert "Visible metrics" in page_html
        assert "data-metric-toggle" in page_html
        assert 'input.dataset.defaultVisible = metric.defaultVisible' in page_html
        assert 'defaultVisible":false' in page_html
        assert 'instance=test' not in page_html
        assert "KiB" in page_html or "bytes" in page_html

        samples = load_samples(tmp_path)
        assert len(samples) == 2
        assert {sample.run_type for sample in samples} == {"01-gpu", "08-no-cache"}
        compare = write_compare_dashboard(tmp_path, tmp_path / "compare.html")
        assert compare.is_file()
        assert "Compare run configurations" in compare.read_text(encoding="utf-8")

        # Direct events-directory mode is what --split-by-run-type uses.
        one_events = tmp_path / "01-gpu" / "events"
        direct = discover_intervals(one_events, cfg)
        assert {i.kind for i in direct} == {"request", "server"}
        direct_samples = load_samples(one_events, include_server=True)
        assert {sample.source_kind for sample in direct_samples} == {"request", "server"}
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
