from __future__ import annotations

import json
import math
import tempfile
import unittest
from pathlib import Path

from tools.ldbc_snb.statistics import analyze_dataset, summarize_degrees, write_report_outputs


class LdbcStatisticsTest(unittest.TestCase):
    def test_degree_summary_includes_zero_degree_nodes(self) -> None:
        from collections import Counter

        stats = summarize_degrees(Counter({"1": 2, "2": 1}), 4)
        self.assertEqual(stats.population, 4)
        self.assertEqual(stats.active, 2)
        self.assertEqual(stats.total, 3)
        self.assertEqual(stats.minimum, 0)
        self.assertEqual(stats.median, 0)
        self.assertEqual(stats.p95, 2)
        self.assertEqual(stats.maximum, 2)
        self.assertAlmostEqual(stats.mean, 0.75)
        self.assertAlmostEqual(stats.stddev, math.sqrt(0.6875))

    def test_raw_dataset_report(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            static = root / "static"
            dynamic = root / "dynamic"
            static.mkdir()
            dynamic.mkdir()

            (dynamic / "person_0_0.csv").write_text(
                "id|firstName|\n1|A|\n2|B|\n3|C|\n4|D|\n", encoding="utf-8"
            )
            (dynamic / "post_0_0.csv").write_text(
                "id|content|\n10|a|\n11|b|\n12|c|\n", encoding="utf-8"
            )
            (dynamic / "person_knows_person_0_0.csv").write_text(
                "Person.id|Person.id|creationDate|\n"
                "1|2|x|\n1|3|x|\n2|4|x|\n",
                encoding="utf-8",
            )
            (dynamic / "person_likes_post_0_0.csv").write_text(
                "Person.id|Post.id|creationDate|\n1|10|x|\n4|10|x|\n",
                encoding="utf-8",
            )
            (static / "person_email_emailaddress_0_0.csv").write_text(
                "Person.id|email|\n1|a@example.test|\n", encoding="utf-8"
            )

            report = analyze_dataset(root)
            self.assertEqual(report["summary"]["node_rows"], 7)
            self.assertEqual(report["summary"]["relationship_rows"], 5)
            self.assertEqual(report["summary"]["auxiliary_rows"], 1)

            relationships = {item["name"]: item for item in report["relationships"]}
            knows = relationships["person_knows_person"]
            self.assertEqual(knows["outgoing"]["population"], 4)
            self.assertEqual(knows["outgoing"]["active"], 2)
            self.assertEqual(knows["outgoing"]["total"], 3)
            self.assertEqual(knows["outgoing"]["minimum"], 0)
            self.assertEqual(knows["outgoing"]["maximum"], 2)
            self.assertEqual(knows["incoming"]["active"], 3)
            self.assertEqual(knows["incoming"]["maximum"], 1)
            self.assertEqual(knows["combined"]["population"], 4)
            self.assertEqual(knows["combined"]["active"], 4)
            self.assertEqual(knows["combined"]["total"], 6)
            self.assertEqual(knows["combined"]["maximum"], 2)

            likes = relationships["person_likes_post"]
            self.assertEqual(likes["incoming"]["population"], 3)
            self.assertEqual(likes["incoming"]["active"], 1)
            self.assertEqual(likes["incoming"]["total"], 2)
            self.assertEqual(likes["incoming"]["maximum"], 2)
            self.assertEqual(likes["combined"]["population"], 7)
            self.assertEqual(likes["combined"]["active"], 3)
            self.assertEqual(likes["combined"]["total"], 4)
            self.assertEqual(likes["combined"]["maximum"], 2)

    def test_stats_scan_generates_placement_artifacts_without_graph_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dynamic = root / "dynamic"
            static = root / "static"
            dynamic.mkdir()
            static.mkdir()

            (dynamic / "person_0_0.csv").write_text(
                "id|firstName|\n1|Alice|\n2|Bob|\n", encoding="utf-8"
            )
            (dynamic / "post_0_0.csv").write_text(
                "id|content|\n10|Post|\n", encoding="utf-8"
            )
            (dynamic / "tag_0_0.csv").write_text(
                "id|name|\n30|Graph|\n", encoding="utf-8"
            )
            (dynamic / "post_hasCreator_person_0_0.csv").write_text(
                "Post.id|Person.id|creationDate|\n10|1|2020-01-01|\n",
                encoding="utf-8",
            )
            (dynamic / "post_hasTag_tag_0_0.csv").write_text(
                "Post.id|Tag.id|creationDate|\n10|30|2020-01-01|\n",
                encoding="utf-8",
            )

            placement = root / "placement.config.json"
            placement.write_text(
                json.dumps(
                    {
                        "storageBackends": ["Neo4J", "ArangoDB", "GPU"],
                        "defaultNodeSource": "Neo4J",
                        "defaultRelationshipSource": "Neo4J",
                        "nodeSources": {"Post": "ArangoDB", "Tag": "GPU"},
                        "relationshipSources": {
                            "POST_HASCREATOR_PERSON": "ArangoDB",
                            "POST_HASTAG_TAG": "ArangoDB",
                        },
                    }
                ),
                encoding="utf-8",
            )

            report = analyze_dataset(root, placement_config=placement)
            placement_report = report["placement"]
            self.assertEqual(placement_report["summary"]["nodeCount"], 4)
            self.assertEqual(placement_report["summary"]["relationshipCount"], 2)
            self.assertEqual(
                placement_report["backends"]["Neo4J"]["nodeTypes"],
                {"Person": 2},
            )
            self.assertEqual(
                placement_report["backends"]["ArangoDB"]["nodeTypes"],
                {"Post": 1},
            )
            self.assertEqual(
                placement_report["backends"]["GPU"]["nodeTypes"],
                {"Tag": 1},
            )
            self.assertEqual(
                placement_report["backends"]["ArangoDB"]["relationshipTypes"],
                {
                    "POST_HASCREATOR_PERSON": 1,
                    "POST_HASTAG_TAG": 1,
                },
            )
            self.assertEqual(
                placement_report["summary"]["relationshipsWithRemoteEndpoint"],
                2,
            )

            output_dir = root / "statistics-output"
            outputs = write_report_outputs(
                report, output_dir, render_placement_graph=False
            )
            self.assertTrue((output_dir / "ldbc-statistics.json").is_file())
            self.assertTrue((output_dir / "placement-report.json").is_file())
            self.assertTrue((output_dir / "placement-report.txt").is_file())
            self.assertTrue((output_dir / "placement-graph.dot").is_file())
            self.assertFalse((output_dir / "graph.jsonl").exists())
            self.assertEqual(set(outputs["placement"]), {"json", "text", "dot"})


if __name__ == "__main__":
    unittest.main()
