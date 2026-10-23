# PolyGraph Benchmark Atlas — repeatable meeting reports

**Read-only** report generator for the PolyGraph BFS distribution/cache matrix. Produces a single-file, **offline interactive HTML dashboard** (Plotly bundled), presentation-ready PNG figures, a meeting brief, standardized CSVs, and an audit manifest.

The reporting tool does **not** change graph data, cache plans, measurements, event settings, C++ binaries, or Docker containers. It adds no runtime work to BFS measurements.

## Install once

From the PolyGraph repository root, using your regular Python environment:

```bash
PY=/home/akos/miniconda3/bin/python3
$PY -m pip install -r tools/measurement/reporting/requirements.txt
```

The generated HTML requires **no server**, Python, or Internet connection to open. The reporting process only reads small `measurements.csv` files and status metadata; it never needs the large `events/` or BFS response JSON files.

## For the meeting: generate the successful 40/40 baseline report

**Preferred**: generate directly from the complete original run's measurements (preserves result counts and request settings):

```bash
$PY tools/measurement/reporting/polygraph_report.py \
  --baseline measurements/sf1-distributions-d7-all-caches-20261007-185231 \
  --baseline-label 'SF1 · 7–8 Oct 2026 · completed 40/40' \
  --output reports/sf1-20261007

# Open reports/sf1-20261007/index.html
```

**Portable fallback**: this patch contains a small, source-labeled snapshot of all **40 successful configurations / 240 measured requests**, reconstructed from the original console log. Use it even if the original run directory is not on this machine:

```bash
$PY tools/measurement/reporting/polygraph_report.py \
  --baseline tools/measurement/reporting/baselines/sf1-d7-20261007 \
  --baseline-label 'SF1 · 7–8 Oct 2026 · console-log recovery' \
  --output reports/sf1-20261007-recovered
```

**Provenance limitation:** the recovered snapshot contains real HTTP/BFS timings, request identifiers, roots, success statuses, but no original vertex counts or complete request payloads. Use the original measurement directory for final thesis evidence when possible. The resulting report displays a provenance warning.

## Compare the next experiment, using the SAME baseline

Once `reports/sf1-20261007/` exists, it is itself a portable snapshot. You may archive the large source run and retain only the report (especially `normalized.csv`, `cases.csv`, `manifest.json`).

```bash
$PY tools/measurement/reporting/polygraph_report.py \
  --baseline reports/sf1-20261007 \
  --candidate measurements/NEW_RUN_DIRECTORY \
  --baseline-label 'Oct 7 baseline' \
  --candidate-label 'New experiment' \
  --output reports/compare-new-run
```

Or compare directly to the included compact baseline:

```bash
$PY tools/measurement/reporting/polygraph_report.py \
  --baseline tools/measurement/reporting/baselines/sf1-d7-20261007 \
  --candidate measurements/NEW_RUN_DIRECTORY \
  --output reports/compare-new-run
```

For your **Arango 256/Neo4j 16** run, replace `NEW_RUN_DIRECTORY` with `sf1-d7-arango256-c16-20261008-123916`; its 12 failing cases are explicitly marked unavailable rather than incorrectly shown as performance improvements.

Quickly compare any run against the included baseline:

```bash
tools/measurement/reporting/compare_sf1.sh \
  measurements/NEW_RUN_DIRECTORY \
  reports/compare-new-run
```

## What gets created

| Path | Use |
|---|---|
| `index.html` | Standalone meeting dashboard: performance landscape, filters, per-query distributions/boxplots, HTTP vs server BFS, comparisons |
| `figures/*.png` | Slide-ready latency heatmap, speedup heatmap, per-distribution rankings, optional change heatmap |
| `meeting_brief.md` | Talking points with precise figures and caveats |
| `normalized.csv` | Versioned per-query evidence for future comparisons |
| `cases.csv` | One record per distribution × cache; median/p95/mean, case success, speedup |
| `deltas.csv` | Matching cells, percentage improvements/regressions and exclusion reasons |
| `manifest.json` | Source, labels, metric definitions, request config fields when available, checksum |

**Measurement definition:** median server-side BFS latency in milliseconds (`server_bfs_ms`) across **measured successful requests**, after excluding warmups. Entire cases with failures are excluded even if some queries succeeded. Speedup = median no-cache latency / median cache latency *within the same distribution*. Candidate delta (%) = `100 × (candidate median / baseline median − 1)`; **negative is faster**.

An apples-to-apples candidate comparison requires the same distribution, cache, measured root UUIDs, and request profiles. When both runs expose vertex counts, they must match. The reporter warns about differing request settings when available; it cannot enforce machine/hardware/backend-equivalence, so record that independently. With only six measured requests per case, the results describe observed performance, not statistical certainty.

The schema is versioned `1.0`; keep normalized/cases/manifest together when archiving. No event logs are needed to preserve these comparisons.

## Console-log recovery (only when original CSV is unavailable)

```bash
$PY tools/measurement/reporting/recover_console_log.py \
  --log /path/to/completed-matrix.log \
  --output /tmp/recovered-matrix
$PY tools/measurement/reporting/polygraph_report.py \
  --baseline /tmp/recovered-matrix \
  --output reports/recovered-matrix
```

## Tests

```bash
$PY -m unittest discover -s tools/measurement/reporting/tests -v
```
