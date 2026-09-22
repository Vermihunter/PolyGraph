from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.measurement.log_reconstruction import (
    VIEW_OPTIONS,
    attribute_filter_token,
    build_comparison_metadata,
    build_dashboard,
    build_run_overview_html,
    build_timeline_filter_metadata,
    build_timeline_figure,
    build_timeline_thread_view,
    load_jsonl,
    make_hover_text,
    make_lane_by_thread,
    pair_spans,
)


class LogReconstructionTimelineFilterTests(unittest.TestCase):
    def _make_spans(self):
        events = [
            {
                "event": "neo4j.query.start",
                "seq": 1,
                "steady_ns": 1_000,
                "thread_id": 1,
                "native": True,
            },
            {
                "event": "neo4j.query.end",
                "seq": 2,
                "steady_ns": 2_000,
                "thread_id": 1,
            },
            {
                "event": "neo4j.parse.start",
                "seq": 3,
                "steady_ns": 2_100,
                "thread_id": 1,
                "native": False,
            },
            {
                "event": "neo4j.parse.end",
                "seq": 4,
                "steady_ns": 2_900,
                "thread_id": 1,
            },
            {
                "event": "arango.query.start",
                "seq": 5,
                "steady_ns": 3_000,
                "thread_id": 2,
                "backend": "ArangoDB",
            },
            {
                "event": "arango.query.end",
                "seq": 6,
                "steady_ns": 4_500,
                "thread_id": 2,
            },
        ]
        temporary = tempfile.TemporaryDirectory()
        path = Path(temporary.name) / "events.jsonl"
        path.write_text(
            "".join(json.dumps(event) + "\n" for event in events),
            encoding="utf-8",
        )
        spans = pair_spans(load_jsonl(path))
        return temporary, spans

    def test_metadata_contains_prefixes_and_boolean_missing_categories(self) -> None:
        temporary, spans = self._make_spans()
        self.addCleanup(temporary.cleanup)

        metadata = build_timeline_filter_metadata(spans)
        self.assertEqual(
            metadata["prefixes"],
            [{"name": "arango", "count": 1}, {"name": "neo4j", "count": 2}],
        )

        native = metadata["attributes"]["native"]
        self.assertEqual(native["missingCount"], 1)
        tokens = {entry["token"] for entry in native["values"]}
        self.assertEqual(
            tokens,
            {attribute_filter_token(True), attribute_filter_token(False)},
        )
        labels = {entry["label"] for entry in native["values"]}
        self.assertEqual(labels, {"True", "False"})

        events = {entry["name"]: entry for entry in metadata["events"]}
        self.assertEqual(events["neo4j.query"]["count"], 1)
        self.assertAlmostEqual(events["neo4j.query"]["minDurationMs"], 0.001)
        self.assertAlmostEqual(events["neo4j.query"]["maxDurationMs"], 0.001)
        self.assertAlmostEqual(events["arango.query"]["maxDurationMs"], 0.0015)

    def test_thread_view_renders_prefix_and_attribute_controls(self) -> None:
        temporary, spans = self._make_spans()
        self.addCleanup(temporary.cleanup)

        rendered = build_timeline_thread_view({
            "safe_id": "test_run",
            "label": "Test run",
            "spans": spans,
        })

        self.assertIn('data-timeline-prefix value="neo4j"', rendered)
        self.assertIn('data-timeline-prefix value="arango"', rendered)
        self.assertIn('<option value="native">native</option>', rendered)
        self.assertIn('data-add-attribute-filter', rendered)
        self.assertIn('data-active-attribute-filters', rendered)
        self.assertIn('multiple attribute cards combine with AND', rendered)
        self.assertIn('data-duration-event-select', rendered)
        self.assertIn('data-add-duration-filter', rendered)
        self.assertIn('<option value="neo4j.query"', rendered)
        self.assertIn('Event duration ranges', rendered)
        self.assertIn('"missingCount":1', rendered)
        self.assertIn('boolean:true', rendered)
        self.assertIn('boolean:false', rendered)

    def test_dashboard_javascript_supports_multiple_attribute_cards(self) -> None:
        source = Path(__file__).with_name("log_reconstruction.py").read_text(encoding="utf-8")
        self.assertIn("function addTimelineAttributeFilter(panel, attribute)", source)
        self.assertIn("function collectTimelineAttributeFilters(panel)", source)
        self.assertIn('querySelectorAll("[data-attribute-filter-card]")', source)
        self.assertIn("attributeFilters.every(filter =>", source)
        self.assertIn('button.dataset.attributeAction = action', source)
        self.assertIn('card.dataset.attributeName = attribute', source)

    def test_dashboard_javascript_filters_duration_and_removes_empty_lanes(self) -> None:
        source = Path(__file__).with_name("log_reconstruction.py").read_text(encoding="utf-8")
        self.assertIn("function addTimelineDurationFilter(panel, eventName)", source)
        self.assertIn("function collectTimelineDurationFilters(panel)", source)
        self.assertIn("durationMs >= durationFilter.minimum", source)
        self.assertIn("durationMs <= durationFilter.maximum", source)
        self.assertIn("const visibleLanes = new Set();", source)
        self.assertIn("const visibleLaneOrder = state.originalLaneOrder.filter", source)
        self.assertIn("categoryarray: visibleLaneOrder", source)
        self.assertIn("80 + 42 * visibleLaneOrder.length", source)

    def test_threads_receive_stable_zero_based_display_indices(self) -> None:
        temporary, spans = self._make_spans()
        self.addCleanup(temporary.cleanup)

        self.assertEqual(spans["thread_index"].tolist(), [0, 0, 1])
        self.assertEqual(
            make_lane_by_thread({**spans.iloc[0].to_dict(), "overlap_lane": 0}),
            "thread 0 / lane 0",
        )
        hover = make_hover_text({**spans.iloc[2].to_dict(), "overlap_lane": 0})
        self.assertIn("thread: 1", hover)
        self.assertNotIn("thread: 2", hover)

    def test_dashboard_javascript_supports_plotly_typed_arrays(self) -> None:
        source = Path(__file__).with_name("log_reconstruction.py").read_text(encoding="utf-8")
        self.assertIn("ArrayBuffer.isView(value)", source)
        self.assertIn("timelineTracePointCount(trace)", source)
        self.assertNotIn("Array.isArray(trace.x) ? trace.x.length : 0", source)

    def test_dashboard_exposes_comparison_workspace(self) -> None:
        self.assertEqual(
            VIEW_OPTIONS,
            [
                ("overview", "Overview"),
                ("timeline_thread", "Timeline: by thread"),
                ("comparison", "Compare runs"),
            ],
        )

    def test_dashboard_renders_comparison_workspace_and_average_index_link(self) -> None:
        temporary, spans = self._make_spans()
        self.addCleanup(temporary.cleanup)

        with tempfile.TemporaryDirectory() as output_directory:
            output_root = Path(output_directory)
            average_index = output_root / "average_index.html"
            average_index.write_text("<html>average</html>", encoding="utf-8")
            dashboard = output_root / "dashboard.html"
            build_dashboard(
                runs=[{
                    "safe_id": "run_one",
                    "label": "Run one",
                    "relative_path": Path("logs/1.jsonl"),
                    "events": load_jsonl(Path(temporary.name) / "events.jsonl"),
                    "spans": spans,
                }],
                output_html=dashboard,
                average_index=average_index,
            )
            rendered = dashboard.read_text(encoding="utf-8")

        self.assertIn('value="overview">Overview</option>', rendered)
        self.assertIn('value="timeline_thread">Timeline: by thread</option>', rendered)
        self.assertIn('value="comparison">Compare runs</option>', rendered)
        self.assertIn('data-comparison-chart="boxplot"', rendered)
        self.assertIn('data-comparison-chart="families"', rendered)
        self.assertIn('data-comparison-chart="events"', rendered)
        self.assertIn('data-comparison-group', rendered)
        self.assertIn('data-comparison-source', rendered)
        self.assertIn('data-comparison-primary-metric', rendered)
        self.assertIn('server.events.jsonl', rendered)
        self.assertIn('data-comparison-family', rendered)
        self.assertIn('data-comparison-event', rendered)
        self.assertNotIn('Timeline: by thread + event', rendered)
        self.assertNotIn('Summary: event type', rendered)
        self.assertNotIn('Average: timing profile', rendered)
        self.assertIn('href="average_index.html"', rendered)
        self.assertIn('Open average report index', rendered)



    def test_comparison_groups_request_samples_by_run_configuration_and_excludes_server_by_default(self) -> None:
        temporary, spans = self._make_spans()
        self.addCleanup(temporary.cleanup)

        runs = [
            {
                "safe_id": "request_one",
                "label": "08-no-cache/events/requests/r1.events.jsonl",
                "relative_path": Path("08-no-cache/events/requests/r1.events.jsonl"),
                "spans": spans,
                "is_average": False,
            },
            {
                "safe_id": "request_two",
                "label": "08-no-cache/events/requests/r2.events.jsonl",
                "relative_path": Path("08-no-cache/events/requests/r2.events.jsonl"),
                "spans": spans,
                "is_average": False,
            },
            {
                "safe_id": "server",
                "label": "08-no-cache/events/server.events.jsonl",
                "relative_path": Path("08-no-cache/events/server.events.jsonl"),
                "spans": spans,
                "is_average": False,
            },
        ]
        metadata = build_comparison_metadata(runs)
        self.assertEqual([group["name"] for group in metadata["runGroups"]], ["08-no-cache"])
        self.assertEqual(metadata["runGroups"][0]["defaultSampleCount"], 2)
        sources = {source["name"]: source for source in metadata["sources"]}
        self.assertTrue(sources["request"]["defaultEnabled"])
        self.assertFalse(sources["server"]["defaultEnabled"])
        self.assertEqual(metadata["runs"][0]["group"], "08-no-cache")
        self.assertEqual(metadata["runs"][2]["sourceKind"], "server")

    def test_comparison_family_duration_uses_interval_union(self) -> None:
        temporary, spans = self._make_spans()
        self.addCleanup(temporary.cleanup)

        run = {
            "safe_id": "run_one",
            "label": "Run one",
            "spans": spans,
            "is_average": False,
        }
        metadata = build_comparison_metadata([run])
        run_metrics = metadata["runs"][0]

        # neo4j.query is [0, 0.001] ms and neo4j.parse is [0.0011, 0.0019] ms,
        # so the family wall-clock coverage is their union rather than an arbitrary
        # nested/instrumentation sum.
        self.assertAlmostEqual(run_metrics["groups"]["neo4j"], 0.0018)
        self.assertAlmostEqual(run_metrics["events"]["neo4j.query"], 0.001)

    def test_overview_contains_all_event_rows_but_defaults_to_top_ten(self) -> None:
        temporary, spans = self._make_spans()
        self.addCleanup(temporary.cleanup)
        run = {
            "relative_path": Path("logs/1.jsonl"),
            "events": load_jsonl(Path(temporary.name) / "events.jsonl"),
            "spans": spans,
        }
        rendered = build_run_overview_html(run)
        self.assertIn('data-overview-expanded="false"', rendered)
        self.assertIn('data-overview-toggle', rendered)
        self.assertIn('data-overview-search', rendered)
        self.assertIn('data-overview-event-row', rendered)

    def test_timeline_points_carry_attribute_json_for_browser_filtering(self) -> None:
        temporary, spans = self._make_spans()
        self.addCleanup(temporary.cleanup)

        figure = build_timeline_figure(
            spans,
            title="test",
            group_cols=("thread_id",),
            lane_fn=make_lane_by_thread,
        )
        neo4j_query = next(trace for trace in figure.data if trace.name == "neo4j.query")
        self.assertEqual(len(neo4j_query.customdata[0]), 2)
        attributes = json.loads(neo4j_query.customdata[0][1])
        self.assertIs(attributes["native"], True)


if __name__ == "__main__":
    unittest.main()
