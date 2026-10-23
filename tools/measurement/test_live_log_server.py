import json
import time
from pathlib import Path

from tools.measurement.live_log_server import (
    LiveReconstructor,
    LiveServerContext,
    SSEBroadcaster,
)


def event(name: str, seq: int, elapsed_ns: int, **extra):
    return {
        "event": name,
        "sequence": seq,
        "elapsed_ns": elapsed_ns,
        "thread_id": extra.pop("thread_id", 77),
        **extra,
    }


def make_context(tmp_path: Path) -> LiveServerContext:
    return LiveServerContext(
        broadcaster=SSEBroadcaster(),
        watch_root=tmp_path / "logs",
        output_dir=tmp_path / "output",
        output_name="dashboard.html",
        inline_plotly=False,
        poll_interval=0.01,
    )


def append_event(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(value) + "\n")
        handle.flush()


def wait_until(predicate, timeout: float = 2.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("condition was not reached before timeout")


def test_live_reconstructor_opens_grows_and_completes_span(tmp_path: Path):
    source = tmp_path / "run.jsonl"
    state = LiveReconstructor(source)

    opened = state.process_event(
        event("neo4j.query.start", 1, 1_000_000, backend="neo4j", depth=3),
        line_number=1,
    )
    snapshot = state.snapshot()

    assert opened[0]["type"] == "span_opened"
    assert len(snapshot.open_spans) == 1
    assert snapshot.open_spans[0]["running"] is True
    assert snapshot.open_spans[0]["lane"] == 0
    assert snapshot.open_spans[0]["attributes"]["backend"] == "neo4j"

    completed = state.process_event(
        event("neo4j.query.end", 2, 8_000_000, rows=12, status="success"),
        line_number=2,
    )
    snapshot = state.snapshot()

    assert completed[0]["type"] == "span_completed"
    assert not snapshot.open_spans
    assert len(snapshot.completed_spans) == 1
    span = snapshot.completed_spans[0]
    assert span["durationNs"] == 7_000_000
    assert span["attributes"] == {
        "backend": "neo4j",
        "depth": 3,
        "rows": 12,
        "status": "success",
    }


def test_nested_spans_receive_separate_lanes_and_lanes_are_reused(tmp_path: Path):
    state = LiveReconstructor(tmp_path / "nested.jsonl")
    state.process_event(event("work.start", 1, 10), line_number=1)
    state.process_event(event("work.start", 2, 20), line_number=2)

    lanes = sorted(span["lane"] for span in state.snapshot().open_spans)
    assert lanes == [0, 1]

    state.process_event(event("work.end", 3, 30), line_number=3)
    state.process_event(event("other.start", 4, 40), line_number=4)
    new_span = next(
        span for span in state.snapshot().open_spans if span["name"] == "other"
    )
    assert new_span["lane"] == 1


def test_server_can_start_without_a_log_and_attach_future_file(tmp_path: Path):
    context = make_context(tmp_path)
    context.watch_root.mkdir(parents=True)

    assert context.snapshot_message()["attached"] is False
    result = context.attach("run-a/1.jsonl")
    assert result["waitingForFile"] is True
    assert context.snapshot_message()["attached"] is True

    source = context.watch_root / "run-a" / "1.jsonl"
    append_event(source, event("gpu.expand.start", 1, 1_000_000, backend="gpu"))
    wait_until(lambda: len(context.state.snapshot().open_spans) == 1)
    append_event(source, event("gpu.expand.end", 2, 5_000_000, result_count=9))
    wait_until(lambda: len(context.state.snapshot().completed_spans) == 1)

    context.shutdown()


def test_switching_logs_exports_previous_run_and_uses_unique_output(tmp_path: Path):
    context = make_context(tmp_path)
    context.watch_root.mkdir(parents=True)

    context.attach("run-a/1.jsonl")
    source_a = context.watch_root / "run-a" / "1.jsonl"
    append_event(source_a, event("gpu.expand.start", 1, 1_000_000))
    append_event(source_a, event("gpu.expand.end", 2, 5_000_000))
    wait_until(lambda: len(context.state.snapshot().completed_spans) == 1)

    result = context.attach("run-b/2.jsonl")
    assert result["previousExport"] is not None
    previous_html = Path(result["previousExport"]["path"])
    assert previous_html.is_file()
    assert previous_html == tmp_path / "output" / "run-a" / "1" / "dashboard.html"
    assert context.output_html == tmp_path / "output" / "run-b" / "2" / "dashboard.html"
    assert context.snapshot_message()["sourceRelativePath"] == "run-b/2.jsonl"

    context.shutdown()


def test_static_export_uses_existing_dashboard_generator(tmp_path: Path):
    context = make_context(tmp_path)
    context.watch_root.mkdir(parents=True)
    context.attach("run.jsonl")
    source = context.watch_root / "run.jsonl"
    append_event(source, event("gpu.expand.start", 1, 1_000_000, backend="gpu"))
    append_event(source, event("gpu.expand.end", 2, 5_000_000, result_count=9))
    wait_until(lambda: len(context.state.snapshot().completed_spans) == 1)

    result = context.export_static()
    output_html = tmp_path / "output" / "run" / "dashboard.html"

    assert result["ok"] is True
    assert result["completedSpans"] == 1
    assert output_html.is_file()
    page = output_html.read_text(encoding="utf-8")
    assert "Overview" in page
    assert "Timeline: by thread" in page
    assert "gpu.expand" in page
    assert (output_html.parent / "spans" / "run.spans.csv").is_file()

    context.shutdown()


def test_file_listing_is_scoped_to_watch_directory(tmp_path: Path):
    context = make_context(tmp_path)
    inside = context.watch_root / "nested" / "run.jsonl"
    append_event(inside, event("work.start", 1, 1))
    outside = tmp_path / "outside.jsonl"
    append_event(outside, event("work.start", 1, 1))

    files = context.list_log_files()
    assert [item["path"] for item in files] == ["nested/run.jsonl"]

    try:
        context.attach(outside)
    except ValueError as exc:
        assert "inside the watch directory" in str(exc)
    else:
        raise AssertionError("outside path should have been rejected")
