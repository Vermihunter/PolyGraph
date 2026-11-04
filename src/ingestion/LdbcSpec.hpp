#ifndef POLYGRAPH_INGESTION_LDBC_SPEC_HPP
#define POLYGRAPH_INGESTION_LDBC_SPEC_HPP
#include <array>
#include <string_view>
namespace polygraph::ingestion::detail {
    struct NodeSpec {
        std::string_view prefix;
        std::string_view type;
    };
    struct EdgeSpec {
        std::string_view prefix;
        std::string_view type;
        std::string_view from_type;
        std::string_view to_type;
    };
    inline constexpr std::array<NodeSpec, 8> node_specs{{
            {"person", "Person"},
            {"forum", "Forum"},
            {"post", "Post"},
            {"comment", "Comment"},
            {"organisation", "Organisation"},
            {"place", "Place"},
            {"tag", "Tag"},
            {"tagclass", "TagClass"},
    }};
    inline constexpr std::array<EdgeSpec, 23> edge_specs{{
            {"comment_hasCreator_person", "COMMENT_HASCREATOR_PERSON", "Comment", "Person"},
            {"comment_hasTag_tag", "COMMENT_HASTAG_TAG", "Comment", "Tag"},
            {"comment_isLocatedIn_place", "COMMENT_ISLOCATEDIN_PLACE", "Comment", "Place"},
            {"comment_replyOf_comment", "COMMENT_REPLYOF_COMMENT", "Comment", "Comment"},
            {"comment_replyOf_post", "COMMENT_REPLYOF_POST", "Comment", "Post"},
            {"forum_containerOf_post", "FORUM_CONTAINEROF_POST", "Forum", "Post"},
            {"forum_hasMember_person", "FORUM_HASMEMBER_PERSON", "Forum", "Person"},
            {"forum_hasModerator_person", "FORUM_HASMODERATOR_PERSON", "Forum", "Person"},
            {"forum_hasTag_tag", "FORUM_HASTAG_TAG", "Forum", "Tag"},
            {"organisation_isLocatedIn_place", "ORGANISATION_ISLOCATEDIN_PLACE", "Organisation", "Place"},
            {"person_hasInterest_tag", "PERSON_HASINTEREST_TAG", "Person", "Tag"},
            {"person_isLocatedIn_place", "PERSON_ISLOCATEDIN_PLACE", "Person", "Place"},
            {"person_knows_person", "PERSON_KNOWS_PERSON", "Person", "Person"},
            {"person_likes_comment", "PERSON_LIKES_COMMENT", "Person", "Comment"},
            {"person_likes_post", "PERSON_LIKES_POST", "Person", "Post"},
            {"person_studyAt_organisation", "PERSON_STUDYAT_ORGANISATION", "Person", "Organisation"},
            {"person_workAt_organisation", "PERSON_WORKAT_ORGANISATION", "Person", "Organisation"},
            {"post_hasCreator_person", "POST_HASCREATOR_PERSON", "Post", "Person"},
            {"post_hasTag_tag", "POST_HASTAG_TAG", "Post", "Tag"},
            {"post_isLocatedIn_place", "POST_ISLOCATEDIN_PLACE", "Post", "Place"},
            {"tag_hasType_tagclass", "TAG_HASTYPE_TAGCLASS", "Tag", "TagClass"},
            {"tagclass_isSubclassOf_tagclass", "TAGCLASS_ISSUBCLASSOF_TAGCLASS", "TagClass", "TagClass"},
            {"place_isPartOf_place", "PLACE_ISPARTOF_PLACE", "Place", "Place"},
    }};
} // namespace polygraph::ingestion::detail
#endif
