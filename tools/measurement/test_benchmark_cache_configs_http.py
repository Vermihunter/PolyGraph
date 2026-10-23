from __future__ import annotations

import json
from pathlib import Path

import pytest

from tools.measurement.benchmark_cache_configs import (
    _http_bfs_payload,
    _http_server_config,
    _load_http_request_plan,
    _post_bfs,
    _redirect_observability,
    _split_server_events_by_request,
)


def test_http_bfs_payload_maps_native_cli_options() -> None:
    payload = _http_bfs_payload(
        [
            "--no-predecessors",
            "--allow-native-pushdown",
            "--neo4j-max-concurrent-requests",
            "16",
            "--arango-max-concurrent-requests",
            "12",
            "--arango-request-strategy",
            "single",
            "--arango-single-request-max-frontier",
            "250000",
            "--streaming-candidate-batch-size",
            "16384",
            "--redis-pipeline-batch-size",
            "20000",
            "--backend-request-retries",
            "3",
            "--backend-retry-delay-ms",
            "25",
            "--allowed-edge-types",
            "KNOWS,LIKES",
            "--thread-count",
            "24",
        ],
        max_depth=10,
        direction="both",
        thread_count=8,
        neighbor_batch_size=10000,
        response_mode="summary",
    )

    assert payload == {
        "maxDepth": 10,
        "direction": "both",
        "responseMode": "summary",
        "allowedEdgeTypes": ["KNOWS", "LIKES"],
        "allowNativePushdown": True,
        "returnPredecessors": False,
        "neighborBatchSize": 10000,
        "neo4jMaxConcurrentRequests": 16,
        "arangoMaxConcurrentRequests": 12,
        "arangoRequestStrategy": "single",
        "arangoSingleRequestMaxFrontier": 250000,
        "streamingCandidateBatchSize": 16384,
        "redisPipelineBatchSize": 20000,
        "backendRequestRetries": 3,
        "backendRetryDelayMs": 25,
        "threadCount": 24,
    }


def test_http_bfs_payload_rejects_binary_only_arguments() -> None:
    with pytest.raises(ValueError, match="cannot be represented"):
        _http_bfs_payload(
            ["--output-file", "x.csv"],
            max_depth=10,
            direction="both",
            thread_count=None,
            neighbor_batch_size=None,
            response_mode="summary",
        )


def test_generated_http_server_config_overrides_domain_cache_and_observability(tmp_path: Path) -> None:
    base = tmp_path / "server.json"
    base.write_text(
        json.dumps(
            {
                "listeners": [{"address": "127.0.0.1", "port": 1234}],
                "plugins": [
                    {
                        "name": "polygraph::services::ServicesPlugin",
                        "dependencies": [],
                        "config": {"domain": "old.json"},
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    domain = tmp_path / "domain.json"
    cache_plan = tmp_path / "cache-plan.json"
    observability = tmp_path / "observability.json"
    for path in (domain, cache_plan, observability):
        path.write_text("{}", encoding="utf-8")

    output = _http_server_config(
        base,
        domain=domain,
        cache_plan=cache_plan,
        observability=observability,
        host="0.0.0.0",
        port=8080,
        output=tmp_path / "generated.json",
    )
    generated = json.loads(output.read_text(encoding="utf-8"))
    plugin = generated["plugins"][0]["config"]
    assert generated["listeners"] == [{"address": "0.0.0.0", "port": 8080, "https": False}]
    assert plugin["domain"] == str(domain.resolve())
    assert plugin["cache_plan"] == str(cache_plan.resolve())
    assert plugin["observability_config"] == str(observability.resolve())


def test_observability_redirect_keeps_stdout_and_isolates_files(tmp_path: Path) -> None:
    source = tmp_path / "observability-source.json"
    source.write_text(
        json.dumps(
            {
                "events": {
                    "enabled": True,
                    "sinks": [{"name": "events", "type": "rotating_file", "path": "old.events.jsonl"}],
                },
                "logging": {
                    "enabled": True,
                    "sinks": [
                        {"name": "console", "type": "stdout", "enabled": True},
                        {"name": "file", "type": "rotating_file", "path": "old.log"},
                    ],
                },
                "metrics": {"enabled": True},
            }
        ),
        encoding="utf-8",
    )

    generated_path = _redirect_observability(source, tmp_path / "run")
    generated = json.loads(generated_path.read_text(encoding="utf-8"))
    assert generated["logging"]["sinks"][0]["type"] == "stdout"
    assert generated["logging"]["sinks"][1]["path"].endswith("run/logs/server.log")
    assert generated["logging"]["sinks"][1]["type"] == "basic_file"
    assert generated["events"]["sinks"][0]["path"].endswith("run/events/server.events.jsonl")
    assert generated["events"]["sinks"][0]["format"] == "json"


def test_polygraph_benchmark_caches_accepts_options_after_domain() -> None:
    from tools import polygraph

    parser = polygraph.build_parser()
    args = parser.parse_args(
        [
            "benchmark-caches",
            "data/domain.json",
            "--transport",
            "http",
            "--cache-configs",
            "config/cache/a.json",
            "config/cache/b.json",
            "--start-uuids-file",
            "starts.txt",
            "--repetitions",
            "5",
            "--output-dir",
            "measurements/x",
            "--overwrite",
        ]
    )
    assert args.domain == "data/domain.json"
    assert args.cache_configs == ["config/cache/a.json", "config/cache/b.json"]
    assert args.transport == "http"


def test_polygraph_benchmark_caches_accepts_existing_cache_plans() -> None:
    from tools import polygraph

    parser = polygraph.build_parser()
    args = parser.parse_args(
        [
            "benchmark-caches",
            "data/domain.json",
            "--transport",
            "http",
            "--cache-plans",
            "data/caches/a/cache-plan.json",
            "data/caches/b/cache-plan.json",
            "--start-uuids-file",
            "starts.txt",
            "--repetitions",
            "5",
        ]
    )
    assert args.cache_configs is None
    assert args.cache_plans == [
        "data/caches/a/cache-plan.json",
        "data/caches/b/cache-plan.json",
    ]


def test_existing_cache_plan_is_validated_without_regeneration(tmp_path: Path) -> None:
    from tools.measurement.benchmark_cache_configs import _load_existing_cache_plan

    graph = tmp_path / "graph.jsonl"
    graph.write_text('{"record":"metadata"}\n', encoding="utf-8")
    cache_dir = tmp_path / "cache-a"
    cache_dir.mkdir()
    (cache_dir / "residency.bin").write_bytes(b"")
    mmap_dir = cache_dir / "mmap"
    mmap_dir.mkdir()
    (mmap_dir / "index.bin").write_bytes(b"")
    (mmap_dir / "neighbors.bin").write_bytes(b"")
    (mmap_dir / "manifest.json").write_text(
        json.dumps({"index": "index.bin", "neighbors": "neighbors.bin"}),
        encoding="utf-8",
    )
    gpu_dir = cache_dir / "gpu" / "gpu0"
    gpu_dir.mkdir(parents=True)
    for name in ("global_ids.bin", "sources.bin", "destinations.bin"):
        (gpu_dir / name).write_bytes(b"")
    (gpu_dir / "manifest.json").write_text(
        json.dumps(
            {
                "format": "polygraph.gpu-projection.dense-edgelist",
                "version": 1,
                "globalIds": "global_ids.bin",
                "sources": "sources.bin",
                "destinations": "destinations.bin",
            }
        ),
        encoding="utf-8",
    )

    plan_path = cache_dir / "cache-plan.json"
    plan_path.write_text(
        json.dumps(
            {
                "format": "polygraph.cache-plan",
                "version": 1,
                "graph": str(graph.resolve()),
                "artifacts": {
                    "residency": {"path": "residency.bin", "entries": 0},
                    "mmap": {"manifest": "mmap/manifest.json"},
                    "gpu": {"0": {"manifest": "gpu/gpu0/manifest.json"}},
                },
                "runtimeBackends": [],
            }
        ),
        encoding="utf-8",
    )

    loaded = _load_existing_cache_plan(plan_path, graph)
    assert loaded["plan"] == str(plan_path.resolve())


def test_http_bfs_zero_timeout_waits_without_client_deadline(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[dict[str, object]] = []

    class FakeResponse:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

        @staticmethod
        def read() -> bytes:
            return b'{"vertexCount":1}'

    def fake_urlopen(request, *args, **kwargs):
        calls.append({"args": args, "kwargs": kwargs})
        return FakeResponse()

    monkeypatch.setattr("urllib.request.urlopen", fake_urlopen)
    status, raw, body, _ = _post_bfs("http://127.0.0.1:8080/api/v1/graphs/1/bfs", {}, 0.0)

    assert status == 200
    assert raw == b'{"vertexCount":1}'
    assert body["vertexCount"] == 1
    assert calls == [{"args": (), "kwargs": {}}]


def test_polygraph_http_benchmark_timeouts_default_to_disabled() -> None:
    from tools import polygraph

    parser = polygraph.build_parser()
    args = parser.parse_args(
        [
            "benchmark-caches",
            "data/domain.json",
            "--transport",
            "http",
            "--cache-plans",
            "data/caches/a/cache-plan.json",
        ]
    )

    assert args.http_request_timeout == 0.0
    assert args.http_startup_timeout == 0.0


def test_http_request_plan_supports_relative_schedule_uuid_and_payload_overrides(tmp_path: Path) -> None:
    plan = tmp_path / "requests.json"
    plan.write_text(
        json.dumps(
            {
                "mode": "serial",
                "defaults": {
                    "startUuid": "$DOMAIN_START_UUID",
                    "payload": {"maxDepth": 4, "returnPredecessors": False},
                },
                "requests": [
                    {"id": "warmup", "atMs": 0, "measure": False},
                    {
                        "id": "measured-root-2",
                        "atMs": 1250,
                        "startUuid": "777",
                        "payload": {"maxDepth": 7},
                    },
                ],
            }
        ),
        encoding="utf-8",
    )

    requests = _load_http_request_plan(
        plan,
        domain_start_uuid="123",
        base_payload={"direction": "both", "responseMode": "summary"},
    )

    assert [(item.request_id, item.at_ms, item.start_uuid) for item in requests] == [
        ("warmup", 0.0, "123"),
        ("measured-root-2", 1250.0, "777"),
    ]
    assert requests[0].measure is False
    assert requests[1].measure is True
    assert requests[0].payload == {
        "direction": "both",
        "responseMode": "summary",
        "maxDepth": 4,
        "returnPredecessors": False,
    }
    assert requests[1].payload["maxDepth"] == 7


def test_http_request_plan_rejects_decreasing_schedule(tmp_path: Path) -> None:
    plan = tmp_path / "requests.json"
    plan.write_text(
        json.dumps(
            {
                "requests": [
                    {"id": "later", "atMs": 100, "startUuid": "1"},
                    {"id": "earlier", "atMs": 50, "startUuid": "2"},
                ]
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="non-decreasing"):
        _load_http_request_plan(plan, domain_start_uuid="1", base_payload={})


def test_server_events_are_split_into_one_jsonl_per_serial_http_request(tmp_path: Path) -> None:
    source = tmp_path / "server.events.jsonl"
    source.write_text(
        "".join(
            json.dumps({"timestamp_unix_ns": timestamp, "event": f"e-{timestamp}"}) + "\n"
            for timestamp in (90, 100, 150, 200, 250, 300, 310)
        ),
        encoding="utf-8",
    )
    intervals = [
        {"request_id": "r1", "started_unix_ns": 100, "finished_unix_ns": 200},
        {"request_id": "r2", "started_unix_ns": 250, "finished_unix_ns": 300},
    ]
    result = _split_server_events_by_request(source, tmp_path / "requests", intervals)

    r1 = [json.loads(line)["timestamp_unix_ns"] for line in Path(result["r1"]).read_text().splitlines()]
    r2 = [json.loads(line)["timestamp_unix_ns"] for line in Path(result["r2"]).read_text().splitlines()]
    assert r1 == [100, 150, 200]
    assert r2 == [250, 300]
    assert (tmp_path / "requests" / "index.json").is_file()


def test_polygraph_benchmark_caches_accepts_http_request_plan_and_instance_subset() -> None:
    from tools import polygraph

    parser = polygraph.build_parser()
    benchmark_args = parser.parse_args(
        [
            "benchmark-caches",
            "data/domain.json",
            "--transport",
            "http",
            "--cache-plans",
            "cache/cache-plan.json",
            "--http-request-plan",
            "requests.json",
        ]
    )
    assert benchmark_args.http_request_plan == "requests.json"

    instance_args = parser.parse_args(
        ["instance", "sf1_0", "schema-based", "up", "--services", "neo4j", "arangodb"]
    )
    assert instance_args.services == ["neo4j", "arangodb"]


def test_polygraph_load_exposes_arango_import_workers() -> None:
    from tools import polygraph

    parser = polygraph.build_parser()
    args = parser.parse_args(["load", "data/domain.json", "--workers", "12"])
    assert args.workers == 12
