#!/usr/bin/env python3
"""PolyGraph SF1 reusable, offline meeting dashboard. Read-only against benchmark outputs.

Inputs: standard measurements/<run>/timing/<distribution>/<cache>/...
        OR an earlier dashboard snapshot (normalized.csv + manifest.json).
Outputs: one offline HTML, meeting-ready PNGs, normalized CSV, case CSV, delta CSV,
         machine-readable manifest, and a meeting summary.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import statistics
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

VERSION = "1.0"
CACHE_ORDER = [
    "01-gpu-100", "02-redis-100", "03-mmap-100", "04-gpu30-redis20-mmap20",
    "05-gpu20-redis15-mmap15", "06-redis-25", "07-mmap-25", "08-no-cache",
]
DIST_ORDER = ["schema-based", "hash-attributes", "label-striped-3", "schema-family", "proxy-minimized"]
CACHE_SHORT = {"01-gpu-100": "GPU 100%", "02-redis-100": "Redis 100%", "03-mmap-100": "MMap 100%",
               "04-gpu30-redis20-mmap20": "30/20/20 mix", "05-gpu20-redis15-mmap15": "20/15/15 mix",
               "06-redis-25": "Redis 25%", "07-mmap-25": "MMap 25%", "08-no-cache": "No cache"}
FIELDS = ["experiment", "distribution", "cache", "request_id", "template", "start_uuid", "measured", "status",
          "http_status", "server_bfs_ms", "duration_ms", "vertex_count", "error"]
CASE_FIELDS = ["experiment", "distribution", "cache", "case_status", "measured_total", "measured_success",
               "measured_failed", "median_server_bfs_ms", "mean_server_bfs_ms", "p95_server_bfs_ms",
               "median_http_ms", "min_server_bfs_ms", "max_server_bfs_ms", "speedup_vs_no_cache"]
DELTA_FIELDS = ["distribution", "cache", "baseline_status", "candidate_status", "comparable", "reason",
                "baseline_median_ms", "candidate_median_ms", "change_percent", "speedup_candidate_vs_baseline",
                "baseline_n", "candidate_n", "paired_root_count"]


def order(values: set[str], preferred: list[str]) -> list[str]:
    return [x for x in preferred if x in values] + sorted(values - set(preferred))


def readable_bool(v: Any) -> bool:
    return str(v).strip().lower() in ("1", "true", "yes")


def float_or_none(v: Any) -> float | None:
    try:
        x = float(v)
        return x if math.isfinite(x) else None
    except (ValueError, TypeError):
        return None


def template_of(s: str) -> str:
    return re.sub(r"^(?:r\d+-(?:measured-)?|warmup-\d+-)", "", s)


def percentile(vals: list[float], fraction: float) -> float:
    vals = sorted(vals)
    if not vals:
        return math.nan
    ix = fraction * (len(vals) - 1)
    a = int(ix)
    b = min(a + 1, len(vals) - 1)
    return vals[a] + (vals[b] - vals[a]) * (ix - a)


def csv_rows(path: Path, delimiter: str = ",") -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as f:
        return list(csv.DictReader(f, delimiter=delimiter))


def write_csv(path: Path, rows: list[dict], fields: list[str]):
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def locate_measurements(case_dir: Path, cache: str) -> Path | None:
    # Successful compact runs move their original measurements to case/<cache>/.
    direct = case_dir / cache / "measurements.csv"
    if direct.is_file():
        return direct
    # Failed compact runs retain staging rather than a finalized case directory.
    for p in sorted((case_dir / "_benchmark" / "staging").glob("*/" + cache + "/measurements.csv")):
        return p
    # Alternate benchmark configurations may place it in a slightly different location.
    matches = sorted(case_dir.glob("*/measurements.csv"))
    return matches[0] if matches else None


def request_config(root: Path, dist: str) -> dict:
    candidates = [root / "_requests" / (dist + ".json"), root / "_requests" / dist / "requests.json"]
    for p in candidates:
        if p.is_file():
            try:
                req = json.loads(p.read_text())
                return {"defaults": (req.get("defaults") or {}).get("payload", {}),
                        "requests": [{"id": r.get("id"), "startUuid": str(r.get("startUuid", "")),
                                      "payload": r.get("payload", {})} for r in req.get("requests", [])]}
            except (OSError, ValueError):
                pass
    return {}


def load(root: Path, label: str):
    if not root.is_dir():
        raise ValueError(f"Input directory not found: {root}")
    snapshot = root / "normalized.csv"
    if snapshot.exists():
        records = csv_rows(snapshot)
        m = json.loads((root / "manifest.json").read_text())
        statuses = {tuple(k.split("/", 1)): v for k, v in m.get("case_status", {}).items()}
        for r in records:
            r["experiment"] = label
        return records, statuses, m.get("request_configs", {}), "snapshot-log-recovered" if "log-recovered" in m.get("input_kind", "") else "snapshot"

    st: dict[tuple[str, str], str] = {}
    if (root / "status.tsv").is_file():
        for r in csv_rows(root / "status.tsv", "\t"):
            if r.get("phase", "timing") == "timing":
                st[(r.get("distribution", ""), r.get("cache", ""))] = r.get("status", "UNKNOWN").upper()
    records: list[dict] = []
    timing = root / "timing"
    if not timing.exists():
        raise ValueError(f"No timing/ or normalized.csv under {root}")
    for dist_dir in sorted(timing.iterdir()):
        if not dist_dir.is_dir():
            continue
        for case_dir in sorted(dist_dir.iterdir()):
            if not case_dir.is_dir():
                continue
            dist, cache = dist_dir.name, case_dir.name
            path = locate_measurements(case_dir, cache)
            if not path:
                st.setdefault((dist, cache), "MISSING")
                continue
            for r in csv_rows(path, ";"):
                record = {"experiment": label, "distribution": dist, "cache": cache,
                          "request_id": r.get("request_id", ""), "template": template_of(r.get("request_id", "")),
                          "start_uuid": r.get("start_uuid", ""),
                          "measured": str(readable_bool(r.get("measured", "true"))).lower(),
                          "status": r.get("status", "").lower(), "http_status": r.get("http_status", ""),
                          "server_bfs_ms": r.get("server_bfs_ms", ""), "duration_ms": r.get("duration_ms", ""),
                          "vertex_count": r.get("vertex_count", ""), "error": r.get("error", "")}
                records.append(record)
            st.setdefault((dist, cache), "INFERRED")
    configs = {dist: request_config(root, dist) for dist, _ in st}
    if not st:
        raise ValueError(f"No benchmark cases found under {timing}")
    return records, st, configs, "log-recovered" if (root / "recovered-from-console-log.json").exists() else "raw"


def summarize(records: list[dict], st: dict[tuple[str, str], str], experiment: str):
    grouped: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for r in records:
        if readable_bool(r["measured"]):
            grouped[(r["distribution"], r["cache"])].append(r)
    results: list[dict] = []
    for dist, cache in sorted(set(st) | set(grouped)):
        all_rows = grouped.get((dist, cache), [])
        good = [r for r in all_rows if r["status"] == "success" and float_or_none(r["server_bfs_ms"]) is not None]
        failed = len(all_rows) - len(good)
        status = st.get((dist, cache), "INFERRED")
        if status == "INFERRED":
            status = "SUCCESS" if all_rows and not failed else "FAILED"
        if status == "SUCCESS" and failed:
            status = "FAILED"  # Do not silently present contaminated cases as successful.
        times = [float(r["server_bfs_ms"]) for r in good]
        http_times = [float(r["duration_ms"]) for r in good if float_or_none(r["duration_ms"]) is not None]
        valid = status == "SUCCESS" and bool(times)
        results.append({"experiment": experiment, "distribution": dist, "cache": cache,
                        "case_status": status, "measured_total": len(all_rows), "measured_success": len(good),
                        "measured_failed": failed,
                        "median_server_bfs_ms": round(statistics.median(times), 4) if valid else None,
                        "mean_server_bfs_ms": round(statistics.mean(times), 4) if valid else None,
                        "p95_server_bfs_ms": round(percentile(times, .95), 4) if valid else None,
                        "median_http_ms": round(statistics.median(http_times), 4) if valid and http_times else None,
                        "min_server_bfs_ms": round(min(times), 4) if valid else None,
                        "max_server_bfs_ms": round(max(times), 4) if valid else None,
                        "speedup_vs_no_cache": None})
    baselines = {r["distribution"]: r["median_server_bfs_ms"] for r in results
                 if r["cache"] == "08-no-cache" and r["median_server_bfs_ms"]}
    for r in results:
        base = baselines.get(r["distribution"])
        value = r["median_server_bfs_ms"]
        if base and value and value > 0:
            r["speedup_vs_no_cache"] = round(base / value, 4)
    return results


def pair_signature(records: list[dict]):
    # Compare same logical roots, BFS profiles, and returned vertex counts.
    return {(r["template"], r["start_uuid"]): r["vertex_count"] for r in records
            if readable_bool(r["measured"]) and r["status"] == "success"}


def compare(baseline_cases: list[dict], candidate_cases: list[dict], b_records: list[dict], c_records: list[dict]):
    bmap = {(r["distribution"], r["cache"]): r for r in baseline_cases}
    cmap = {(r["distribution"], r["cache"]): r for r in candidate_cases}
    brows: dict[tuple[str, str], list[dict]] = defaultdict(list)
    crows: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for r in b_records:
        brows[(r["distribution"], r["cache"])].append(r)
    for r in c_records:
        crows[(r["distribution"], r["cache"])].append(r)
    out = []
    for key in sorted(bmap.keys() | cmap.keys()):
        a, b = bmap.get(key), cmap.get(key)
        bs = a["case_status"] if a else "MISSING"
        cs = b["case_status"] if b else "MISSING"
        same = False
        reason = ""
        nroots = 0
        if not a or not b or bs != "SUCCESS" or cs != "SUCCESS":
            reason = "Missing or unsuccessful case"
        else:
            ak = pair_signature(brows[key]); bk = pair_signature(crows[key])
            nroots = len(ak)
            if not ak or ak.keys() != bk.keys():
                reason = "Different measured root/profile coverage"
            elif any(ak[k] != bk[k] for k in ak if ak[k] and bk[k]):
                reason = "BFS vertex counts differ (correctness warning)"
            else:
                same = True
        av = a["median_server_bfs_ms"] if a else None
        bv = b["median_server_bfs_ms"] if b else None
        pct = round(100 * (bv / av - 1), 3) if same and av and bv and av > 0 else None
        out.append({"distribution": key[0], "cache": key[1], "baseline_status": bs, "candidate_status": cs,
                    "comparable": same, "reason": reason,
                    "baseline_median_ms": av, "candidate_median_ms": bv,
                    "change_percent": pct,
                    "speedup_candidate_vs_baseline": round(av / bv, 4) if same and bv and bv > 0 else None,
                    "baseline_n": a["measured_success"] if a else 0,
                    "candidate_n": b["measured_success"] if b else 0,
                    "paired_root_count": nroots})
    return out


def check_integrity(cases: list[dict], records: list[dict], configs: dict, expected: int = 40):
    notes = []
    good = [r for r in cases if r["case_status"] == "SUCCESS"]
    if len(cases) < expected:
        notes.append(f"Only {len(cases)} of the expected {expected} matrix cells are present.")
    bad = [r for r in cases if r["case_status"] != "SUCCESS"]
    if bad:
        notes.append(f"{len(bad)} cases did not succeed; their timings are excluded from performance charts.")
    grouped = defaultdict(list)
    for r in records:
        if readable_bool(r["measured"]) and r["status"] == "success":
            grouped[(r["distribution"], r["cache"], r["template"], r["start_uuid"])].append(r)
    inconsistent = []
    for key, rs in grouped.items():
        counts = {r["vertex_count"] for r in rs if r["vertex_count"]}
        if len(counts) > 1:
            inconsistent.append(key)
    if inconsistent:
        notes.append(f"Vertex-count mismatches across repetitions in {len(inconsistent)} root/profile groups.")
    dists = {r["distribution"] for r in cases}
    no_cache = {r["distribution"] for r in cases if r["cache"] == "08-no-cache" and r["case_status"] == "SUCCESS"}
    for d in sorted(dists - no_cache):
        notes.append(f"Missing successful no-cache reference for {d}: acceleration cannot be calculated.")
    counts = {r["measured_total"] for r in good}
    if len(counts) > 1:
        notes.append("Successful cases have different measured request counts; compare boxplots with sample sizes in mind.")
    if not configs or not any(configs.values()):
        notes.append("Request-plan metadata unavailable; external run configuration/hardware equivalence cannot be verified.")
    return notes


def metadata_diff(a: dict, b: dict) -> list[str]:
    messages = []
    for d in sorted(a.keys() & b.keys()):
        ca = a[d].get("defaults", {}); cb = b[d].get("defaults", {})
        for k in sorted(ca.keys() | cb.keys()):
            if ca.get(k) != cb.get(k):
                messages.append(f"{d}: {k}: {ca.get(k)!r} → {cb.get(k)!r}")
    return messages


def pretty_ms(ms: float | None) -> str:
    return "—" if ms is None else f"{ms / 1000:,.2f} s"


def summary_md(baseline, candidate, deltas, warnings, b_label, c_label):
    n = sum(r["case_status"] == "SUCCESS" for r in baseline)
    lines = ["# PolyGraph — Benchmark meeting brief", "", f"**Baseline:** {b_label} · {n}/{len(baseline)} successful cases",
             "", "**Metric:** median `server_bfs_ms` over *measured successful BFS requests*, warmups excluded.",
             "Failed or partially failed cases are excluded from numerical rankings.", "",
             "## Baseline highlights", "",
             "| Distribution | Fastest valid cache | Median server BFS | No-cache median | Speedup |",
             "|---|---|---:|---:|---:|"]
    for d in order({r["distribution"] for r in baseline}, DIST_ORDER):
        rs = [r for r in baseline if r["distribution"] == d and r["median_server_bfs_ms"]]
        if not rs: continue
        best = min(rs, key=lambda x: x["median_server_bfs_ms"])
        nc = next((r for r in rs if r["cache"] == "08-no-cache"), None)
        speedup = (nc["median_server_bfs_ms"] / best["median_server_bfs_ms"]) if nc else None
        lines.append(f"| {d} | {best['cache']} | {pretty_ms(best['median_server_bfs_ms'])} | "
                     f"{pretty_ms(nc['median_server_bfs_ms'] if nc else None)} | {speedup:.2f}× |" if speedup else
                     f"| {d} | {best['cache']} | {pretty_ms(best['median_server_bfs_ms'])} | — | — |")
    if candidate is not None:
        comp = [r for r in deltas if r["comparable"]]
        lines += ["", "## Changes versus baseline", "",
                  f"**Candidate:** {c_label}. {len(comp)} strictly comparable cases; "
                  f"{len(deltas) - len(comp)} unavailable/non-comparable cases.", "",
                  "Negative change (%) is faster; positive change (%) is slower.", "",
                  "| Distribution | Cache | Change |", "|---|---|---:|"]
        for r in sorted(comp, key=lambda x: abs(x["change_percent"]), reverse=True)[:12]:
            lines.append(f"| {r['distribution']} | {r['cache']} | {r['change_percent']:+.1f}% |")
    if warnings:
        lines += ["", "## Data-quality and comparability notes", ""] + [f"- {w}" for w in warnings]
    lines += ["", "## Definitions", "", "- **Median server BFS:** median of successful measured `server_bfs_ms` values within each valid case.",
              "- **Speedup vs no-cache:** no-cache case median divided by selected cache case median **within the same distribution**.",
              "- **Change:** 100 × (candidate median / baseline median − 1); negative is faster.",
              "- **Comparability:** same distribution, cache, measured root/profile coverage and BFS vertex counts; "
              "hardware/environment configuration must additionally be verified by the experimenter.",
              "- **Inference:** These are descriptive timings, not statistical significance claims. "
              "With only two repetitions per root, boxplots are indicative, not confidence intervals.", ""]
    return "\n".join(lines)


def _plot_static(out: Path, base_cases: list[dict], c_cases: list[dict] | None, deltas: list[dict]):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    from matplotlib.colors import LogNorm, TwoSlopeNorm
    from matplotlib.patches import Rectangle
    dist = order({r["distribution"] for r in base_cases}, DIST_ORDER)
    caches = order({r["cache"] for r in base_cases}, CACHE_ORDER)
    theme = {"ink": "#13243a", "muted": "#64748b", "accent": "#1d77b8"}
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "axes.titlesize": 16,
                         "axes.labelcolor": theme["ink"], "text.color": theme["ink"],
                         "axes.edgecolor": "#b6c5d5", "savefig.facecolor": "white", "figure.facecolor": "white"})
    lookup = {(r["distribution"], r["cache"]): r for r in base_cases}
    def draw_heat(metric, filename, title, subtitle, norm=None, cmap="YlGnBu", formatter=None):
        values = np.array([[float(lookup[(d, c)][metric]) if (d, c) in lookup and lookup[(d,c)][metric] is not None else np.nan
                            for c in caches] for d in dist])
        fig, ax = plt.subplots(figsize=(15, 6.2), layout="constrained")
        masked = np.ma.masked_invalid(values)
        img = ax.imshow(masked, cmap=cmap, norm=norm, aspect="auto")
        ax.set_xticks(range(len(caches)), [CACHE_SHORT.get(c,c) for c in caches], rotation=25, ha="right")
        ax.set_yticks(range(len(dist)), dist)
        ax.set_title(title, loc="left", fontweight="bold", pad=27)
        ax.text(0, 1.04, subtitle, transform=ax.transAxes, fontsize=10, color=theme["muted"])
        for i in range(len(dist)):
            for j in range(len(caches)):
                v = values[i,j]
                label = formatter(v) if np.isfinite(v) and formatter else (f"{v:.2f}" if np.isfinite(v) else "—")
                ax.text(j, i, label, ha="center", va="center", fontsize=10, fontweight="medium",
                        color="white" if np.isfinite(v) and (norm(v) if norm else (v - np.nanmin(values)) / max(np.nanmax(values) - np.nanmin(values), .001)) > .63 else "#172638")
        ax.set_xticks(np.arange(-.5,len(caches),1), minor=True)
        ax.set_yticks(np.arange(-.5,len(dist),1), minor=True)
        ax.grid(which="minor", color="white", linewidth=3)
        ax.tick_params(which="minor", length=0)
        fig.colorbar(img, ax=ax, fraction=.025, pad=.02)
        fig.savefig(out / filename, dpi=190, bbox_inches="tight")
        plt.close(fig)
    ms = [r["median_server_bfs_ms"] / 1000 for r in base_cases if r["median_server_bfs_ms"]]
    if ms:
        # Build scaled cases exclusively for plotting: remains untouched for exported reports.
        scaled = [{**r, "s": r["median_server_bfs_ms"] / 1000 if r["median_server_bfs_ms"] is not None else None}
                  for r in base_cases]
        lookup = {(r["distribution"], r["cache"]): r for r in scaled}
        draw_heat("s", "01_latency_landscape.png", "BFS latency across the architecture",
                  "Median server-side BFS · seconds · measured requests only · logarithmic colour scale",
                  norm=LogNorm(max(min(ms),.001), max(ms)*1.0001), cmap="YlGnBu", formatter=lambda v: f"{v:.1f}s")
    lookup = {(r["distribution"], r["cache"]): r for r in base_cases}
    vals = [r["speedup_vs_no_cache"] for r in base_cases if r["speedup_vs_no_cache"]]
    if vals:
        draw_heat("speedup_vs_no_cache", "02_speedup_vs_no_cache.png", "Acceleration relative to uncached BFS",
                  "Speedup × · above 1.00 is faster · normalized within each distribution",
                  norm=LogNorm(max(min(vals),.05), max(vals)*1.0001), cmap="PuBuGn", formatter=lambda v: f"{v:.1f}×")
    for d in dist:
        rs = [r for r in base_cases if r["distribution"] == d]
        rr = {(r["distribution"], r["cache"]): r for r in rs}
        fig, ax = plt.subplots(figsize=(13.8, 5.6), layout="constrained")
        X = np.arange(len(caches))
        y = [rr[(d,c)]["median_server_bfs_ms"] / 1000 if (d,c) in rr and rr[(d,c)]["median_server_bfs_ms"] else np.nan for c in caches]
        bars = ax.bar(X, np.nan_to_num(y), color="#2870a4", alpha=.94, width=.68)
        for i, v in enumerate(y):
            if np.isnan(v):
                ax.text(i, .05, "N/A", ha="center", color="#a02d42", fontsize=11, fontweight="bold")
                bars[i].set_alpha(.12)
            else:
                ax.text(i, v, f"{v:.2f}s", ha="center", va="bottom", fontsize=9)
        ax.set_xticks(X, [CACHE_SHORT.get(c,c) for c in caches], rotation=22, ha="right")
        ax.set_ylabel("Median server BFS (s)")
        ax.set_title(f"{d}  /  cache strategy", loc="left", fontweight="bold")
        ax.grid(axis="y", alpha=.19)
        ax.spines[["right", "top"]].set_visible(False)
        fig.savefig(out / f"03_{d}_cache_ranking.png", dpi=190, bbox_inches="tight")
        plt.close(fig)
    if c_cases is not None:
        dd = {(r["distribution"], r["cache"]): r for r in deltas}
        fig, ax = plt.subplots(figsize=(15,6.2), layout="constrained")
        arr = np.array([[dd[(d,c)]["change_percent"] if (d,c) in dd and dd[(d,c)]["comparable"] else np.nan
                         for c in caches] for d in dist], dtype=float)
        limit = max([abs(v) for v in arr.ravel() if np.isfinite(v)] + [5])
        im = ax.imshow(np.ma.masked_invalid(arr), cmap="RdYlGn_r", vmin=-limit, vmax=limit, aspect="auto")
        ax.set_xticks(range(len(caches)), [CACHE_SHORT.get(c,c) for c in caches], rotation=25, ha="right")
        ax.set_yticks(range(len(dist)), dist)
        ax.set_title("What changed relative to baseline?", loc="left", fontweight="bold", pad=28)
        ax.text(0,1.04,"Median server BFS % change · negative is faster · unmatched/failed cells omitted",
                transform=ax.transAxes,color=theme["muted"])
        for i in range(len(dist)):
            for j in range(len(caches)):
                v=arr[i,j]
                ax.text(j,i,f"{v:+.1f}%" if np.isfinite(v) else "—",ha="center",va="center",fontsize=10,
                        color="white" if np.isfinite(v) and abs(v) > limit*.65 else theme["ink"])
        ax.set_xticks(np.arange(-.5,len(caches),1), minor=True)
        ax.set_yticks(np.arange(-.5,len(dist),1), minor=True)
        ax.grid(which="minor",color="white",linewidth=3)
        ax.tick_params(which="minor",length=0)
        fig.colorbar(im,ax=ax,fraction=.025,pad=.02,label="Change (%)")
        fig.savefig(out / "04_change_vs_baseline.png", dpi=190, bbox_inches="tight")
        plt.close(fig)


HTML = r'''<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PolyGraph · Benchmark Atlas</title>
<style>
:root { --bg:#081321; --surface:#0d1e30; --panel:#10263b; --border:#2a4055; --ink:#edf5fe; --muted:#99b0c7; --cyan:#5dd9e8; --mint:#76e1b2; --orange:#ffb66c; --bad:#fc8590; }
*{box-sizing:border-box} html{scroll-behavior:smooth} body{margin:0;color:var(--ink);background:var(--bg);font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;font-size:15px} body:before{content:"";position:fixed;top:-430px;left:40%;width:1150px;height:850px;background:radial-gradient(ellipse,rgba(35,122,165,.19),transparent 70%);pointer-events:none;z-index:-1}
a{color:var(--cyan)} .wrap{max-width:1540px;margin:auto;padding:0 34px}.top{position:sticky;top:0;z-index:50;backdrop-filter:blur(18px);background:#081321dc;border-bottom:1px solid #294257}.top-inner{display:flex;align-items:center;justify-content:space-between;min-height:72px;gap:20px}.brand{letter-spacing:.08em;font-weight:830;font-size:15px}.brand b{color:var(--cyan)}.topnav{display:flex;flex-wrap:wrap;gap:22px}.topnav a{text-decoration:none;color:var(--muted);font-size:13px}.topnav a:hover{color:var(--cyan)}
.hero{padding:58px 0 22px}.eyebrow{font-size:12px;letter-spacing:.21em;text-transform:uppercase;color:var(--cyan);font-weight:800}.hero h1{font-size:clamp(32px,4vw,57px);line-height:1.04;letter-spacing:-.044em;margin:15px 0 16px;font-weight:830}.hero h1 em{font-style:normal;color:var(--cyan)}.sub{max-width:850px;color:var(--muted);line-height:1.7}.tagrow{display:flex;gap:8px;flex-wrap:wrap;margin-top:24px}.tag{border:1px solid var(--border);border-radius:8px;padding:8px 11px;font-size:12px;color:#d6e5f4;background:#112336}.tag.accent{border-color:#2d7a8e;color:var(--cyan)}
.kpis{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px;margin-top:26px}.kpi{background:linear-gradient(145deg,#122a3d,#0c1b2c);border:1px solid var(--border);border-radius:14px;padding:22px 25px;min-height:124px}.kpi .k{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.1em}.kpi .v{font-size:36px;font-weight:820;letter-spacing:-.04em;margin-top:9px}.kpi .detail{color:var(--muted);font-size:12px;margin-top:4px}
.section-head{display:flex;align-items:flex-end;justify-content:space-between;gap:20px;margin:45px 0 18px}.section-head h2{font-size:27px;letter-spacing:-.03em;margin:0}.section-head p{color:var(--muted);max-width:730px;margin:0;font-size:13px;line-height:1.55}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}.panel{border:1px solid var(--border);background:linear-gradient(155deg,#0e2337,#0b1929);border-radius:14px;overflow:hidden;min-width:0}.panel.wide{grid-column:1/-1}.panel-header{padding:20px 22px 7px}.panel h3{font-size:16px;letter-spacing:-.01em;margin:0 0 6px}.panel .desc{font-size:12px;color:var(--muted);margin:0;line-height:1.55}.plot{height:430px;width:100%;}.plot.large{height:510px}.filters{display:flex;flex-wrap:wrap;align-items:center;gap:10px;padding:15px 18px;border:1px solid var(--border);border-radius:12px;background:#0b1c2b;margin-bottom:16px}.filters label{color:var(--muted);font-size:12px}.filters select{background:#173149;color:var(--ink);border:1px solid #38546a;border-radius:7px;padding:10px 12px;font-size:13px}.filters .hint{margin-left:auto;color:var(--muted);font-size:12px}
.note{border:1px solid #2c5965;background:#102e37;padding:16px 18px;border-radius:11px;font-size:13px;color:#daebf3;line-height:1.6}.note.warn{border-color:#6b5d40;background:#30291b;color:#ffe7c4}.note + .note{margin-top:10px}.mini{color:var(--muted);font-size:12px}.data-table{width:100%;border-collapse:collapse;font-size:12px;white-space:nowrap}.data-table th{font-weight:670;text-align:left;color:var(--muted);text-transform:uppercase;letter-spacing:.09em;font-size:10px;background:#10283e;position:sticky;top:0}.data-table td,.data-table th{padding:12px 14px;border-bottom:1px solid #263d4f}.data-table tr:hover td{background:#132a3b}.table-scroll{overflow:auto;max-height:540px}.status-good{color:var(--mint)}.status-bad{color:var(--bad)} .faster{color:var(--mint)}.slower{color:var(--bad)} .padded{padding:18px 22px} .foot{color:var(--muted);border-top:1px solid var(--border);margin-top:55px;padding:30px 0 60px;font-size:12px;line-height:1.6} code{background:#142a41;border:1px solid #2d465d;padding:3px 5px;border-radius:4px;font-size:12px;overflow-wrap:anywhere}.blank{height:140px;display:flex;align-items:center;justify-content:center;color:var(--muted)}
@media(max-width:900px){.grid{grid-template-columns:1fr}.kpis{grid-template-columns:repeat(2,minmax(0,1fr))}.topnav{display:none}.wrap{padding:0 16px}.hero{padding-top:40px}.plot.large{height:450px}}@media(max-width:510px){.kpis{grid-template-columns:1fr 1fr}.kpi{padding:14px;min-height:96px}.kpi .v{font-size:26px}.plot{height:370px}}
@media print{body{background:white;color:#1b293a}.top,.filters{display:none}.panel{break-inside:avoid;background:white;border-color:#bbc6d1}.kpi{background:white;color:#172536}.sub,.section-head p,.panel .desc{color:#52606a}.plot{height:350px}}
</style></head><body>
<header class="top"><div class="wrap top-inner"><div class="brand">POLY<b>GRAPH</b> &nbsp; / &nbsp; BENCHMARK ATLAS</div><nav class="topnav"><a href="#landscape">Landscape</a><a href="#distribution">Distributions</a><a href="#comparison">Changes</a><a href="#quality">Coverage &amp; methodology</a></nav></div></header>
<main class="wrap"><div class="hero"><div class="eyebrow">PERFORMANCE RESEARCH / SF1 · REPRODUCIBLE REPORT</div><h1>One graph. <em>Many execution paths.</em></h1><div class="sub">An interactive record of PolyGraph BFS performance across data placements and cache strategies. Built for decisions today and defensible comparisons with every subsequent run.</div><div class="tagrow" id="tags"></div><div class="kpis" id="kpis"></div></div>
<div id="landscape"><div class="section-head"><h2>01 &nbsp; Performance landscape</h2><p>Absolute latency is useful for capacity planning; relative acceleration shows where caching actually pays off. These answer different questions and are deliberately kept separate.</p></div>
<div class="grid"><div class="panel wide"><div class="panel-header"><h3>Median BFS latency · all 40 matrix cells</h3><p class="desc">Seconds per request, measured-only. Logarithmic color scaling; unsuccessful cases have no latency color.</p></div><div class="plot large" id="heatmap"></div></div>
<div class="panel wide"><div class="panel-header"><h3>Acceleration versus uncached storage</h3><p class="desc">Within each distribution: no-cache median ÷ cache median. 2× means twice as fast; below 1× means slower.</p></div><div class="plot large" id="speedup"></div></div></div></div>
<div id="distribution"><div class="section-head"><h2>02 &nbsp; Inside each distribution</h2><p>Inspect caching trade-offs, warm execution variability and client-versus-server time for any placement strategy.</p></div>
<div class="filters"><label for="dist">PLACEMENT</label><select id="dist"></select><label for="metric">MEASURE</label><select id="metric"><option value="server_bfs_ms">Server BFS</option><option value="duration_ms">HTTP total</option></select><label for="series">SERIES</label><select id="series"><option value="baseline">Baseline</option><option value="candidate">Candidate</option></select><span class="hint">The same controls update both charts.</span></div>
<div class="grid"><div class="panel"><div class="panel-header"><h3>Cache strategy ranking</h3><p class="desc">Median measured request duration. Failed cases remain visible as unavailable.</p></div><div class="plot" id="ranking"></div></div>
<div class="panel"><div class="panel-header"><h3>Run-to-run variability</h3><p class="desc">Every dot is a measured request; box shows interquartile range and median. Warmups excluded.</p></div><div class="plot" id="boxplot"></div></div>
<div class="panel wide"><div class="panel-header"><h3>HTTP overhead versus BFS execution</h3><p class="desc">Points above the diagonal spend additional time in transport/serialization; requests shown only from successful cases.</p></div><div class="plot" id="http"></div></div></div></div>
<div id="comparison"><div class="section-head"><h2>03 &nbsp; What changed?</h2><p>Future runs become directly comparable to this baseline using the same canonical export and chart axes. Failed or mismatched cases never masquerade as improvements.</p></div>
<div id="comparison-body"></div></div>
<div id="quality"><div class="section-head"><h2>04 &nbsp; Coverage and trust</h2><p>These safeguards matter: a graph that excludes failures can make an unstable configuration look fast.</p></div>
<div class="grid"><div class="panel"><div class="panel-header"><h3>Completed matrix cells</h3><p class="desc">Run status, observed requests and descriptive median.</p></div><div class="table-scroll"><table class="data-table"><thead><tr><th>Placement</th><th>Cache</th><th>Status</th><th>Samples</th><th>Median</th></tr></thead><tbody id="coverage-table"></tbody></table></div></div><div class="panel"><div class="panel-header"><h3>Validation &amp; comparability</h3><p class="desc">Generated from the original measurement files and request metadata.</p></div><div class="padded" id="warnings"></div><div class="panel-header"><h3>How to interpret this report</h3></div><div class="padded mini" style="line-height:1.9"><strong>Typical runtime</strong> = median of successfully measured server BFS requests in a fully successful case.<br><strong>Warmup</strong> is excluded entirely.<br><strong>Speedup</strong> is calculated only against the same distribution's no-cache case.<br><strong>Change</strong> = (new median ÷ baseline median − 1) × 100%; negative is faster.<br><strong>Matching</strong> requires equal root/profile coverage and vertex counts; hardware and software comparability must also be checked.<br><strong>Uncertainty</strong>: these are descriptive samples, not confidence intervals or significance tests.</div></div></div></div>
</main><footer class="wrap foot">PolyGraph · Benchmark Atlas v1 · Generated from raw benchmark CSV files · No external resources required · Plotly charts support zoom, legend filtering and PNG exports via the modebar.<br>Archive <code>normalized.csv</code> + <code>manifest.json</code> with this report to preserve a compact, reusable baseline.</footer>
<script>/*__PLOTLY__*/</script>
<script>const DATA=/*__DATA__*/;
const palette={paper:'#0b1929',plot:'#0b1929',grid:'#274057',font:'#dae8f7',muted:'#9fb0c8'};
const cacheShort=DATA.cacheShort;
const fmt=s=>s===null||s===undefined?'—':(s/1000).toLocaleString('en',{maximumFractionDigits:2,minimumFractionDigits:2})+' s';
const round=(v,d=2)=>v===null||v===undefined?'—':Number(v).toFixed(d);
const valid=x=>typeof x==='number'&&isFinite(x);
const casesMap=arr=>Object.fromEntries(arr.map(x=>[x.distribution+'|'+x.cache,x]));
const bmap=casesMap(DATA.baselineCases),cmap=casesMap(DATA.candidateCases||[]);
function commonLayout(more={}) {return {paper_bgcolor:palette.paper,plot_bgcolor:palette.plot,font:{family:'Inter,system-ui,sans-serif',size:12,color:palette.font},margin:{l:130,r:35,t:30,b:93},xaxis:{gridcolor:palette.grid,zeroline:false},yaxis:{gridcolor:palette.grid,zeroline:false},hoverlabel:{bgcolor:'#14304a',font:{color:'#edf5ff'}},...more}}
const conf={responsive:true,displaylogo:false,modeBarButtonsToRemove:['lasso2d','select2d'],toImageButtonOptions:{format:'png',scale:2}};
function plot(id,traces,layout){Plotly.react(id,traces,layout,conf)}
const colorSet=['#55c6d6','#56d6a4','#909af5','#fbbd6e','#e893b1','#62add9','#bd9bff','#97d7ce'];
const tickCache=DATA.caches.map(c=>cacheShort[c]||c);
function heatMetric(){let arr=DATA.distributions.map(d=>DATA.caches.map(c=>{let x=bmap[d+'|'+c];return x&&valid(x.median_server_bfs_ms)&&x.median_server_bfs_ms>0?Math.log10(x.median_server_bfs_ms/1000):null}));
let texts=DATA.distributions.map(d=>DATA.caches.map(c=>{let x=bmap[d+'|'+c];return x&&valid(x.median_server_bfs_ms)?(x.median_server_bfs_ms/1000).toFixed(1)+'s':'×'}));
let mx=Math.max(...arr.flat().filter(valid)),mn=Math.min(...arr.flat().filter(valid));
plot('heatmap',[{type:'heatmap',x:tickCache,y:DATA.distributions,z:arr,text:texts,texttemplate:'%{text}',textfont:{size:13},colorscale:[[0,'#163249'],[.45,'#2277a9'],[1,'#efbd79']],zmin:mn,zmax:mx,colorbar:{title:{text:'log₁₀(sec)'},tickfont:{size:11}},hovertemplate:'<b>%{y}</b><br>%{x}<br>Median: %{text}<extra></extra>',xgap:4,ygap:4,hoverongaps:false}],commonLayout({margin:{l:157,r:35,t:20,b:93},yaxis:{autorange:'reversed',tickfont:{size:12}},xaxis:{tickangle:-25}}))}
function speedHeat(){let arr=DATA.distributions.map(d=>DATA.caches.map(c=>{let x=bmap[d+'|'+c];return x&&valid(x.speedup_vs_no_cache)&&x.speedup_vs_no_cache>0?Math.log2(x.speedup_vs_no_cache):null}));let texts=DATA.distributions.map(d=>DATA.caches.map(c=>{let x=bmap[d+'|'+c];return x&&valid(x.speedup_vs_no_cache)?x.speedup_vs_no_cache.toFixed(1)+'×':'×'}));let vals=arr.flat().filter(valid),lo=Math.min(...vals,0),hi=Math.max(...vals,0),limit=Math.max(Math.abs(lo),Math.abs(hi),.1);plot('speedup',[{type:'heatmap',x:tickCache,y:DATA.distributions,z:arr,text:texts,texttemplate:'%{text}',textfont:{size:13},zmin:-limit,zmax:limit,zmid:0,colorscale:[[0,'#b45b71'],[.5,'#1f3850'],[1,'#55d0ac']],xgap:4,ygap:4,hoverongaps:false,colorbar:{title:{text:'log₂(speedup)'},tickfont:{size:11}},hovertemplate:'<b>%{y}</b><br>%{x}<br>Speedup: %{text}<extra></extra>'}],commonLayout({margin:{l:157,r:35,t:20,b:93},yaxis:{autorange:'reversed'},xaxis:{tickangle:-25}}))}
function selectedRows(){let label=document.getElementById('series').value;return label==='candidate'&&DATA.candidateCases?{cases:DATA.candidateCases,records:DATA.candidateRecords,lookup:cmap}:{cases:DATA.baselineCases,records:DATA.baselineRecords,lookup:bmap}}
function updateDistribution(){let d=document.getElementById('dist').value,m=document.getElementById('metric').value;let {cases,records,lookup}=selectedRows();let one=records.filter(r=>r.distribution===d&&r.measured==='true'&&r.status==='success'&&valid(+r[m])&&lookup[d+'|'+r.cache]?.case_status==='SUCCESS');let by=Object.groupBy?Object.groupBy(one,x=>x.cache):one.reduce((a,x)=>((a[x.cache]??=[]).push(x),a),{});let med=DATA.caches.map(c=>{let vs=(by[c]||[]).map(r=>+r[m]);return vs.length?vs.sort((a,b)=>a-b)[Math.floor((vs.length-1)/2)]/1000:null});
let realMed=DATA.caches.map(c=>{let vs=(by[c]||[]).map(r=>+r[m]).sort((a,b)=>a-b);return vs.length?(vs.length%2?vs[(vs.length-1)/2]:(vs[vs.length/2-1]+vs[vs.length/2])/2)/1000:null});
plot('ranking',[{type:'bar',x:tickCache,y:realMed,marker:{color:DATA.caches.map((c,i)=>valid(realMed[i])?colorSet[i]:'#35495c')},text:realMed.map(v=>valid(v)?v.toFixed(2)+'s':'N/A'),textposition:'outside',hovertemplate:'%{x}<br>%{y:.3f} s<extra></extra>'}],commonLayout({margin:{l:62,r:17,t:35,b:99},xaxis:{tickangle:-33,tickfont:{size:10}},yaxis:{title:'Seconds',rangemode:'tozero'},showlegend:false}));
let traces=DATA.caches.map((c,i)=>{let vals=(by[c]||[]).map(r=>+r[m]/1000);return {type:'box',name:cacheShort[c]||c,y:vals,boxpoints:'all',jitter:.22,pointpos:0,marker:{color:colorSet[i],size:6,opacity:.75},line:{color:colorSet[i]},fillcolor:colorSet[i]+'25',hovertemplate:'%{y:.3f} s<extra>'+c+'</extra>'}});plot('boxplot',traces,commonLayout({margin:{l:62,r:12,t:25,b:97},xaxis:{tickangle:-35,tickfont:{size:10}},yaxis:{title:'Seconds'},showlegend:false}));
let x=one.filter(r=>valid(+r.server_bfs_ms)&&valid(+r.duration_ms)).map(r=>+r.server_bfs_ms/1000),y=one.filter(r=>valid(+r.server_bfs_ms)&&valid(+r.duration_ms)).map(r=>+r.duration_ms/1000);let mx=Math.max(...x,...y,1)*1.04;
plot('http',[{type:'scattergl',mode:'markers',x,y,text:one.map(r=>r.cache+' / '+r.request_id),marker:{color:one.map(r=>colorSet[DATA.caches.indexOf(r.cache)]||'#aaa'),opacity:.7,size:9},hovertemplate:'%{text}<br>BFS %{x:.3f}s · HTTP %{y:.3f}s<extra></extra>'},{type:'scatter',mode:'lines',x:[0,mx],y:[0,mx],line:{dash:'dash',width:1,color:'#8298ae'},hoverinfo:'skip',showlegend:false}],commonLayout({margin:{l:72,r:24,t:25,b:68},xaxis:{title:'Server BFS (s)',range:[0,mx]},yaxis:{title:'HTTP end-to-end (s)',range:[0,mx]},showlegend:false}));}
function addWarnings(){let box=document.getElementById('warnings');if(!DATA.warnings.length){box.innerHTML='<div class="note">No automatically detected data-quality issues. Match hardware, system load and software versions separately.</div>';return;}box.innerHTML=DATA.warnings.map(s=>'<div class="note warn">'+escapeHTML(s)+'</div>').join('')}
function escapeHTML(s){return String(s).replace(/[&<>"']/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function tableBase(){let arr=[...DATA.baselineCases].sort((a,b)=>(DATA.distributions.indexOf(a.distribution)-DATA.distributions.indexOf(b.distribution))||(DATA.caches.indexOf(a.cache)-DATA.caches.indexOf(b.cache)));document.getElementById('coverage-table').innerHTML=arr.map(x=>'<tr><td>'+escapeHTML(x.distribution)+'</td><td>'+escapeHTML(cacheShort[x.cache]||x.cache)+'</td><td class="'+(x.case_status==='SUCCESS'?'status-good':'status-bad')+'">'+escapeHTML(x.case_status)+'</td><td>'+x.measured_success+'/'+x.measured_total+'</td><td>'+fmt(x.median_server_bfs_ms)+'</td></tr>').join('')}
function renderChanges(){const p=document.getElementById('comparison-body');if(!DATA.candidateCases){p.innerHTML='<div class="note">This is the archived baseline view. To make this section interactive, generate another report with <code>--baseline BASELINE --candidate NEW_RUN</code>. Only successfully completed, workload-matched cases will produce a % change.</div>';return;}
p.innerHTML='<div class="grid"><div class="panel wide"><div class="panel-header"><h3>Performance change heatmap</h3><p class="desc">Relative to the same placement and cache in the baseline. Green is faster, red slower; × means failed or incompatible.</p></div><div id="change-chart" class="plot large"></div></div><div class="panel"><div class="panel-header"><h3>Baseline → candidate</h3><p class="desc">Each point is a complete matched case. Below diagonal = faster.</p></div><div id="scatter-chart" class="plot"></div></div><div class="panel"><div class="panel-header"><h3>Largest observed changes</h3><p class="desc">Ranked absolute % change, not statistical significance.</p></div><div class="table-scroll"><table class="data-table"><thead><tr><th>Distribution</th><th>Cache</th><th>Δ%</th><th>New median</th></tr></thead><tbody id="delta-table"></tbody></table></div></div></div>';
let lookup=Object.fromEntries(DATA.deltas.map(x=>[x.distribution+'|'+x.cache,x]));let arr=DATA.distributions.map(d=>DATA.caches.map(c=>{let x=lookup[d+'|'+c];return x&&x.comparable?x.change_percent:null}));let max=Math.max(...arr.flat().filter(valid).map(Math.abs),5);let txt=DATA.distributions.map(d=>DATA.caches.map(c=>{let x=lookup[d+'|'+c];return x&&x.comparable?(x.change_percent>0?'+':'')+x.change_percent.toFixed(1)+'%':'×'}));plot('change-chart',[{type:'heatmap',z:arr,x:tickCache,y:DATA.distributions,text:txt,texttemplate:'%{text}',textfont:{size:13},colorscale:[[0,'#3bab90'],[.5,'#193246'],[1,'#d15d6e']],zmin:-max,zmax:max,zmid:0,xgap:4,ygap:4,colorbar:{title:{text:'Change %'}},hoverongaps:false,hovertemplate:'%{y}<br>%{x}<br>Δ %{text}<extra></extra>'}],commonLayout({margin:{l:157,r:35,t:20,b:93},yaxis:{autorange:'reversed'},xaxis:{tickangle:-25}}));
let matched=DATA.deltas.filter(x=>x.comparable),maxV=Math.max(...matched.flatMap(x=>[x.baseline_median_ms,x.candidate_median_ms]).filter(valid),1)/1000*1.3,minV=Math.min(...matched.flatMap(x=>[x.baseline_median_ms,x.candidate_median_ms]).filter(valid),1000)/1000*.7;
plot('scatter-chart',[{type:'scatter',mode:'markers',x:matched.map(x=>x.baseline_median_ms/1000),y:matched.map(x=>x.candidate_median_ms/1000),text:matched.map(x=>x.distribution+' / '+x.cache+' · '+x.change_percent.toFixed(1)+'%'),marker:{size:10,color:matched.map(x=>x.change_percent),colorscale:[[0,'#3bab90'],[.5,'#b3bcc7'],[1,'#d15d6e']],opacity:.85},hovertemplate:'%{text}<br>Before %{x:.2f}s → after %{y:.2f}s<extra></extra>'},{type:'scatter',mode:'lines',x:[minV,maxV],y:[minV,maxV],line:{dash:'dot',color:'#7d92a5'},hoverinfo:'skip',showlegend:false}],commonLayout({margin:{l:73,r:20,t:32,b:73},xaxis:{title:'Baseline (s)',type:'log'},yaxis:{title:'Candidate (s)',type:'log'},showlegend:false}));
document.getElementById('delta-table').innerHTML=matched.sort((a,b)=>Math.abs(b.change_percent)-Math.abs(a.change_percent)).slice(0,24).map(x=>'<tr><td>'+escapeHTML(x.distribution)+'</td><td>'+escapeHTML(cacheShort[x.cache]||x.cache)+'</td><td class="'+(x.change_percent<=0?'faster':'slower')+'">'+(x.change_percent>0?'+':'')+x.change_percent.toFixed(1)+'%</td><td>'+fmt(x.candidate_median_ms)+'</td></tr>').join('')||'<tr><td colspan="4">No comparable completed cases</td></tr>';}
function init(){let b=DATA.baselineCases.filter(r=>r.case_status==='SUCCESS').length,tot=DATA.baselineCases.length,good=DATA.baselineCases.filter(r=>valid(r.median_server_bfs_ms)),best=good.length?good.reduce((a,x)=>(x.median_server_bfs_ms<a.median_server_bfs_ms?x:a)):null,changed=DATA.deltas.filter(x=>x.comparable).length;
document.getElementById('tags').innerHTML='<span class="tag accent">'+escapeHTML(DATA.baselineLabel)+'</span>'+(DATA.candidateLabel?'<span class="tag">vs '+escapeHTML(DATA.candidateLabel)+'</span>':'')+'<span class="tag">BFS · server-side timing</span><span class="tag">Warmups excluded</span><span class="tag">Offline, portable</span>';
document.getElementById('kpis').innerHTML=[['SUCCESSFUL CELLS',b+' / '+tot,'Incomplete cases excluded'],['DATA PLACEMENTS',String(DATA.distributions.length),'Distribution strategies'],['CACHE STRATEGIES',String(DATA.caches.length),'Across each placement'],[DATA.candidateLabel?'MATCHED COMPARISONS':'FASTEST CASE',DATA.candidateLabel?String(changed):best?fmt(best.median_server_bfs_ms):'—',DATA.candidateLabel?'Safe paired case comparisons':best?best.distribution+' / '+(cacheShort[best.cache]||best.cache):'No valid case']].map(x=>'<div class="kpi"><div class="k">'+x[0]+'</div><div class="v">'+x[1]+'</div><div class="detail">'+escapeHTML(x[2])+'</div></div>').join('');
let dist=document.getElementById('dist');dist.innerHTML=DATA.distributions.map(d=>'<option value="'+escapeHTML(d)+'">'+escapeHTML(d)+'</option>').join('');document.getElementById('series').querySelector('option[value="candidate"]').disabled=!DATA.candidateCases;for(let s of ['dist','metric','series'])document.getElementById(s).addEventListener('change',updateDistribution);heatMetric();speedHeat();updateDistribution();renderChanges();addWarnings();tableBase();}
init();</script></body></html>'''


def export_html(path: Path, dataset: dict):
    try:
        from plotly.offline import get_plotlyjs
    except ImportError as e:
        raise SystemExit("Install requirements: python -m pip install plotly matplotlib numpy") from e
    js = get_plotlyjs()
    payload = json.dumps(dataset, ensure_ascii=False, separators=(",", ":"), allow_nan=False).replace("</", "<\\/")
    path.write_text(HTML.replace("/*__PLOTLY__*/", js).replace("/*__DATA__*/", payload), encoding="utf-8")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--baseline", type=Path, required=True, help="Completed PolyGraph experiment or saved report snapshot")
    p.add_argument("--candidate", type=Path, help="Optional future run to compare against baseline")
    p.add_argument("--output", type=Path, required=True, help="Output report directory; must not be an input directory")
    p.add_argument("--baseline-label", help="Human-readable baseline name")
    p.add_argument("--candidate-label", help="Human-readable candidate name")
    p.add_argument("--expected-cases", type=int, default=40)
    args = p.parse_args(argv)
    baseline = args.baseline.resolve(); candidate = args.candidate.resolve() if args.candidate else None
    output = args.output.resolve()
    if baseline == output or candidate == output or baseline in output.parents and (baseline / "normalized.csv").exists():
        p.error("Output must be separate from input experiment/snapshot")
    b_label = args.baseline_label or baseline.name
    c_label = (args.candidate_label or candidate.name) if candidate else None
    b_records, b_status, b_configs, b_kind = load(baseline, b_label)
    b_cases = summarize(b_records, b_status, b_label)
    if candidate:
        c_records, c_status, c_configs, c_kind = load(candidate, c_label)
        c_cases = summarize(c_records, c_status, c_label)
        deltas = compare(b_cases, c_cases, b_records, c_records)
    else:
        c_records, c_status, c_configs, c_cases, deltas = [], {}, {}, None, []
    warnings = check_integrity(b_cases, b_records, b_configs, args.expected_cases)
    if "log-recovered" in b_kind:
        warnings.append("Baseline timings were reconstructed from the terminal log rather than original CSVs. HTTP and server BFS timing values are preserved, but per-request vertex counts, full request payloads and some other provenance fields are unavailable. Regenerate from the original experiment folder when possible.")
    if candidate:
        warnings += ["Candidate: " + w for w in check_integrity(c_cases, c_records, c_configs, args.expected_cases)]
        if "log-recovered" in c_kind:
            warnings.append("Candidate source is also console-log recovered: vertex count verification is unavailable.")
        if "log-recovered" in b_kind:
            warnings.append("Baseline vs candidate correctness comparison cannot verify BFS vertex counts because the baseline console log omits them; manually validate correctness using original response files.")
        unmatched = [r for r in deltas if not r["comparable"]]
        if unmatched:
            warnings.append(f"{len(unmatched)} paired cells excluded from deltas (failed, missing, different roots/profiles or different vertex counts).")
        diff = metadata_diff(b_configs, c_configs)
        if diff:
            warnings.append(f"Request configuration changed ({len(diff)} differences). This is a comparative experiment, not an identical-configuration rerun: " + "; ".join(diff[:8]))
    dists = order({r["distribution"] for r in b_cases + (c_cases or [])}, DIST_ORDER)
    caches = order({r["cache"] for r in b_cases + (c_cases or [])}, CACHE_ORDER)
    dataset = {"reportVersion": VERSION, "baselineLabel": b_label, "candidateLabel": c_label,
               "baselineCases": b_cases, "candidateCases": c_cases, "deltas": deltas,
               "baselineRecords": b_records, "candidateRecords": c_records,
               "distributions": dists, "caches": caches, "cacheShort": CACHE_SHORT, "warnings": warnings}
    output.mkdir(parents=True, exist_ok=True)
    write_csv(output / "normalized.csv", b_records, FIELDS)
    write_csv(output / "cases.csv", b_cases, CASE_FIELDS)
    write_csv(output / "deltas.csv", deltas, DELTA_FIELDS)
    if candidate:
        write_csv(output / "candidate.normalized.csv", c_records, FIELDS)
        write_csv(output / "candidate.cases.csv", c_cases, CASE_FIELDS)
    manifest = {"schema_version": VERSION, "created_utc": datetime.now(timezone.utc).isoformat(),
                "source": str(baseline), "label": b_label, "input_kind": b_kind,
                "case_status": {d + "/" + c: s for (d,c),s in b_status.items()},
                "request_configs": b_configs,
                "normalized_sha256": hashlib.sha256((output / "normalized.csv").read_bytes()).hexdigest(),
                "metric_definition": "median(server_bfs_ms) on measured successful requests from fully successful cases",
                "expected_cases": args.expected_cases}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    (output / "meeting_brief.md").write_text(summary_md(b_cases,c_cases,deltas,warnings,b_label,c_label))
    export_html(output / "index.html", dataset)
    image_out = output / "figures"; image_out.mkdir(exist_ok=True)
    _plot_static(image_out, b_cases, c_cases, deltas)
    print(f"Report: {output / 'index.html'}")
    print(f"Meeting summary: {output / 'meeting_brief.md'}")
    print(f"PNG charts: {image_out}")
    print(f"Portable baseline: {output / 'normalized.csv'} + {output / 'manifest.json'}")
    print(f"Valid baseline cells: {sum(r['case_status'] == 'SUCCESS' for r in b_cases)}/{len(b_cases)}")
    if candidate:
        print(f"Comparable paired cells: {sum(r['comparable'] for r in deltas)}/{len(deltas)}")
    for w in warnings:
        print("WARNING: " + w, file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
