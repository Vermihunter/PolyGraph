"""Independent SNB Interactive v1 CsvBasic file-family facts.

These are test-oracle declarations, not imports from the conversion or loader
under test.  The eight concrete CSV node types and the projected relations are
the SNB schema, with Place and Organisation subclasses stored in their parent
CSV family.  Reference: https://ldbcouncil.org/ldbc_snb_docs/
The pipe separator, static/dynamic split, and two numeric shard suffixes are
documented in https://github.com/ldbc/ldbc_snb_docs/blob/main/data.tex .
"""

NODE_TYPES = (
    "Comment", "Forum", "Organisation", "Person", "Place", "Post", "Tag", "TagClass",
)

# CSV family -> (initial-data directory, concrete vertex type).
NODE_FILES = {
    "comment": ("dynamic", "Comment"),
    "forum": ("dynamic", "Forum"),
    "organisation": ("static", "Organisation"),
    "person": ("dynamic", "Person"),
    "place": ("static", "Place"),
    "post": ("dynamic", "Post"),
    "tag": ("static", "Tag"),
    "tagclass": ("static", "TagClass"),
}

# CSV family -> (directory, full typed relation label, source type, target type).
EDGE_FILES = {
    "comment_hasCreator_person": ("dynamic", "COMMENT_HASCREATOR_PERSON", "Comment", "Person"),
    "comment_hasTag_tag": ("dynamic", "COMMENT_HASTAG_TAG", "Comment", "Tag"),
    "comment_isLocatedIn_place": ("dynamic", "COMMENT_ISLOCATEDIN_PLACE", "Comment", "Place"),
    "comment_replyOf_comment": ("dynamic", "COMMENT_REPLYOF_COMMENT", "Comment", "Comment"),
    "comment_replyOf_post": ("dynamic", "COMMENT_REPLYOF_POST", "Comment", "Post"),
    "forum_containerOf_post": ("dynamic", "FORUM_CONTAINEROF_POST", "Forum", "Post"),
    "forum_hasMember_person": ("dynamic", "FORUM_HASMEMBER_PERSON", "Forum", "Person"),
    "forum_hasModerator_person": ("dynamic", "FORUM_HASMODERATOR_PERSON", "Forum", "Person"),
    "forum_hasTag_tag": ("dynamic", "FORUM_HASTAG_TAG", "Forum", "Tag"),
    "organisation_isLocatedIn_place": ("static", "ORGANISATION_ISLOCATEDIN_PLACE", "Organisation", "Place"),
    "person_hasInterest_tag": ("dynamic", "PERSON_HASINTEREST_TAG", "Person", "Tag"),
    "person_isLocatedIn_place": ("dynamic", "PERSON_ISLOCATEDIN_PLACE", "Person", "Place"),
    "person_knows_person": ("dynamic", "PERSON_KNOWS_PERSON", "Person", "Person"),
    "person_likes_comment": ("dynamic", "PERSON_LIKES_COMMENT", "Person", "Comment"),
    "person_likes_post": ("dynamic", "PERSON_LIKES_POST", "Person", "Post"),
    "person_studyAt_organisation": ("dynamic", "PERSON_STUDYAT_ORGANISATION", "Person", "Organisation"),
    "person_workAt_organisation": ("dynamic", "PERSON_WORKAT_ORGANISATION", "Person", "Organisation"),
    "place_isPartOf_place": ("static", "PLACE_ISPARTOF_PLACE", "Place", "Place"),
    "post_hasCreator_person": ("dynamic", "POST_HASCREATOR_PERSON", "Post", "Person"),
    "post_hasTag_tag": ("dynamic", "POST_HASTAG_TAG", "Post", "Tag"),
    "post_isLocatedIn_place": ("dynamic", "POST_ISLOCATEDIN_PLACE", "Post", "Place"),
    "tag_hasType_tagclass": ("static", "TAG_HASTYPE_TAGCLASS", "Tag", "TagClass"),
    "tagclass_isSubclassOf_tagclass": ("static", "TAGCLASS_ISSUBCLASSOF_TAGCLASS", "TagClass", "TagClass"),
}

# These files contain multivalued Person properties, not graph edges.
AUX_FILES = {
    "person_email_emailaddress": ("dynamic", "email"),
    "person_speaks_language": ("dynamic", "language"),
}
