from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.measurement.log_reconstruction import (
    attribute_filter_token,
    build_timeline_filter_metadata,
    build_timeline_figure,
    build_timeline_thread_view,
    load_jsonl,
    make_hover_text,
    make_lane_by_thread,
    make_lane_by_thread_and_event,
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
        self.assertEqual(
            make_lane_by_thread_and_event({**spans.iloc[2].to_dict(), "overlap_lane": 0}),
            "thread 1 / arango.query / lane 0",
        )
        hover = make_hover_text({**spans.iloc[2].to_dict(), "overlap_lane": 0})
        self.assertIn("thread: 1", hover)
        self.assertNotIn("thread: 2", hover)

    def test_dashboard_javascript_supports_plotly_typed_arrays(self) -> None:
        source = Path(__file__).with_name("log_reconstruction.py").read_text(encoding="utf-8")
        self.assertIn("ArrayBuffer.isView(value)", source)
        self.assertIn("timelineTracePointCount(trace)", source)
        self.assertNotIn("Array.isArray(trace.x) ? trace.x.length : 0", source)

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
