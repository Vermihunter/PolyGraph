#!/usr/bin/env python3
"""Recover raw request timings from the PolyGraph distribution-matrix terminal log.

Fallback only: use original measurements/<run>/timing/... if available.
Recovered logs don't contain vertex counts or full request payloads.
"""
from __future__ import annotations
import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path

CASE = re.compile(r'^timing / ([^ /]+) / ([^ /]+)\s*$')
EVENT = re.compile(r'^\s*\[(\d+)/(\d+)\]\s+(\S+)\s+start=(\d+):\s+(success|failed);\s+HTTP\s+([0-9.]+)\s+ms;\s+server BFS\s+([0-9.]+)\s+ms;\s+schedule delay\s+([0-9.]+)\s+ms')
STATUS = re.compile(r'^timing\s+(\S+)\s+(\S+)\s+(SUCCESS|FAILED|[\w-]+)\s+(\d+)\s+([\dTZ+:-]+)')


def recover(src: Path, output: Path):
    rows = defaultdict(list)
    case = None
    status = {}
    for line in src.read_text(encoding='utf-8', errors='replace').splitlines():
        m = CASE.search(line)
        if m:
            case = (m.group(1), m.group(2))
            continue
        m = EVENT.search(line)
        if m and case:
            idx,total,rid,uid,state,http,bfs,schedule = m.groups()
            rows[case].append({'run_index':idx,'request_id':rid,'start_uuid':uid,
                               'measured':str(not rid.startswith('warmup-')),
                               'status':state,'http_status':'200' if state=='success' else '',
                               'duration_ms':http,'server_bfs_ms':bfs if state=='success' else '0',
                               'vertex_count':'','schedule_delay_ms':schedule,'error':''})
        m = STATUS.match(line)
        if m:
            d,c,s,code,finished=m.groups()
            status[(d,c)]=(s,code,finished)
    if not rows:
        raise ValueError(f'No PolyGraph request timings in {src}')
    if status and (set(status) != set(rows)):
        raise ValueError('Log has inconsistent status table and timing sections; likely truncated.')
    if output.resolve() == src.parent.resolve():
        raise ValueError('Output must be an independent directory')
    output.mkdir(parents=True,exist_ok=True)
    with (output/'status.tsv').open('w',newline='') as f:
        writer=csv.writer(f,delimiter='\t');writer.writerow(['phase','distribution','cache','status','exit_code','finished'])
        for d,c in sorted(set(status)|set(rows)):
            s,code,time=status.get((d,c),('SUCCESS' if all(r['status']=='success' for r in rows[(d,c)]) else 'FAILED','', ''))
            writer.writerow(['timing',d,c,s,code,time])
    fields=['run_index','request_id','start_uuid','measured','status','http_status','duration_ms','server_bfs_ms','vertex_count','schedule_delay_ms','error']
    for (d,c),rs in rows.items():
        dest=output/'timing'/d/c/c
        dest.mkdir(parents=True,exist_ok=True)
        with (dest/'measurements.csv').open('w',newline='') as f:
            writer=csv.DictWriter(f,fieldnames=fields,delimiter=';');writer.writeheader();writer.writerows(rs)
    (output/'recovered-from-console-log.json').write_text(json.dumps({
        'source':str(src.resolve()), 'data_quality':'Recovered from console log; no vertex counts or full request configs',
        'case_count':len(rows),'request_count':sum(map(len,rows.values())),
        'note':'Original per-request measurements.csv provides richer evidence and is preferred for future runs.'},indent=2)+'\n')
    print(f'Recovered {len(rows)} cases / {sum(map(len,rows.values()))} requests -> {output}')
    return len(rows)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    recover(args.log,args.output)

if __name__=='__main__':main()
