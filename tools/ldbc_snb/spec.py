from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class NodeSpec:
    file_prefix: str
    type_name: str
    properties: tuple[tuple[str, str], ...]


@dataclass(frozen=True)
class EdgeSpec:
    file_prefix: str
    type_name: str
    from_type: str
    to_type: str
    properties: tuple[tuple[str, str], ...] = ()
    directed: bool = True


NODE_SPECS: dict[str, NodeSpec] = {
    "person": NodeSpec("person", "Person", (
        ("ldbc_id", "integer"), ("firstName", "string"), ("lastName", "string"),
        ("gender", "string"), ("birthday", "date"), ("creationDate", "datetime"),
        ("locationIP", "string"), ("browserUsed", "string"), ("language", "string"),
        ("email", "string"),
    )),
    "forum": NodeSpec("forum", "Forum", (("ldbc_id", "integer"), ("title", "string"), ("creationDate", "datetime"))),
    "post": NodeSpec("post", "Post", (
        ("ldbc_id", "integer"), ("imageFile", "string"), ("creationDate", "datetime"),
        ("locationIP", "string"), ("browserUsed", "string"), ("language", "string"),
        ("content", "string"), ("length", "integer"),
    )),
    "comment": NodeSpec("comment", "Comment", (
        ("ldbc_id", "integer"), ("creationDate", "datetime"), ("locationIP", "string"),
        ("browserUsed", "string"), ("content", "string"), ("length", "integer"),
    )),
    "organisation": NodeSpec("organisation", "Organisation", (
        ("ldbc_id", "integer"), ("type", "string"), ("name", "string"), ("url", "string"),
    )),
    "place": NodeSpec("place", "Place", (
        ("ldbc_id", "integer"), ("name", "string"), ("url", "string"), ("type", "string"),
    )),
    "tag": NodeSpec("tag", "Tag", (("ldbc_id", "integer"), ("name", "string"), ("url", "string"))),
    "tagclass": NodeSpec("tagclass", "TagClass", (("ldbc_id", "integer"), ("name", "string"), ("url", "string"))),
}


def edge(
    prefix: str,
    from_type: str,
    to_type: str,
    *properties: tuple[str, str],
    directed: bool = True,
) -> EdgeSpec:
    return EdgeSpec(prefix, prefix.upper(), from_type, to_type, tuple(properties), directed)


EDGE_SPECS: dict[str, EdgeSpec] = {
    "comment_hasCreator_person": edge("comment_hasCreator_person", "Comment", "Person"),
    "comment_hasTag_tag": edge("comment_hasTag_tag", "Comment", "Tag"),
    "comment_isLocatedIn_place": edge("comment_isLocatedIn_place", "Comment", "Place"),
    "comment_replyOf_comment": edge("comment_replyOf_comment", "Comment", "Comment"),
    "comment_replyOf_post": edge("comment_replyOf_post", "Comment", "Post"),
    "forum_containerOf_post": edge("forum_containerOf_post", "Forum", "Post"),
    "forum_hasMember_person": edge("forum_hasMember_person", "Forum", "Person", ("joinDate", "datetime")),
    "forum_hasModerator_person": edge("forum_hasModerator_person", "Forum", "Person"),
    "forum_hasTag_tag": edge("forum_hasTag_tag", "Forum", "Tag"),
    "organisation_isLocatedIn_place": edge("organisation_isLocatedIn_place", "Organisation", "Place"),
    "person_hasInterest_tag": edge("person_hasInterest_tag", "Person", "Tag"),
    "person_isLocatedIn_place": edge("person_isLocatedIn_place", "Person", "Place"),
    "person_knows_person": edge(
        "person_knows_person", "Person", "Person", ("creationDate", "datetime"), directed=False
    ),
    "person_likes_comment": edge("person_likes_comment", "Person", "Comment", ("creationDate", "datetime")),
    "person_likes_post": edge("person_likes_post", "Person", "Post", ("creationDate", "datetime")),
    "person_studyAt_organisation": edge("person_studyAt_organisation", "Person", "Organisation", ("classYear", "integer")),
    "person_workAt_organisation": edge("person_workAt_organisation", "Person", "Organisation", ("workFrom", "integer")),
    "post_hasCreator_person": edge("post_hasCreator_person", "Post", "Person"),
    "post_hasTag_tag": edge("post_hasTag_tag", "Post", "Tag"),
    "post_isLocatedIn_place": edge("post_isLocatedIn_place", "Post", "Place"),
    "tag_hasType_tagclass": edge("tag_hasType_tagclass", "Tag", "TagClass"),
    "tagclass_isSubclassOf_tagclass": edge("tagclass_isSubclassOf_tagclass", "TagClass", "TagClass"),
    "place_isPartOf_place": edge("place_isPartOf_place", "Place", "Place"),
}

AUXILIARY_PREFIXES = {
    "person_email_emailaddress": "email",
    "person_speaks_language": "language",
}
