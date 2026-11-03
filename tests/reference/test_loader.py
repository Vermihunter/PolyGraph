"""Small, handwritten CSV fixtures for the independent test-only loader."""

from pathlib import Path
import tempfile
import unittest

from tests.reference.ldbc_csv import load_csv_graph
from tests.reference.model import Edge, Graph


class CsvLoaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "static").mkdir()
        (self.root / "dynamic").mkdir()

    def write(self, relative, contents):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    def people(self):
        self.write("dynamic/person_0_0.csv", "id|firstName\n1|Alice\n2|Bob\n")

    def test_raw_typed_ids_and_quoted_property_delimiters(self):
        self.write("dynamic/person_0_0.csv", 'id|firstName|\n0|"A|B"|\n18446744073709551615|C|\n')
        self.write("static/place_0_0.csv", "id|name\n0|Somewhere\n")
        self.write("dynamic/person_isLocatedIn_place_0_0.csv", "Person.id|Place.id|\n0|0|\n")
        graph = load_csv_graph(self.root)
        self.assertEqual(
            graph,
            Graph(
                frozenset({("Person", 0), ("Person", 18446744073709551615), ("Place", 0)}),
                (Edge(("Person", 0), ("Place", 0), "PERSON_ISLOCATEDIN_PLACE"),),
            ),
        )
        canonical = next(vertex for vertex in graph.vertices if vertex == ("Person", 0))
        self.assertIs(graph.edges[0].source, canonical)

    def test_recursive_shards_have_deterministic_order_and_keep_duplicate_edges(self):
        self.write("dynamic/z/person_9_0.csv", "id\n2\n")
        self.write("dynamic/a/person_1_0.csv", "id\n1\n")
        self.write("dynamic/z/person_knows_person_1_0.csv", "Person.id|Person.id\n2|1\n")
        self.write("dynamic/a/person_knows_person_9_0.csv", "Person.id|Person.id\n1|2\n1|2\n")
        self.assertEqual(
            load_csv_graph(self.root).edges,
            (
                Edge(("Person", 1), ("Person", 2), "PERSON_KNOWS_PERSON"),
                Edge(("Person", 1), ("Person", 2), "PERSON_KNOWS_PERSON"),
                Edge(("Person", 2), ("Person", 1), "PERSON_KNOWS_PERSON"),
            ),
        )

    def test_all_v1_relation_families_have_their_own_typed_endpoints(self):
        for relative in (
            "dynamic/comment_0_0.csv", "dynamic/forum_0_0.csv", "dynamic/person_0_0.csv",
            "dynamic/post_0_0.csv", "static/organisation_0_0.csv", "static/place_0_0.csv",
            "static/tag_0_0.csv", "static/tagclass_0_0.csv",
        ):
            self.write(relative, "id\n1\n")
        files = {
            "dynamic/comment_hasCreator_person_0_0.csv": "Comment.id|Person.id",
            "dynamic/comment_hasTag_tag_0_0.csv": "Comment.id|Tag.id",
            "dynamic/comment_isLocatedIn_place_0_0.csv": "Comment.id|Place.id",
            "dynamic/comment_replyOf_comment_0_0.csv": "Comment.id|Comment.id",
            "dynamic/comment_replyOf_post_0_0.csv": "Comment.id|Post.id",
            "dynamic/forum_containerOf_post_0_0.csv": "Forum.id|Post.id",
            "dynamic/forum_hasMember_person_0_0.csv": "Forum.id|Person.id|joinDate",
            "dynamic/forum_hasModerator_person_0_0.csv": "Forum.id|Person.id",
            "dynamic/forum_hasTag_tag_0_0.csv": "Forum.id|Tag.id",
            "dynamic/person_hasInterest_tag_0_0.csv": "Person.id|Tag.id",
            "dynamic/person_isLocatedIn_place_0_0.csv": "Person.id|Place.id",
            "dynamic/person_knows_person_0_0.csv": "Person.id|Person.id|creationDate",
            "dynamic/person_likes_comment_0_0.csv": "Person.id|Comment.id|creationDate",
            "dynamic/person_likes_post_0_0.csv": "Person.id|Post.id|creationDate",
            "dynamic/person_studyAt_organisation_0_0.csv": "Person.id|Organisation.id|classYear",
            "dynamic/person_workAt_organisation_0_0.csv": "Person.id|Organisation.id|workFrom",
            "dynamic/post_hasCreator_person_0_0.csv": "Post.id|Person.id",
            "dynamic/post_hasTag_tag_0_0.csv": "Post.id|Tag.id",
            "dynamic/post_isLocatedIn_place_0_0.csv": "Post.id|Place.id",
            "static/organisation_isLocatedIn_place_0_0.csv": "Organisation.id|Place.id",
            "static/place_isPartOf_place_0_0.csv": "Place.id|Place.id",
            "static/tag_hasType_tagclass_0_0.csv": "Tag.id|TagClass.id",
            "static/tagclass_isSubclassOf_tagclass_0_0.csv": "TagClass.id|TagClass.id",
        }
        for relative, header in files.items():
            self.write(relative, header + "\n" + ("1|1|2010\n" if header.count("|") == 2 else "1|1\n"))
        self.write("dynamic/person_email_emailaddress_0_0.csv", "Person.id|email\n1|a@example.org\n")
        self.write("dynamic/person_speaks_language_0_0.csv", "Person.id|language\n1|en\n")
        graph = load_csv_graph(self.root, require_complete=True)
        self.assertEqual(graph.vertices, frozenset({
            ("Comment", 1), ("Forum", 1), ("Person", 1), ("Post", 1),
            ("Organisation", 1), ("Place", 1), ("Tag", 1), ("TagClass", 1),
        }))
        self.assertEqual(graph.edges, (
            Edge(("Comment", 1), ("Person", 1), "COMMENT_HASCREATOR_PERSON"),
            Edge(("Comment", 1), ("Tag", 1), "COMMENT_HASTAG_TAG"),
            Edge(("Comment", 1), ("Place", 1), "COMMENT_ISLOCATEDIN_PLACE"),
            Edge(("Comment", 1), ("Comment", 1), "COMMENT_REPLYOF_COMMENT"),
            Edge(("Comment", 1), ("Post", 1), "COMMENT_REPLYOF_POST"),
            Edge(("Forum", 1), ("Post", 1), "FORUM_CONTAINEROF_POST"),
            Edge(("Forum", 1), ("Person", 1), "FORUM_HASMEMBER_PERSON"),
            Edge(("Forum", 1), ("Person", 1), "FORUM_HASMODERATOR_PERSON"),
            Edge(("Forum", 1), ("Tag", 1), "FORUM_HASTAG_TAG"),
            Edge(("Person", 1), ("Tag", 1), "PERSON_HASINTEREST_TAG"),
            Edge(("Person", 1), ("Place", 1), "PERSON_ISLOCATEDIN_PLACE"),
            Edge(("Person", 1), ("Person", 1), "PERSON_KNOWS_PERSON"),
            Edge(("Person", 1), ("Comment", 1), "PERSON_LIKES_COMMENT"),
            Edge(("Person", 1), ("Post", 1), "PERSON_LIKES_POST"),
            Edge(("Person", 1), ("Organisation", 1), "PERSON_STUDYAT_ORGANISATION"),
            Edge(("Person", 1), ("Organisation", 1), "PERSON_WORKAT_ORGANISATION"),
            Edge(("Post", 1), ("Person", 1), "POST_HASCREATOR_PERSON"),
            Edge(("Post", 1), ("Tag", 1), "POST_HASTAG_TAG"),
            Edge(("Post", 1), ("Place", 1), "POST_ISLOCATEDIN_PLACE"),
            Edge(("Organisation", 1), ("Place", 1), "ORGANISATION_ISLOCATEDIN_PLACE"),
            Edge(("Place", 1), ("Place", 1), "PLACE_ISPARTOF_PLACE"),
            Edge(("Tag", 1), ("TagClass", 1), "TAG_HASTYPE_TAGCLASS"),
            Edge(("TagClass", 1), ("TagClass", 1), "TAGCLASS_ISSUBCLASSOF_TAGCLASS"),
        ))
        for path in self.root.rglob("*.csv"):
            header = path.read_text(encoding="utf-8").splitlines()[0]
            path.write_text(header + "\n", encoding="utf-8")
        self.assertEqual(load_csv_graph(self.root, require_complete=True), Graph(frozenset(), ()))

    def test_auxiliary_properties_validate_person_without_creating_topology(self):
        self.people()
        self.write("dynamic/person_email_emailaddress_0_0.csv", "Person.id|email|\n1|a@example.org|\n1|b@example.org|\n")
        self.write("dynamic/person_speaks_language_0_0.csv", "Person.id|language\n2|en\n")
        self.assertEqual(load_csv_graph(self.root), Graph(frozenset({("Person", 1), ("Person", 2)}), ()))

    def test_duplicate_typed_node_is_rejected_across_shards(self):
        self.people()
        self.write("dynamic/person_1_0.csv", "id\n01\n")
        with self.assertRaises(ValueError):
            load_csv_graph(self.root)

    def test_ids_are_strict_unsigned_64_bit_ascii_decimal(self):
        for value in ("-1", "+1", " 1", "1 ", "1.0", "1e3", "", "١", "18446744073709551616"):
            with self.subTest(value=value):
                self.write("dynamic/person_0_0.csv", f"id|name\n{value}|Alice\n")
                with self.assertRaises(ValueError):
                    load_csv_graph(self.root)

    def test_missing_relation_endpoint_is_rejected_without_inventing_nodes(self):
        self.people()
        self.write("dynamic/person_likes_post_0_0.csv", "Person.id|Post.id\n1|9\n")
        with self.assertRaises(ValueError):
            load_csv_graph(self.root)

    def test_missing_auxiliary_person_endpoint_is_rejected(self):
        self.write("dynamic/person_speaks_language_0_0.csv", "Person.id|language\n9|en\n")
        with self.assertRaises(ValueError):
            load_csv_graph(self.root)

    def test_invalid_edge_endpoint_number_is_rejected(self):
        self.people()
        self.write("dynamic/person_knows_person_0_0.csv", "Person.id|Person.id\n1|+2\n")
        with self.assertRaises(ValueError):
            load_csv_graph(self.root)

    def test_wrong_positional_endpoint_headers_are_rejected(self):
        self.people()
        for header in ("Person.id|Post.id", "person.id|Person.id", "Person.id|id", "Person.id"):
            with self.subTest(header=header):
                self.write("dynamic/person_knows_person_0_0.csv", header + "\n")
                with self.assertRaises(ValueError):
                    load_csv_graph(self.root)

    def test_malformed_node_and_auxiliary_headers_are_rejected(self):
        for relative, header in (
            ("dynamic/person_0_0.csv", "Person.id|name"),
            ("dynamic/person_0_0.csv", "id||name"),
            ("dynamic/person_0_0.csv", "id|name||"),
            ("dynamic/person_0_0.csv", "id|name|name"),
            ("dynamic/person_0_0.csv", ""),
            ("dynamic/person_email_emailaddress_0_0.csv", "Person.id|language"),
            ("dynamic/person_speaks_language_0_0.csv", "id|language"),
        ):
            with self.subTest(relative=relative, header=header):
                path = self.root / relative
                self.write(relative, header + "\n")
                with self.assertRaises(ValueError):
                    load_csv_graph(self.root)
                path.unlink()

    def test_short_long_and_blank_rows_are_rejected(self):
        for row in ("1", "1|Alice|extra", "1|Alice||", ""):
            with self.subTest(row=row):
                self.write("dynamic/person_0_0.csv", "id|name\n" + row + "\n")
                with self.assertRaises(ValueError):
                    load_csv_graph(self.root)

    def test_optional_trailing_pipe_does_not_eat_a_real_empty_property(self):
        self.write("dynamic/person_0_0.csv", "id|name\n1|\n2||\n")
        self.assertEqual(load_csv_graph(self.root).vertices, frozenset({("Person", 1), ("Person", 2)}))

    def test_invalid_utf8_and_csv_are_rejected(self):
        path = self.root / "dynamic/person_0_0.csv"
        for content in (b"id|name\n1|\xff\n", b'id|name\n1|"unterminated\n', b'id|name\n1|"a"garbage\n'):
            with self.subTest(content=content):
                path.write_bytes(content)
                with self.assertRaises(ValueError):
                    load_csv_graph(self.root)

    def test_unknown_families_bad_case_bad_shard_names_and_wrong_area_rejected(self):
        for relative in (
            "dynamic/person_typo_tag_0_0.csv", "dynamic/Person_0_0.csv",
            "dynamic/person_0.csv", "dynamic/person_0_0.CSV", "static/person_0_0.csv",
            "dynamic/tag_0_0.csv", "person_0_0.csv", "other/person_0_0.csv",
        ):
            with self.subTest(relative=relative):
                self.write(relative, "id\n1\n")
                with self.assertRaises(ValueError):
                    load_csv_graph(self.root)
                (self.root / relative).unlink()

    def test_root_update_streams_and_crc_sidecars_are_excluded(self):
        self.people()
        (self.root / "updateStream_0_0_person.csv").write_bytes(b"not|initial|data\n\xff")
        self.write("dynamic/.person_0_0.csv.crc", "not CSV")
        self.write("dynamic/unknown.csv.crc", "not CSV")
        self.assertEqual(load_csv_graph(self.root).vertices, frozenset({("Person", 1), ("Person", 2)}))

    def test_update_streams_inside_initial_directories_are_not_silently_ignored(self):
        self.write("dynamic/updateStream_0_0_person.csv", "id\n1\n")
        with self.assertRaises(ValueError):
            load_csv_graph(self.root)

    def test_both_initial_directories_are_required(self):
        (self.root / "static").rmdir()
        with self.assertRaises(ValueError):
            load_csv_graph(self.root)

    def test_csv_symlinks_and_directory_symlinks_are_rejected(self):
        target = self.root / "source.txt"
        target.write_text("id\n1\n", encoding="utf-8")
        link = self.root / "dynamic/person_0_0.csv"
        link.symlink_to(target)
        with self.assertRaises(ValueError):
            load_csv_graph(self.root)
        link.unlink()
        (self.root / "dynamic/shard").symlink_to(self.root / "static", target_is_directory=True)
        with self.assertRaises(ValueError):
            load_csv_graph(self.root)

    def test_complete_flag_requires_all_33_families_even_without_rows(self):
        self.people()
        self.assertEqual(len(load_csv_graph(self.root).vertices), 2)
        with self.assertRaises(ValueError):
            load_csv_graph(self.root, require_complete=True)


if __name__ == "__main__":
    unittest.main()
