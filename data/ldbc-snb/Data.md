# LDBC-SNB data statistics

```shell
python3 tools/polygraph.py stats ldbc --input data/ldbc-snb/sf1_0/raw/
```

LDBC dataset:
- Degree populations include zero-degree nodes from the corresponding node table
- Outgoing/incoming follow the raw first/second endpoint columns
- Combined degree is incoming + outgoing; self-loops contribute two


## Scaling Factor 0.1

### Dataset summary
```
+-----------------------------+-----------+
| Metric                      |     Value |
+-----------------------------+-----------+
| Csv Files                   |        33 |
| Logical Tables              |        33 |
| Rows                        | 1,812,248 |
| Node Rows                   |   327,588 |
| Relationship Rows           | 1,477,965 |
| Auxiliary Rows              |     6,695 |
| Unknown Rows                |         0 |
| Malformed Relationship Rows |         0 |
+-----------------------------+-----------+
```

### Logical CSV tables
```
+--------------------------------+--------------+---------+-------+---------+--------------+--------------+------------+
| Table                          | Kind         | Section | Files |    Rows | From         | To           | Mode       |
+--------------------------------+--------------+---------+-------+---------+--------------+--------------+------------+
| comment                        | node         | dynamic |     1 | 151,043 | -            | -            | -          |
| comment_hasCreator_person      | relationship | dynamic |     1 | 151,043 | Comment      | Person       | directed   |
| comment_hasTag_tag             | relationship | dynamic |     1 | 191,303 | Comment      | Tag          | directed   |
| comment_isLocatedIn_place      | relationship | dynamic |     1 | 151,043 | Comment      | Place        | directed   |
| comment_replyOf_comment        | relationship | dynamic |     1 |  76,787 | Comment      | Comment      | directed   |
| comment_replyOf_post           | relationship | dynamic |     1 |  74,256 | Comment      | Post         | directed   |
| forum                          | node         | dynamic |     1 |  13,750 | -            | -            | -          |
| forum_containerOf_post         | relationship | dynamic |     1 | 135,701 | Forum        | Post         | directed   |
| forum_hasMember_person         | relationship | dynamic |     1 | 123,268 | Forum        | Person       | directed   |
| forum_hasModerator_person      | relationship | dynamic |     1 |  13,750 | Forum        | Person       | directed   |
| forum_hasTag_tag               | relationship | dynamic |     1 |  47,697 | Forum        | Tag          | directed   |
| organisation                   | node         | static  |     1 |   7,955 | -            | -            | -          |
| organisation_isLocatedIn_place | relationship | static  |     1 |   7,955 | Organisation | Place        | directed   |
| person                         | node         | dynamic |     1 |   1,528 | -            | -            | -          |
| person_email_emailaddress      | auxiliary    | dynamic |     1 |   3,310 | -            | -            | -          |
| person_hasInterest_tag         | relationship | dynamic |     1 |  35,475 | Person       | Tag          | directed   |
| person_isLocatedIn_place       | relationship | dynamic |     1 |   1,528 | Person       | Place        | directed   |
| person_knows_person            | relationship | dynamic |     1 |  14,073 | Person       | Person       | undirected |
| person_likes_comment           | relationship | dynamic |     1 |  62,225 | Person       | Comment      | directed   |
| person_likes_post              | relationship | dynamic |     1 |  47,215 | Person       | Post         | directed   |
| person_speaks_language         | auxiliary    | dynamic |     1 |   3,385 | -            | -            | -          |
| person_studyAt_organisation    | relationship | dynamic |     1 |   1,209 | Person       | Organisation | directed   |
| person_workAt_organisation     | relationship | dynamic |     1 |   3,313 | Person       | Organisation | directed   |
| place                          | node         | static  |     1 |   1,460 | -            | -            | -          |
| place_isPartOf_place           | relationship | static  |     1 |   1,454 | Place        | Place        | directed   |
| post                           | node         | dynamic |     1 | 135,701 | -            | -            | -          |
| post_hasCreator_person         | relationship | dynamic |     1 | 135,701 | Post         | Person       | directed   |
| post_hasTag_tag                | relationship | dynamic |     1 |  51,118 | Post         | Tag          | directed   |
| post_isLocatedIn_place         | relationship | dynamic |     1 | 135,701 | Post         | Place        | directed   |
| tag                            | node         | static  |     1 |  16,080 | -            | -            | -          |
| tag_hasType_tagclass           | relationship | static  |     1 |  16,080 | Tag          | TagClass     | directed   |
| tagclass                       | node         | static  |     1 |      71 | -            | -            | -          |
| tagclass_isSubclassOf_tagclass | relationship | static  |     1 |      70 | TagClass     | TagClass     | directed   |
+--------------------------------+--------------+---------+-------+---------+--------------+--------------+------------+
```

### Outgoing degree by relationship table
```
+--------------------------------+--------------+---------+---------+---------+--------+-----+-----+-----+-----+--------+
| Relationship                   | Node type    |   Nodes |  Active |   Total |   Mean | Min | P50 | P95 | Max | Stddev |
+--------------------------------+--------------+---------+---------+---------+--------+-----+-----+-----+-----+--------+
| comment_hasCreator_person      | Comment      | 151,043 | 151,043 | 151,043 |  1.000 |   1 |   1 |   1 |   1 |  0.000 |
| comment_hasTag_tag             | Comment      | 151,043 |  50,339 | 191,303 |  1.267 |   0 |   0 |   7 |  19 |  2.308 |
| comment_isLocatedIn_place      | Comment      | 151,043 | 151,043 | 151,043 |  1.000 |   1 |   1 |   1 |   1 |  0.000 |
| comment_replyOf_comment        | Comment      | 151,043 |  76,787 |  76,787 |  0.508 |   0 |   1 |   1 |   1 |  0.500 |
| comment_replyOf_post           | Comment      | 151,043 |  74,256 |  74,256 |  0.492 |   0 |   0 |   1 |   1 |  0.500 |
| forum_containerOf_post         | Forum        |  13,750 |  12,617 | 135,701 |  9.869 |   0 |   9 |  20 | 218 |  8.433 |
| forum_hasMember_person         | Forum        |  13,750 |  11,077 | 123,268 |  8.965 |   0 |   4 |  33 | 340 | 14.944 |
| forum_hasModerator_person      | Forum        |  13,750 |  13,750 |  13,750 |  1.000 |   1 |   1 |   1 |   1 |  0.000 |
| forum_hasTag_tag               | Forum        |  13,750 |  13,750 |  47,697 |  3.469 |   1 |   1 |  18 |  80 | 10.443 |
| organisation_isLocatedIn_place | Organisation |   7,955 |   7,955 |   7,955 |  1.000 |   1 |   1 |   1 |   1 |  0.000 |
| person_hasInterest_tag         | Person       |   1,528 |   1,528 |  35,475 | 23.217 |   1 |  14 |  70 |  80 | 23.294 |
| person_isLocatedIn_place       | Person       |   1,528 |   1,528 |   1,528 |  1.000 |   1 |   1 |   1 |   1 |  0.000 |
| person_knows_person            | Person       |   1,528 |   1,199 |  14,073 |  9.210 |   0 |   3 |  40 | 243 | 15.705 |
| person_likes_comment           | Person       |   1,528 |   1,423 |  62,225 | 40.723 |   0 |  30 | 119 | 471 | 42.646 |
| person_likes_post              | Person       |   1,528 |   1,385 |  47,215 | 30.900 |   0 |  16 | 108 | 637 | 44.590 |
| person_studyAt_organisation    | Person       |   1,528 |   1,209 |   1,209 |  0.791 |   0 |   1 |   1 |   1 |  0.406 |
| person_workAt_organisation     | Person       |   1,528 |   1,207 |   3,313 |  2.168 |   0 |   2 |   5 |   5 |  1.601 |
| place_isPartOf_place           | Place        |   1,460 |   1,454 |   1,454 |  0.996 |   0 |   1 |   1 |   1 |  0.064 |
| post_hasCreator_person         | Post         | 135,701 | 135,701 | 135,701 |  1.000 |   1 |   1 |   1 |   1 |  0.000 |
| post_hasTag_tag                | Post         | 135,701 |  15,445 |  51,118 |  0.377 |   0 |   0 |   3 |  14 |  1.303 |
| post_isLocatedIn_place         | Post         | 135,701 | 135,701 | 135,701 |  1.000 |   1 |   1 |   1 |   1 |  0.000 |
| tag_hasType_tagclass           | Tag          |  16,080 |  16,080 |  16,080 |  1.000 |   1 |   1 |   1 |   1 |  0.000 |
| tagclass_isSubclassOf_tagclass | TagClass     |      71 |      70 |      70 |  0.986 |   0 |   1 |   1 |   1 |  0.118 |
+--------------------------------+--------------+---------+---------+---------+--------+-----+-----+-----+-----+--------+
```

### Incoming degree by relationship table
```
+--------------------------------+--------------+---------+---------+---------+---------+-----+-----+-------+--------+---------+
| Relationship                   | Node type    |   Nodes |  Active |   Total |    Mean | Min | P50 |   P95 |    Max |  Stddev |
+--------------------------------+--------------+---------+---------+---------+---------+-----+-----+-------+--------+---------+
| comment_hasCreator_person      | Person       |   1,528 |   1,400 | 151,043 |  98.850 |   0 |  39 |   412 |  2,179 | 160.439 |
| comment_hasTag_tag             | Tag          |  16,080 |  10,802 | 191,303 |  11.897 |   0 |   3 |    78 |  1,064 |  28.689 |
| comment_isLocatedIn_place      | Place        |   1,460 |     111 | 151,043 | 103.454 |   0 |   0 |   170 | 20,390 | 854.428 |
| comment_replyOf_comment        | Comment      | 151,043 |  32,525 |  76,787 |   0.508 |   0 |   0 |     3 |     13 |   1.203 |
| comment_replyOf_post           | Post         | 135,701 |  14,460 |  74,256 |   0.547 |   0 |   0 |     5 |     19 |   1.822 |
| forum_containerOf_post         | Post         | 135,701 | 135,701 | 135,701 |   1.000 |   1 |   1 |     1 |      1 |   0.000 |
| forum_hasMember_person         | Person       |   1,528 |   1,480 | 123,268 |  80.673 |   0 |  40 |   292 |  1,493 | 114.840 |
| forum_hasModerator_person      | Person       |   1,528 |   1,528 |  13,750 |   8.999 |   1 |   2 |    30 |     37 |  10.302 |
| forum_hasTag_tag               | Tag          |  16,080 |   4,182 |  47,697 |   2.966 |   0 |   0 |    20 |    366 |  12.449 |
| organisation_isLocatedIn_place | Place        |   1,460 |   1,203 |   7,955 |   5.449 |   0 |   2 |    21 |    130 |  10.098 |
| person_hasInterest_tag         | Tag          |  16,080 |   4,182 |  35,475 |   2.206 |   0 |   0 |    16 |    173 |   9.194 |
| person_isLocatedIn_place       | Place        |   1,460 |     903 |   1,528 |   1.047 |   0 |   1 |     3 |      6 |   1.091 |
| person_knows_person            | Person       |   1,528 |   1,205 |  14,073 |   9.210 |   0 |   4 |    35 |    331 |  17.832 |
| person_likes_comment           | Comment      | 151,043 |   4,701 |  62,225 |   0.412 |   0 |   0 |     0 |    269 |   5.143 |
| person_likes_post              | Post         | 135,701 |   8,419 |  47,215 |   0.348 |   0 |   0 |     1 |    338 |   3.601 |
| person_studyAt_organisation    | Organisation |   7,955 |     524 |   1,209 |   0.152 |   0 |   0 |     1 |     22 |   0.934 |
| person_workAt_organisation     | Organisation |   7,955 |   1,030 |   3,313 |   0.416 |   0 |   0 |     2 |     32 |   1.893 |
| place_isPartOf_place           | Place        |   1,460 |     117 |   1,454 |   0.996 |   0 |   0 |     1 |    199 |   8.316 |
| post_hasCreator_person         | Person       |   1,528 |   1,347 | 135,701 |  88.810 |   0 |  30 |   312 |    473 | 107.753 |
| post_hasTag_tag                | Tag          |  16,080 |   7,343 |  51,118 |   3.179 |   0 |   0 |    17 |    828 |  13.914 |
| post_isLocatedIn_place         | Place        |   1,460 |     111 | 135,701 |  92.946 |   0 |   0 |   165 | 20,470 | 774.289 |
| tag_hasType_tagclass           | TagClass     |      71 |      62 |  16,080 | 226.479 |   0 |  13 | 1,000 |  5,061 | 801.599 |
| tagclass_isSubclassOf_tagclass | TagClass     |      71 |      19 |      70 |   0.986 |   0 |   0 |     5 |     19 |   3.000 |
+--------------------------------+--------------+---------+---------+---------+---------+-----+-----+-------+--------+---------+
```

### Combined degree (incoming + outgoing)
```
+--------------------------------+-----------------------+---------+---------+---------+--------+-----+-----+-----+--------+--------+
| Relationship                   | Node type             |   Nodes |  Active |   Total |   Mean | Min | P50 | P95 |    Max | Stddev |
+--------------------------------+-----------------------+---------+---------+---------+--------+-----+-----+-----+--------+--------+
| comment_hasCreator_person      | Comment + Person      | 152,571 | 152,443 | 302,086 |  1.980 |   0 |   1 |   1 |  2,179 | 18.781 |
| comment_hasTag_tag             | Comment + Tag         | 167,123 |  61,141 | 382,606 |  2.289 |   0 |   0 |   8 |  1,064 |  9.687 |
| comment_isLocatedIn_place      | Comment + Place       | 152,503 | 151,154 | 302,086 |  1.981 |   0 |   1 |   1 | 20,390 | 84.194 |
| comment_replyOf_comment        | Comment               | 151,043 |  95,632 | 153,574 |  1.017 |   0 |   1 |   4 |     13 |  1.232 |
| comment_replyOf_post           | Comment + Post        | 286,744 |  88,716 | 148,512 |  0.518 |   0 |   0 |   1 |     19 |  1.305 |
| forum_containerOf_post         | Forum + Post          | 149,451 | 148,318 | 271,402 |  1.816 |   0 |   1 |   8 |    218 |  3.621 |
| forum_hasMember_person         | Forum + Person        |  15,278 |  12,557 | 246,536 | 16.137 |   0 |   5 |  62 |  1,493 | 44.529 |
| forum_hasModerator_person      | Forum + Person        |  15,278 |  15,278 |  27,500 |  1.800 |   1 |   1 |   2 |     37 |  4.046 |
| forum_hasTag_tag               | Forum + Tag           |  29,830 |  17,932 |  95,394 |  3.198 |   0 |   1 |  20 |    366 | 11.570 |
| organisation_isLocatedIn_place | Organisation + Place  |   9,415 |   9,158 |  15,910 |  1.690 |   0 |   1 |   4 |    130 |  4.290 |
| person_hasInterest_tag         | Person + Tag          |  17,608 |   5,710 |  70,950 |  4.029 |   0 |   0 |  30 |    173 | 12.620 |
| person_isLocatedIn_place       | Person + Place        |   2,988 |   2,431 |   3,056 |  1.023 |   0 |   1 |   3 |      6 |  0.763 |
| person_knows_person            | Person                |   1,528 |   1,357 |  28,146 | 18.420 |   0 |   9 |  65 |    340 | 26.639 |
| person_likes_comment           | Person + Comment      | 152,571 |   6,124 | 124,450 |  0.816 |   0 |   0 |   0 |    471 |  7.779 |
| person_likes_post              | Person + Post         | 137,229 |   9,804 |  94,430 |  0.688 |   0 |   0 |   1 |    637 |  6.726 |
| person_studyAt_organisation    | Person + Organisation |   9,483 |   1,733 |   2,418 |  0.255 |   0 |   0 |   1 |     22 |  0.902 |
| person_workAt_organisation     | Person + Organisation |   9,483 |   2,237 |   6,626 |  0.699 |   0 |   0 |   4 |     32 |  1.958 |
| place_isPartOf_place           | Place                 |   1,460 |   1,460 |   2,908 |  1.992 |   1 |   1 |   2 |    200 |  8.308 |
| post_hasCreator_person         | Post + Person         | 137,229 | 137,048 | 271,402 |  1.978 |   0 |   1 |   1 |    473 | 14.635 |
| post_hasTag_tag                | Post + Tag            | 151,781 |  22,788 | 102,236 |  0.674 |   0 |   0 |   4 |    828 |  4.772 |
| post_isLocatedIn_place         | Post + Place          | 137,161 | 135,812 | 271,402 |  1.979 |   0 |   1 |   1 | 20,470 | 80.440 |
| tag_hasType_tagclass           | Tag + TagClass        |  16,151 |  16,142 |  32,160 |  1.991 |   0 |   1 |   1 |  5,061 | 55.202 |
| tagclass_isSubclassOf_tagclass | TagClass              |      71 |      71 |     140 |  1.972 |   1 |   1 |   6 |     20 |  2.993 |
+--------------------------------+-----------------------+---------+---------+---------+--------+-----+-----+-----+--------+--------+
```

## Scaling Factor 1.0

Schema-based distribution

```json
{
  "input": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/raw",
  "output": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/graph.jsonl",
  "schema": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/ldbc-snb.graph-shape.json",
  "nodes": 3181724,
  "relationships": 17256038,
  "uuid_routing": [
    "Neo4J",
    "ArangoDB",
    "GPU"
  ],
  "uuid_routing_bits": 16,
  "placement": {
    "json": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/placement-report.json",
    "text": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/placement-report.txt",
    "dot": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/placement-graph.dot",
    "svg": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/placement-graph.svg",
    "pdf": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/placement-graph.pdf"
  },
  "cache": {
    "format": "polygraph.cache-plan",
    "version": 1,
    "graph": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/graph.jsonl",
    "nodes": 3181724,
    "relationships": 17256038,
    "locationEncoding": "bitmask-msb-v1",
    "regions": [
      {
        "location": "GPU:0",
        "deviceId": 0,
        "region": "original-mapped-region",
        "priority": 0,
        "vertices": 16151,
        "estimatedRelations": 3982883,
        "estimatedBytes": 32250688
      },
      {
        "location": "GPU:0",
        "deviceId": 0,
        "region": "hot-vertices",
        "priority": 0,
        "vertices": 3179383,
        "estimatedRelations": 34509679,
        "estimatedBytes": 352382624
      },
      {
        "location": "Redis",
        "region": "redis",
        "priority": 20,
        "vertices": 1000000,
        "estimatedRelations": 27926052,
        "estimatedBytes": 351408416
      }
    ],
    "artifacts": {
      "redis": {
        "manifest": "redis/manifest.json",
        "vertices": 1000000,
        "relations": 27926052,
        "bytes": 247408416
      }
    },
    "priorities": {
      "GPU": 0,
      "MMap": 10,
      "Redis": 20,
      "Neo4J": 50,
      "ArangoDB": 50
    },
    "gpus": [
      {
        "device_id": 0,
        "memory": "4GiB",
        "relation_limit": 150000000,
        "priority": 0,
        "regions": [
          {
            "name": "original-mapped-region",
            "policy": {
              "kind": "mapped",
              "owner_sources": [
                "GPU"
              ]
            }
          },
          {
            "name": "hot-vertices",
            "policy": {
              "kind": "top-degree",
              "vertex_count": 100000,
              "include_neighbors": true
            }
          }
        ]
      }
    ],
    "mmap": {},
    "redis": {
      "enabled": true,
      "endpoint": "127.0.0.1:6379",
      "memory": "2GiB",
      "vertex_limit": 1000000,
      "priority": 20,
      "key_prefix": "polygraph:adj:",
      "policy": {
        "kind": "top-degree"
      },
      "memory_utilization": 0.9,
      "entry_overhead_bytes": 128
    },
    "plan": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/cache/cache-plan.json"
  },
  "ignored_csv_groups": []
}
```

```json
{
  "Neo4J": {
    "nodes": 109799,
    "proxies": 3065611,
    "relationships": 11776068,
    "integer_node_ids": 109799,
    "integer_relationship_ids": 5664520,
    "id_map": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/graph.resolved.id-map.jsonl",
    "resolved_graph": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/graph.resolved.jsonl"
  },
  "ArangoDB": {
    "nodes": 3055774,
    "proxies": 106624,
    "relationships": 14769068
  },
  "GPU:0": {
    "device_id": 0,
    "nodes": 3179502,
    "proxies": 2222,
    "relationships": 17256036,
    "projection_vertices": 3181724,
    "runtime_projection": 1
  },
  "Redis": {
    "prepared_by_generation": 1
  },
  "domain": {
    "manifest": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/domain.json",
    "active_graph": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/graph.resolved.jsonl",
    "source_graph": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/graph.jsonl",
    "native_id_map": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf1_0/schema-based/graph.resolved.id-map.jsonl",
    "default_start_id": "11571159646280637"
  }
}
```

### Dataset summary
```
+-----------------------------+------------+
| Metric                      |      Value |
+-----------------------------+------------+
| Csv Files                   |         33 |
| Logical Tables              |         33 |
| Rows                        | 20,480,521 |
| Node Rows                   |  3,181,724 |
| Relationship Rows           | 17,256,038 |
| Auxiliary Rows              |     42,759 |
| Unknown Rows                |          0 |
| Malformed Relationship Rows |          0 |
+-----------------------------+------------+
```

### Logical CSV tables
```
+--------------------------------+--------------+---------+-------+-----------+--------------+--------------+------------+
| Table                          | Kind         | Section | Files |      Rows | From         | To           | Mode       |
+--------------------------------+--------------+---------+-------+-----------+--------------+--------------+------------+
| comment                        | node         | dynamic |     1 | 2,052,169 | -            | -            | -          |
| comment_hasCreator_person      | relationship | dynamic |     1 | 2,052,169 | Comment      | Person       | directed   |
| comment_hasTag_tag             | relationship | dynamic |     1 | 2,698,393 | Comment      | Tag          | directed   |
| comment_isLocatedIn_place      | relationship | dynamic |     1 | 2,052,169 | Comment      | Place        | directed   |
| comment_replyOf_comment        | relationship | dynamic |     1 | 1,040,749 | Comment      | Comment      | directed   |
| comment_replyOf_post           | relationship | dynamic |     1 | 1,011,420 | Comment      | Post         | directed   |
| forum                          | node         | dynamic |     1 |    90,492 | -            | -            | -          |
| forum_containerOf_post         | relationship | dynamic |     1 | 1,003,605 | Forum        | Post         | directed   |
| forum_hasMember_person         | relationship | dynamic |     1 | 1,611,869 | Forum        | Person       | directed   |
| forum_hasModerator_person      | relationship | dynamic |     1 |    90,492 | Forum        | Person       | directed   |
| forum_hasTag_tag               | relationship | dynamic |     1 |   309,766 | Forum        | Tag          | directed   |
| organisation                   | node         | static  |     1 |     7,955 | -            | -            | -          |
| organisation_isLocatedIn_place | relationship | static  |     1 |     7,955 | Organisation | Place        | directed   |
| person                         | node         | dynamic |     1 |     9,892 | -            | -            | -          |
| person_email_emailaddress      | auxiliary    | dynamic |     1 |    20,975 | -            | -            | -          |
| person_hasInterest_tag         | relationship | dynamic |     1 |   229,166 | Person       | Tag          | directed   |
| person_isLocatedIn_place       | relationship | dynamic |     1 |     9,892 | Person       | Place        | directed   |
| person_knows_person            | relationship | dynamic |     1 |   180,623 | Person       | Person       | undirected |
| person_likes_comment           | relationship | dynamic |     1 | 1,438,418 | Person       | Comment      | directed   |
| person_likes_post              | relationship | dynamic |     1 |   751,677 | Person       | Post         | directed   |
| person_speaks_language         | auxiliary    | dynamic |     1 |    21,784 | -            | -            | -          |
| person_studyAt_organisation    | relationship | dynamic |     1 |     7,949 | Person       | Organisation | directed   |
| person_workAt_organisation     | relationship | dynamic |     1 |    21,654 | Person       | Organisation | directed   |
| place                          | node         | static  |     1 |     1,460 | -            | -            | -          |
| place_isPartOf_place           | relationship | static  |     1 |     1,454 | Place        | Place        | directed   |
| post                           | node         | dynamic |     1 | 1,003,605 | -            | -            | -          |
| post_hasCreator_person         | relationship | dynamic |     1 | 1,003,605 | Post         | Person       | directed   |
| post_hasTag_tag                | relationship | dynamic |     1 |   713,258 | Post         | Tag          | directed   |
| post_isLocatedIn_place         | relationship | dynamic |     1 | 1,003,605 | Post         | Place        | directed   |
| tag                            | node         | static  |     1 |    16,080 | -            | -            | -          |
| tag_hasType_tagclass           | relationship | static  |     1 |    16,080 | Tag          | TagClass     | directed   |
| tagclass                       | node         | static  |     1 |        71 | -            | -            | -          |
| tagclass_isSubclassOf_tagclass | relationship | static  |     1 |        70 | TagClass     | TagClass     | directed   |
+--------------------------------+--------------+---------+-------+-----------+--------------+--------------+------------+
```

### Outgoing degree by relationship table
```
+--------------------------------+--------------+-----------+-----------+-----------+---------+-----+-----+-----+-------+---------+
| Relationship                   | Node type    |     Nodes |    Active |     Total |    Mean | Min | P50 | P95 |   Max |  Stddev |
+--------------------------------+--------------+-----------+-----------+-----------+---------+-----+-----+-----+-------+---------+
| comment_hasCreator_person      | Comment      | 2,052,169 | 2,052,169 | 2,052,169 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| comment_hasTag_tag             | Comment      | 2,052,169 |   685,366 | 2,698,393 |   1.315 |   0 |   0 |   7 |    26 |   2.373 |
| comment_isLocatedIn_place      | Comment      | 2,052,169 | 2,052,169 | 2,052,169 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| comment_replyOf_comment        | Comment      | 2,052,169 | 1,040,749 | 1,040,749 |   0.507 |   0 |   1 |   1 |     1 |   0.500 |
| comment_replyOf_post           | Comment      | 2,052,169 | 1,011,420 | 1,011,420 |   0.493 |   0 |   0 |   1 |     1 |   0.500 |
| forum_containerOf_post         | Forum        |    90,492 |    84,152 | 1,003,605 |  11.091 |   0 |  10 |  20 | 1,208 |  16.877 |
| forum_hasMember_person         | Forum        |    90,492 |    79,470 | 1,611,869 |  17.812 |   0 |   8 |  63 |   920 |  32.507 |
| forum_hasModerator_person      | Forum        |    90,492 |    90,492 |    90,492 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| forum_hasTag_tag               | Forum        |    90,492 |    90,492 |   309,766 |   3.423 |   1 |   1 |  17 |    80 |  10.339 |
| organisation_isLocatedIn_place | Organisation |     7,955 |     7,955 |     7,955 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| person_hasInterest_tag         | Person       |     9,892 |     9,892 |   229,166 |  23.167 |   1 |  14 |  71 |    80 |  23.242 |
| person_isLocatedIn_place       | Person       |     9,892 |     9,892 |     9,892 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| person_knows_person            | Person       |     9,892 |     8,250 |   180,623 |  18.260 |   0 |   7 |  71 |   814 |  37.826 |
| person_likes_comment           | Person       |     9,892 |     9,542 | 1,438,418 | 145.412 |   0 | 105 | 405 | 2,080 | 161.636 |
| person_likes_post              | Person       |     9,892 |     9,369 |   751,677 |  75.988 |   0 |  44 | 243 | 1,736 | 116.171 |
| person_studyAt_organisation    | Person       |     9,892 |     7,949 |     7,949 |   0.804 |   0 |   1 |   1 |     1 |   0.397 |
| person_workAt_organisation     | Person       |     9,892 |     7,998 |    21,654 |   2.189 |   0 |   2 |   5 |     5 |   1.586 |
| place_isPartOf_place           | Place        |     1,460 |     1,454 |     1,454 |   0.996 |   0 |   1 |   1 |     1 |   0.064 |
| post_hasCreator_person         | Post         | 1,003,605 | 1,003,605 | 1,003,605 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| post_hasTag_tag                | Post         | 1,003,605 |   206,972 |   713,258 |   0.711 |   0 |   0 |   5 |    15 |   1.736 |
| post_isLocatedIn_place         | Post         | 1,003,605 | 1,003,605 | 1,003,605 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| tag_hasType_tagclass           | Tag          |    16,080 |    16,080 |    16,080 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| tagclass_isSubclassOf_tagclass | TagClass     |        71 |        70 |        70 |   0.986 |   0 |   1 |   1 |     1 |   0.118 |
+--------------------------------+--------------+-----------+-----------+-----------+---------+-----+-----+-----+-------+---------+
```

### Incoming degree by relationship table
```
+--------------------------------+--------------+-----------+-----------+-----------+-----------+-----+-----+-------+---------+------------+
| Relationship                   | Node type    |     Nodes |    Active |     Total |      Mean | Min | P50 |   P95 |     Max |     Stddev |
+--------------------------------+--------------+-----------+-----------+-----------+-----------+-----+-----+-------+---------+------------+
| comment_hasCreator_person      | Person       |     9,892 |     9,343 | 2,052,169 |   207.457 |   0 |  78 |   813 |   8,915 |    409.142 |
| comment_hasTag_tag             | Tag          |    16,080 |    12,564 | 2,698,393 |   167.811 |   0 |  50 | 1,107 |  13,482 |    391.794 |
| comment_isLocatedIn_place      | Place        |     1,460 |       111 | 2,052,169 | 1,405.595 |   0 |   0 | 2,729 | 322,299 | 12,407.374 |
| comment_replyOf_comment        | Comment      | 2,052,169 |   441,704 | 1,040,749 |     0.507 |   0 |   0 |     3 |      14 |      1.203 |
| comment_replyOf_post           | Post         | 1,003,605 |   195,947 | 1,011,420 |     1.008 |   0 |   0 |     7 |      20 |      2.383 |
| forum_containerOf_post         | Post         | 1,003,605 | 1,003,605 | 1,003,605 |     1.000 |   1 |   1 |     1 |       1 |      0.000 |
| forum_hasMember_person         | Person       |     9,892 |     9,694 | 1,611,869 |   162.947 |   0 |  78 |   568 |   4,343 |    280.870 |
| forum_hasModerator_person      | Person       |     9,892 |     9,892 |    90,492 |     9.148 |   1 |   2 |    30 |      38 |     10.599 |
| forum_hasTag_tag               | Tag          |    16,080 |     9,837 |   309,766 |    19.264 |   0 |   1 |   141 |   2,890 |     79.645 |
| organisation_isLocatedIn_place | Place        |     1,460 |     1,203 |     7,955 |     5.449 |   0 |   2 |    21 |     130 |     10.098 |
| person_hasInterest_tag         | Tag          |    16,080 |     9,837 |   229,166 |    14.252 |   0 |   1 |   105 |   1,128 |     58.245 |
| person_isLocatedIn_place       | Place        |     1,460 |     1,343 |     9,892 |     6.775 |   0 |   7 |    12 |      19 |      3.365 |
| person_knows_person            | Person       |     9,892 |     8,466 |   180,623 |    18.260 |   0 |   8 |    66 |     766 |     36.851 |
| person_likes_comment           | Comment      | 2,052,169 |    63,351 | 1,438,418 |     0.701 |   0 |   0 |     0 |     894 |     11.336 |
| person_likes_post              | Post         | 1,003,605 |    70,668 |   751,677 |     0.749 |   0 |   0 |     1 |     902 |      9.432 |
| person_studyAt_organisation    | Organisation |     7,955 |     1,279 |     7,949 |     0.999 |   0 |   0 |     5 |     120 |      5.912 |
| person_workAt_organisation     | Organisation |     7,955 |     1,543 |    21,654 |     2.722 |   0 |   0 |    13 |     190 |     11.844 |
| place_isPartOf_place           | Place        |     1,460 |       117 |     1,454 |     0.996 |   0 |   0 |     1 |     199 |      8.316 |
| post_hasCreator_person         | Person       |     9,892 |     8,979 | 1,003,605 |   101.456 |   0 |  41 |   337 |   1,224 |    122.359 |
| post_hasTag_tag                | Tag          |    16,080 |    12,369 |   713,258 |    44.357 |   0 |  10 |   247 |  10,817 |    184.915 |
| post_isLocatedIn_place         | Place        |     1,460 |       111 | 1,003,605 |   687.401 |   0 |   0 | 1,417 | 141,852 |  5,853.900 |
| tag_hasType_tagclass           | TagClass     |        71 |        62 |    16,080 |   226.479 |   0 |  13 | 1,000 |   5,061 |    801.599 |
| tagclass_isSubclassOf_tagclass | TagClass     |        71 |        19 |        70 |     0.986 |   0 |   0 |     5 |      19 |      3.000 |
+--------------------------------+--------------+-----------+-----------+-----------+-----------+-----+-----+-------+---------+------------+
```

### Combined degree (incoming + outgoing)
```
+--------------------------------+-----------------------+-----------+-----------+-----------+--------+-----+-----+-----+---------+---------+
| Relationship                   | Node type             |     Nodes |    Active |     Total |   Mean | Min | P50 | P95 |     Max |  Stddev |
+--------------------------------+-----------------------+-----------+-----------+-----------+--------+-----+-----+-----+---------+---------+
| comment_hasCreator_person      | Comment + Person      | 2,062,061 | 2,061,512 | 4,104,338 |  1.990 |   0 |   1 |   1 |   8,915 |  31.726 |
| comment_hasTag_tag             | Comment + Tag         | 2,068,249 |   697,930 | 5,396,786 |  2.609 |   0 |   0 |   7 |  13,482 |  37.588 |
| comment_isLocatedIn_place      | Comment + Place       | 2,053,629 | 2,052,280 | 4,104,338 |  1.999 |   0 |   1 |   1 | 322,299 | 332.934 |
| comment_replyOf_comment        | Comment               | 2,052,169 | 1,296,179 | 2,081,498 |  1.014 |   0 |   1 |   4 |      14 |   1.233 |
| comment_replyOf_post           | Comment + Post        | 3,055,774 | 1,207,367 | 2,022,840 |  0.662 |   0 |   0 |   3 |      20 |   1.446 |
| forum_containerOf_post         | Forum + Post          | 1,094,097 | 1,087,757 | 2,007,210 |  1.835 |   0 |   1 |   7 |   1,208 |   5.593 |
| forum_hasMember_person         | Forum + Person        |   100,384 |    89,164 | 3,223,738 | 32.114 |   0 |   9 | 119 |   4,343 | 102.944 |
| forum_hasModerator_person      | Forum + Person        |   100,384 |   100,384 |   180,984 |  1.803 |   1 |   1 |   1 |      38 |   4.119 |
| forum_hasTag_tag               | Forum + Tag           |   106,572 |   100,329 |   619,532 |  5.813 |   0 |   1 |  22 |   2,890 |  32.863 |
| organisation_isLocatedIn_place | Organisation + Place  |     9,415 |     9,158 |    15,910 |  1.690 |   0 |   1 |   4 |     130 |   4.290 |
| person_hasInterest_tag         | Person + Tag          |    25,972 |    19,729 |   458,332 | 17.647 |   0 |   2 |  74 |   1,128 |  48.217 |
| person_isLocatedIn_place       | Person + Place        |    11,352 |    11,235 |    19,784 |  1.743 |   0 |   1 |   8 |      19 |   2.279 |
| person_knows_person            | Person                |     9,892 |     9,163 |   361,246 | 36.519 |   0 |  18 | 125 |     920 |  61.951 |
| person_likes_comment           | Person + Comment      | 2,062,061 |    72,893 | 2,876,836 |  1.395 |   0 |   0 |   0 |   2,080 |  18.793 |
| person_likes_post              | Person + Post         | 1,013,497 |    80,037 | 1,503,354 |  1.483 |   0 |   0 |   1 |   1,736 |  16.569 |
| person_studyAt_organisation    | Person + Organisation |    17,847 |     9,228 |    15,898 |  0.891 |   0 |   1 |   1 |     120 |   3.959 |
| person_workAt_organisation     | Person + Organisation |    17,847 |     9,541 |    43,308 |  2.427 |   0 |   1 |   6 |     190 |   7.999 |
| place_isPartOf_place           | Place                 |     1,460 |     1,460 |     2,908 |  1.992 |   1 |   1 |   2 |     200 |   8.308 |
| post_hasCreator_person         | Post + Person         | 1,013,497 | 1,012,584 | 2,007,210 |  1.980 |   0 |   1 |   1 |   1,224 |  15.610 |
| post_hasTag_tag                | Post + Tag            | 1,019,685 |   219,341 | 1,426,516 |  1.399 |   0 |   0 |   6 |  10,817 |  23.911 |
| post_isLocatedIn_place         | Post + Place          | 1,005,065 | 1,003,716 | 2,007,210 |  1.997 |   0 |   1 |   1 | 141,852 | 224.639 |
| tag_hasType_tagclass           | Tag + TagClass        |    16,151 |    16,142 |    32,160 |  1.991 |   0 |   1 |   1 |   5,061 |  55.202 |
| tagclass_isSubclassOf_tagclass | TagClass              |        71 |        71 |       140 |  1.972 |   1 |   1 |   6 |      20 |   2.993 |
+--------------------------------+-----------------------+-----------+-----------+-----------+--------+-----+-----+-----+---------+---------+
```



## Scaling Factor 10.0


Dataset summary
+-----------------------------+-------------+
| Metric                      |       Value |
+-----------------------------+-------------+
| Csv Files                   |          33 |
| Logical Tables              |          33 |
| Rows                        | 206,895,849 |
| Node Rows                   |  29,987,835 |
| Relationship Rows           | 176,623,433 |
| Auxiliary Rows              |     284,581 |
| Unknown Rows                |           0 |
| Malformed Relationship Rows |           0 |
+-----------------------------+-------------+

Logical CSV tables
+--------------------------------+--------------+---------+-------+------------+--------------+--------------+------------+
| Table                          | Kind         | Section | Files |       Rows | From         | To           | Mode       |
+--------------------------------+--------------+---------+-------+------------+--------------+--------------+------------+
| comment                        | node         | dynamic |     1 | 21,865,475 | -            | -            | -          |
| comment_hasCreator_person      | relationship | dynamic |     1 | 21,865,475 | Comment      | Person       | directed   |
| comment_hasTag_tag             | relationship | dynamic |     1 | 28,740,194 | Comment      | Tag          | directed   |
| comment_isLocatedIn_place      | relationship | dynamic |     1 | 21,865,475 | Comment      | Place        | directed   |
| comment_replyOf_comment        | relationship | dynamic |     1 | 11,089,373 | Comment      | Comment      | directed   |
| comment_replyOf_post           | relationship | dynamic |     1 | 10,776,102 | Comment      | Post         | directed   |
| forum                          | node         | dynamic |     1 |    595,453 | -            | -            | -          |
| forum_containerOf_post         | relationship | dynamic |     1 |  7,435,696 | Forum        | Post         | directed   |
| forum_hasMember_person         | relationship | dynamic |     1 | 17,168,614 | Forum        | Person       | directed   |
| forum_hasModerator_person      | relationship | dynamic |     1 |    595,453 | Forum        | Person       | directed   |
| forum_hasTag_tag               | relationship | dynamic |     1 |  2,065,319 | Forum        | Tag          | directed   |
| organisation                   | node         | static  |     1 |      7,955 | -            | -            | -          |
| organisation_isLocatedIn_place | relationship | static  |     1 |      7,955 | Organisation | Place        | directed   |
| person                         | node         | dynamic |     1 |     65,645 | -            | -            | -          |
| person_email_emailaddress      | auxiliary    | dynamic |     1 |    139,869 | -            | -            | -          |
| person_hasInterest_tag         | relationship | dynamic |     1 |  1,535,511 | Person       | Tag          | directed   |
| person_isLocatedIn_place       | relationship | dynamic |     1 |     65,645 | Person       | Place        | directed   |
| person_knows_person            | relationship | dynamic |     1 |  1,938,516 | Person       | Person       | undirected |
| person_likes_comment           | relationship | dynamic |     1 | 19,949,351 | Person       | Comment      | directed   |
| person_likes_post              | relationship | dynamic |     1 |  8,839,872 | Person       | Post         | directed   |
| person_speaks_language         | auxiliary    | dynamic |     1 |    144,712 | -            | -            | -          |
| person_studyAt_organisation    | relationship | dynamic |     1 |     52,632 | Person       | Organisation | directed   |
| person_workAt_organisation     | relationship | dynamic |     1 |    143,553 | Person       | Organisation | directed   |
| place                          | node         | static  |     1 |      1,460 | -            | -            | -          |
| place_isPartOf_place           | relationship | static  |     1 |      1,454 | Place        | Place        | directed   |
| post                           | node         | dynamic |     1 |  7,435,696 | -            | -            | -          |
| post_hasCreator_person         | relationship | dynamic |     1 |  7,435,696 | Post         | Person       | directed   |
| post_hasTag_tag                | relationship | dynamic |     1 |  7,599,701 | Post         | Tag          | directed   |
| post_isLocatedIn_place         | relationship | dynamic |     1 |  7,435,696 | Post         | Place        | directed   |
| tag                            | node         | static  |     1 |     16,080 | -            | -            | -          |
| tag_hasType_tagclass           | relationship | static  |     1 |     16,080 | Tag          | TagClass     | directed   |
| tagclass                       | node         | static  |     1 |         71 | -            | -            | -          |
| tagclass_isSubclassOf_tagclass | relationship | static  |     1 |         70 | TagClass     | TagClass     | directed   |
+--------------------------------+--------------+---------+-------+------------+--------------+--------------+------------+

Outgoing degree by relationship table
+--------------------------------+--------------+------------+------------+------------+---------+-----+-----+-----+-------+---------+
| Relationship                   | Node type    |      Nodes |     Active |      Total |    Mean | Min | P50 | P95 |   Max |  Stddev |
+--------------------------------+--------------+------------+------------+------------+---------+-----+-----+-----+-------+---------+
| comment_hasCreator_person      | Comment      | 21,865,475 | 21,865,475 | 21,865,475 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| comment_hasTag_tag             | Comment      | 21,865,475 |  7,289,227 | 28,740,194 |   1.314 |   0 |   0 |   7 |    25 |   2.370 |
| comment_isLocatedIn_place      | Comment      | 21,865,475 | 21,865,475 | 21,865,475 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| comment_replyOf_comment        | Comment      | 21,865,475 | 11,089,373 | 11,089,373 |   0.507 |   0 |   1 |   1 |     1 |   0.500 |
| comment_replyOf_post           | Comment      | 21,865,475 | 10,776,102 | 10,776,102 |   0.493 |   0 |   0 |   1 |     1 |   0.500 |
| forum_containerOf_post         | Forum        |    595,453 |    556,779 |  7,435,696 |  12.487 |   0 |  10 |  20 | 2,673 |  26.869 |
| forum_hasMember_person         | Forum        |    595,453 |    540,685 | 17,168,614 |  28.833 |   0 |  13 | 102 | 1,553 |  54.078 |
| forum_hasModerator_person      | Forum        |    595,453 |    595,453 |    595,453 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| forum_hasTag_tag               | Forum        |    595,453 |    595,453 |  2,065,319 |   3.468 |   1 |   1 |  18 |    80 |  10.445 |
| organisation_isLocatedIn_place | Organisation |      7,955 |      7,955 |      7,955 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| person_hasInterest_tag         | Person       |     65,645 |     65,645 |  1,535,511 |  23.391 |   1 |  15 |  71 |    80 |  23.312 |
| person_isLocatedIn_place       | Person       |     65,645 |     65,645 |     65,645 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| person_knows_person            | Person       |     65,645 |     57,134 |  1,938,516 |  29.530 |   0 |  11 | 115 | 1,455 |  61.743 |
| person_likes_comment           | Person       |     65,645 |     63,818 | 19,949,351 | 303.897 |   0 | 212 | 859 | 5,557 | 350.491 |
| person_likes_post              | Person       |     65,645 |     62,951 |  8,839,872 | 134.662 |   0 |  80 | 418 | 3,390 | 197.454 |
| person_studyAt_organisation    | Person       |     65,645 |     52,632 |     52,632 |   0.802 |   0 |   1 |   1 |     1 |   0.399 |
| person_workAt_organisation     | Person       |     65,645 |     52,639 |    143,553 |   2.187 |   0 |   2 |   5 |     5 |   1.597 |
| place_isPartOf_place           | Place        |      1,460 |      1,454 |      1,454 |   0.996 |   0 |   1 |   1 |     1 |   0.064 |
| post_hasCreator_person         | Post         |  7,435,696 |  7,435,696 |  7,435,696 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| post_hasTag_tag                | Post         |  7,435,696 |  2,196,348 |  7,599,701 |   1.022 |   0 |   0 |   6 |    17 |   2.001 |
| post_isLocatedIn_place         | Post         |  7,435,696 |  7,435,696 |  7,435,696 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| tag_hasType_tagclass           | Tag          |     16,080 |     16,080 |     16,080 |   1.000 |   1 |   1 |   1 |     1 |   0.000 |
| tagclass_isSubclassOf_tagclass | TagClass     |         71 |         70 |         70 |   0.986 |   0 |   1 |   1 |     1 |   0.118 |
+--------------------------------+--------------+------------+------------+------------+---------+-----+-----+-----+-------+---------+

Incoming degree by relationship table
+--------------------------------+--------------+------------+-----------+------------+------------+-----+-----+--------+-----------+-------------+
| Relationship                   | Node type    |      Nodes |    Active |      Total |       Mean | Min | P50 |    P95 |       Max |      Stddev |
+--------------------------------+--------------+------------+-----------+------------+------------+-----+-----+--------+-----------+-------------+
| comment_hasCreator_person      | Person       |     65,645 |    62,680 | 21,865,475 |    333.087 |   0 | 120 |  1,314 |    16,668 |     675.046 |
| comment_hasTag_tag             | Tag          |     16,080 |    13,586 | 28,740,194 |  1,787.325 |   0 | 526 | 11,882 |   158,058 |   4,214.419 |
| comment_isLocatedIn_place      | Place        |      1,460 |       111 | 21,865,475 | 14,976.353 |   0 |   0 | 26,285 | 3,222,362 | 130,708.524 |
| comment_replyOf_comment        | Comment      | 21,865,475 | 4,703,650 | 11,089,373 |      0.507 |   0 |   0 |      3 |        15 |       1.204 |
| comment_replyOf_post           | Post         |  7,435,696 | 2,083,998 | 10,776,102 |      1.449 |   0 |   0 |      8 |        20 |       2.747 |
| forum_containerOf_post         | Post         |  7,435,696 | 7,435,696 |  7,435,696 |      1.000 |   1 |   1 |      1 |         1 |       0.000 |
| forum_hasMember_person         | Person       |     65,645 |    64,609 | 17,168,614 |    261.537 |   0 | 127 |    904 |     7,508 |     452.145 |
| forum_hasModerator_person      | Person       |     65,645 |    65,645 |    595,453 |      9.071 |   1 |   2 |     30 |        40 |      10.499 |
| forum_hasTag_tag               | Tag          |     16,080 |    13,653 |  2,065,319 |    128.440 |   0 |  11 |    956 |    18,650 |     525.367 |
| organisation_isLocatedIn_place | Place        |      1,460 |     1,203 |      7,955 |      5.449 |   0 |   2 |     21 |       130 |      10.098 |
| person_hasInterest_tag         | Tag          |     16,080 |    13,653 |  1,535,511 |     95.492 |   0 |   8 |    716 |     7,502 |     387.766 |
| person_isLocatedIn_place       | Place        |      1,460 |     1,343 |     65,645 |     44.962 |   0 |  48 |     61 |       139 |      15.470 |
| person_knows_person            | Person       |     65,645 |    58,430 |  1,938,516 |     29.530 |   0 |  13 |    103 |     1,448 |      58.193 |
| person_likes_comment           | Comment      | 21,865,475 |   675,173 | 19,949,351 |      0.912 |   0 |   0 |      0 |       999 |      14.467 |
| person_likes_post              | Post         |  7,435,696 |   545,097 |  8,839,872 |      1.189 |   0 |   0 |      1 |       996 |      14.247 |
| person_studyAt_organisation    | Organisation |      7,955 |     3,194 |     52,632 |      6.616 |   0 |   0 |     36 |       749 |      38.881 |
| person_workAt_organisation     | Organisation |      7,955 |     1,575 |    143,553 |     18.046 |   0 |   0 |     86 |     1,253 |      78.720 |
| place_isPartOf_place           | Place        |      1,460 |       117 |      1,454 |      0.996 |   0 |   0 |      1 |       199 |       8.316 |
| post_hasCreator_person         | Person       |     65,645 |    60,521 |  7,435,696 |    113.271 |   0 |  55 |    358 |     3,021 |     138.539 |
| post_hasTag_tag                | Tag          |     16,080 |    13,646 |  7,599,701 |    472.618 |   0 | 104 |  2,671 |   126,411 |   2,009.091 |
| post_isLocatedIn_place         | Place        |      1,460 |       111 |  7,435,696 |  5,092.942 |   0 |   0 |  8,637 | 1,060,233 |  43,618.862 |
| tag_hasType_tagclass           | TagClass     |         71 |        62 |     16,080 |    226.479 |   0 |  13 |  1,000 |     5,061 |     801.599 |
| tagclass_isSubclassOf_tagclass | TagClass     |         71 |        19 |         70 |      0.986 |   0 |   0 |      5 |        19 |       3.000 |
+--------------------------------+--------------+------------+-----------+------------+------------+-----+-----+--------+-----------+-------------+

Combined degree (incoming + outgoing)
+--------------------------------+-----------------------+------------+------------+------------+--------+-----+-----+-----+-----------+-----------+
| Relationship                   | Node type             |      Nodes |     Active |      Total |   Mean | Min | P50 | P95 |       Max |    Stddev |
+--------------------------------+-----------------------+------------+------------+------------+--------+-----+-----+-----+-----------+-----------+
| comment_hasCreator_person      | Comment + Person      | 21,931,120 | 21,928,155 | 43,730,950 |  1.994 |   0 |   1 |   1 |    16,668 |    41.147 |
| comment_hasTag_tag             | Comment + Tag         | 21,881,555 |  7,302,813 | 57,480,388 |  2.627 |   0 |   0 |   7 |   158,058 |   124.097 |
| comment_isLocatedIn_place      | Comment + Place       | 21,866,935 | 21,865,586 | 43,730,950 |  2.000 |   0 |   1 |   1 | 3,222,362 | 1,075.024 |
| comment_replyOf_comment        | Comment               | 21,865,475 | 13,809,003 | 22,178,746 |  1.014 |   0 |   1 |   4 |        15 |     1.233 |
| comment_replyOf_post           | Comment + Post        | 29,301,171 | 12,860,100 | 21,552,204 |  0.736 |   0 |   0 |   3 |        20 |     1.508 |
| forum_containerOf_post         | Forum + Post          |  8,031,149 |  7,992,475 | 14,871,392 |  1.852 |   0 |   1 |   6 |     2,673 |     7.911 |
| forum_hasMember_person         | Forum + Person        |    661,098 |    605,294 | 34,337,228 | 51.940 |   0 |  15 | 196 |     7,508 |   166.664 |
| forum_hasModerator_person      | Forum + Person        |    661,098 |    661,098 |  1,190,906 |  1.801 |   1 |   1 |   2 |        40 |     4.095 |
| forum_hasTag_tag               | Forum + Tag           |    611,533 |    609,106 |  4,130,638 |  6.755 |   0 |   1 |  23 |    18,650 |    88.112 |
| organisation_isLocatedIn_place | Organisation + Place  |      9,415 |      9,158 |     15,910 |  1.690 |   0 |   1 |   4 |       130 |     4.290 |
| person_hasInterest_tag         | Person + Tag          |     81,725 |     79,298 |  3,071,022 | 37.578 |   0 |  12 |  73 |     7,502 |   175.622 |
| person_isLocatedIn_place       | Person + Place        |     67,105 |     66,988 |    131,290 |  1.956 |   0 |   1 |   1 |       139 |     6.807 |
| person_knows_person            | Person                |     65,645 |     62,371 |  3,877,032 | 59.061 |   0 |  30 | 204 |     1,553 |    99.972 |
| person_likes_comment           | Person + Comment      | 21,931,120 |    738,991 | 39,898,702 |  1.819 |   0 |   0 |   0 |     5,557 |    29.160 |
| person_likes_post              | Person + Post         |  7,501,341 |    608,048 | 17,679,744 |  2.357 |   0 |   0 |   1 |     3,390 |    26.400 |
| person_studyAt_organisation    | Person + Organisation |     73,600 |     55,826 |    105,264 |  1.430 |   0 |   1 |   1 |       749 |    12.915 |
| person_workAt_organisation     | Person + Organisation |     73,600 |     54,214 |    287,106 |  3.901 |   0 |   2 |   5 |     1,253 |    26.387 |
| place_isPartOf_place           | Place                 |      1,460 |      1,460 |      2,908 |  1.992 |   1 |   1 |   2 |       200 |     8.308 |
| post_hasCreator_person         | Post + Person         |  7,501,341 |  7,496,217 | 14,871,392 |  1.982 |   0 |   1 |   1 |     3,021 |    16.652 |
| post_hasTag_tag                | Post + Tag            |  7,451,776 |  2,209,994 | 15,199,402 |  2.040 |   0 |   0 |   6 |   126,411 |    95.880 |
| post_isLocatedIn_place         | Post + Place          |  7,437,156 |  7,435,807 | 14,871,392 |  2.000 |   0 |   1 |   1 | 1,060,233 |   615.299 |
| tag_hasType_tagclass           | Tag + TagClass        |     16,151 |     16,142 |     32,160 |  1.991 |   0 |   1 |   1 |     5,061 |    55.202 |
| tagclass_isSubclassOf_tagclass | TagClass              |         71 |         71 |        140 |  1.972 |   1 |   1 |   6 |        20 |     2.993 |
+--------------------------------+-----------------------+------------+------------+------------+--------+-----+-----+-----+-----------+-----------+


```json

{
  "domain": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf10_0/schema-based/domain.json",
  "graph": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf10_0/schema-based/graph.jsonl",
  "cachePlan": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf10_0/schema-based/cache/cache-plan.json",
  "backendConfig": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf10_0/schema-based/backends.json",
  "result": {
    "format": "polygraph.cache-plan",
    "version": 1,
    "graph": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf10_0/schema-based/graph.jsonl",
    "nodes": 29987835,
    "relationships": 176623433,
    "locationEncoding": "bitmask-msb-v1",
    "regions": [
      {
        "location": "GPU:0",
        "deviceId": 0,
        "region": "original-mapped-region",
        "priority": 0,
        "vertices": 16151,
        "estimatedRelations": 39973025,
        "estimatedBytes": 320171824
      },
      {
        "location": "GPU:0",
        "deviceId": 0,
        "region": "hot-cache",
        "priority": 0,
        "vertices": 1587,
        "estimatedRelations": 60026975,
        "estimatedBytes": 480253888
      },
      {
        "location": "Redis",
        "region": "redis",
        "priority": 20,
        "vertices": 1000000,
        "estimatedRelations": 209522036,
        "estimatedBytes": 1804176288
      }
    ],
    "artifacts": {
      "redis": {
        "manifest": "redis/manifest.json",
        "vertices": 1000000,
        "relations": 209522036,
        "bytes": 1700176288
      }
    },
    "priorities": {
      "GPU": 0,
      "Redis": 20,
      "Neo4J": 50,
      "ArangoDB": 50
    },
    "gpus": [
      {
        "device_id": 0,
        "memory": "3GiB",
        "relation_limit": 100000000,
        "priority": 0,
        "regions": [
          {
            "name": "original-mapped-region",
            "policy": {
              "kind": "mapped",
              "owner_sources": [
                "GPU"
              ]
            }
          },
          {
            "name": "hot-cache",
            "policy": {
              "kind": "top-degree",
              "vertex_count": 100000,
              "include_neighbors": true
            }
          }
        ]
      }
    ],
    "mmap": {
      "enabled": false
    },
    "redis": {
      "enabled": true,
      "endpoint": "127.0.0.1:6379",
      "memory": "2GiB",
      "vertex_limit": 1000000,
      "priority": 20,
      "key_prefix": "polygraph:adj:",
      "policy": {
        "kind": "top-degree"
      },
      "memory_utilization": 0.9,
      "entry_overhead_bytes": 128
    },
    "planningDatabase": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf10_0/schema-based/cache/cache-plan.sqlite3",
    "resumed": false,
    "plan": "/home/akos/Projects/diplomka/vermesa/data/ldbc-snb/sf10_0/schema-based/cache/cache-plan.json",
    "redisReload": {
      "vertices": 1000000,
      "relations": 209522036,
      "artifact_bytes": 1700176288,
      "endpoint": "127.0.0.1:6379",
      "database": 0,
      "key_prefix": "polygraph:adj:"
    }
  }
}
```