# LDBC SNB tools

The public entry point is:

```bash
python3 tools/cpgdb.py prepare ldbc --input /path/to/social_network-sf0.1-CsvBasic-LongDateFormatter
```

The input must contain sibling `static/` and `dynamic/` directories. Both directories form the initial graph snapshot and are converted. Top-level `updateStream_*.csv` files are intentionally ignored for the read-only BFS workload.

To run the pinned official Datagen instead of using a downloaded dataset:

```bash
python3 tools/cpgdb.py generate ldbc --sf 0.1
```

`Dockerfile.datagen` replaces the retired upstream Java image while preserving Java 8 and Hadoop 3.2.1, which is the path expected by the pinned Datagen launcher.

See `tools/CPGDB_WORKFLOWS.md` for the complete build/load/run flow.
