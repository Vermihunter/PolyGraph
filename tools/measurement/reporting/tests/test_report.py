import csv
import json
import tempfile
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import polygraph_report as r

class ReportTests(unittest.TestCase):
    def make(self, base, distribution='schema-based', cache='01-gpu-100', fail=False, seconds=2.0,
             root='123', staging=False):
        d = base / 'timing' / distribution / cache
        output = (d / '_benchmark' / 'staging' / ('01-'+cache) / cache) if staging else (d / cache)
        output.mkdir(parents=True, exist_ok=True)
        with (output / 'measurements.csv').open('w', newline='') as f:
            fields=['request_id','start_uuid','measured','status','http_status','server_bfs_ms','duration_ms','vertex_count','error']
            w=csv.DictWriter(f,fieldnames=fields,delimiter=';');w.writeheader()
            for i,(rid,meas,ok,t) in enumerate([('warmup-01-root-01-d7-both',False,True,7000),
                                                   ('r001-root-01-d7-both',True,True,seconds*1000),
                                                   ('r002-root-01-d7-both',True,not fail,seconds*1100)]):
                w.writerow(dict(request_id=rid,start_uuid=root,measured=meas,status='success' if ok else 'failed',
                                http_status=200 if ok else 400,server_bfs_ms=t if ok else 0,
                                duration_ms=t+100 if ok else 100, vertex_count=3181724 if ok else 0,error=''))
        return d

    def test_template(self):
        self.assertEqual(r.template_of('r001-root-01-d7-both'), 'root-01-d7-both')
        self.assertEqual(r.template_of('warmup-03-root-01-d7-both'), 'root-01-d7-both')
        self.assertEqual(r.template_of('r002-measured-domain-root-01'), 'domain-root-01')

    def test_raw_warmup_and_valid_case(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);self.make(root)
            records, statuses, config, kind=r.load(root,'BASE')
            cases=r.summarize(records,statuses,'BASE')
            self.assertEqual(kind,'raw')
            self.assertEqual(len(records),3)
            self.assertEqual(cases[0]['case_status'],'SUCCESS')
            self.assertEqual(cases[0]['measured_total'],2)
            self.assertAlmostEqual(cases[0]['median_server_bfs_ms'],2100)

    def test_failed_cases_do_not_rank(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);self.make(root,fail=True,staging=True)
            records, statuses, configs, kind=r.load(root,'BASE')
            cases=r.summarize(records,statuses,'BASE')
            self.assertEqual(cases[0]['case_status'],'FAILED')
            self.assertIsNone(cases[0]['median_server_bfs_ms'])
            self.assertEqual(cases[0]['measured_failed'],1)

    def test_pair_root_mismatch(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);a=root/'a';b=root/'b'; self.make(a);self.make(b,root='other')
            ar,as_,_,_=r.load(a,'BASE');br,bs,_,_=r.load(b,'CAND')
            cp=r.compare(r.summarize(ar,as_,'BASE'),r.summarize(br,bs,'CAND'),ar,br)
            self.assertFalse(cp[0]['comparable'])
            self.assertIn('root', cp[0]['reason'])

    def test_pair_change(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);a=root/'a';b=root/'b';self.make(a,seconds=2);self.make(b,seconds=1.5)
            ar,as_,_,_=r.load(a,'BASE');br,bs,_,_=r.load(b,'CAND')
            cp=r.compare(r.summarize(ar,as_,'BASE'),r.summarize(br,bs,'CAND'),ar,br)
            self.assertTrue(cp[0]['comparable'])
            self.assertAlmostEqual(cp[0]['change_percent'],-25.)

    def test_snapshot(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);self.make(root/'a');
            records,st,configs,_=r.load(root/'a','BASE')
            snap=root/'snap';snap.mkdir()
            r.write_csv(snap/'normalized.csv',records,r.FIELDS)
            (snap/'manifest.json').write_text(json.dumps({'case_status':{k[0]+'/'+k[1]:v for k,v in st.items()},'request_configs':configs}))
            reread,rest,_,kind=r.load(snap,'NEW')
            self.assertEqual(kind,'snapshot')
            self.assertEqual(reread[0]['experiment'],'NEW')
            self.assertEqual(rest[('schema-based','01-gpu-100')],'INFERRED')

    def test_full_offline_export(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);self.make(root/'a','schema-based','08-no-cache',seconds=8)
            self.make(root/'a',seconds=2)
            self.make(root/'b','schema-based','08-no-cache',seconds=7)
            self.make(root/'b',seconds=1.8)
            out=root/'report'
            self.assertEqual(r.main(['--baseline',str(root/'a'),'--candidate',str(root/'b'),
                                     '--expected-cases','2','--output',str(out)]),0)
            self.assertTrue((out/'index.html').is_file())
            self.assertTrue((out/'figures'/'04_change_vs_baseline.png').is_file())
            self.assertTrue((out/'normalized.csv').is_file())
            self.assertTrue((out/'deltas.csv').is_file())
            self.assertGreater((out/'index.html').stat().st_size,1000000)
            self.assertTrue('const DATA=' in (out/'index.html').read_text())


class IncludedBaselineTests(unittest.TestCase):
    def test_baseline_snapshot_has_all_40_successful_cases(self):
        src = Path(__file__).resolve().parents[1] / 'baselines' / 'sf1-d7-20261007'
        rows, statuses, config, kind = r.load(src, 'ARCHIVED')
        cases = r.summarize(rows, statuses, 'ARCHIVED')
        self.assertEqual(len(rows), 360)
        self.assertEqual(len(cases), 40)
        self.assertEqual(sum(case['case_status'] == 'SUCCESS' for case in cases), 40)
        self.assertEqual(sum(case['measured_total'] for case in cases), 240)
        self.assertIn('log-recovered', kind)

    def test_baseline_self_compare_produces_no_spurious_regressions(self):
        src = Path(__file__).resolve().parents[1] / 'baselines' / 'sf1-d7-20261007'
        baseline, statuses, _, _ = r.load(src, 'BASE')
        candidate, candidate_statuses, _, _ = r.load(src, 'CAND')
        deltas = r.compare(r.summarize(baseline,statuses,'BASE'),
                           r.summarize(candidate,candidate_statuses,'CAND'), baseline,candidate)
        self.assertEqual(len(deltas), 40)
        self.assertTrue(all(v['comparable'] and v['change_percent'] == 0 for v in deltas))

if __name__=='__main__': unittest.main()
