# Fast LDBC preparation without `graph.jsonl`

For large schema-placed LDBC SNB datasets, `tools/ldbc_snb/fast_prepare.py` avoids
materializing and reparsing the canonical JSONL graph. It performs one lightweight
node-ID scan, block-counts relationship rows, writes the schema/placement metadata,
and creates an immediately usable empty `08-no-cache` cache plan.

```bash
python3 tools/ldbc_snb/fast_prepare.py \
  --input data/ldbc-snb/sf10_0/raw \
  --sf 10 \
  --domain-id ldbc-snb-sf10_0-schema-based \
  --output-dir data/ldbc-snb/sf10_0/schema-based \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --storage-backends Neo4J ArangoDB PostgreSQL \
  --root-count 100
```

The output contains `graph.ldbc.json`, a small descriptor referencing the original
CsvBasic shards, rather than `graph.jsonl`. Python graph consumers transparently
stream records from the raw CSVs through the existing `GraphReader` interface.
The generated no-cache plan carries the routing metadata and empty residency index,
so the C++ HTTP runtime validates routing from the cache plan and does not need to
open the descriptor during normal no-cache execution.

Fast preparation requires explicit schema/type placement. Identity-hash placement
would require per-record placement statistics and is deliberately rejected.

This optimizes **preparation**, not the unavoidable work of importing millions of
records into Neo4j/ArangoDB/PostgreSQL. Persistent storage loading still parses the
source records when it actually imports them. Cache materialization can also read
`graph.ldbc.json` directly, so it no longer needs an intermediate JSONL copy, but
building large cache indexes/artifacts remains real work.
