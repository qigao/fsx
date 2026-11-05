"""Formal CLI content integration using the actual packaged ugrep."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FSX, UGREP = sys.argv[1:3]
del sys.argv[1:3]


class ContentCli(unittest.TestCase):
    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix='fsx-content-cli-', dir=Path.cwd())
        self.root = Path(self.fixture.name) / 'tree'
        self.root.mkdir()
        self.db = Path(self.fixture.name) / 'index.db'

    def tearDown(self):
        self.fixture.cleanup()

    def run_fsx(self, *args):
        return subprocess.run([FSX, *map(str, args)], capture_output=True,
                              encoding='utf-8', timeout=20)

    def index(self):
        result = self.run_fsx('index', self.db, self.root)
        self.assertEqual(result.returncode, 0, result.stderr)

    def search(self, mode, pattern):
        return self.run_fsx('content', self.db, UGREP, mode, pattern, '--', 'file:')

    def test_real_matches_are_records_with_live_read_summary(self):
        (self.root / '猫.txt').write_text('needle needle\n', encoding='utf-8')
        self.index()
        result = self.search('--literal', 'needle')
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([r['offset'] for r in rows[:-1]], [0, 7])
        self.assertTrue(rows[0]['path'].endswith('猫.txt'))
        self.assertEqual(rows[-1]['semantics'], 'live_read')
        self.assertTrue(rows[-1]['complete'])
        self.assertEqual(rows[-1]['records'], 2)
        self.assertTrue(rows[-1]['candidates_known'])

    def test_full_scope_uses_metadata_predicates(self):
        for i in range(105):
            (self.root / f'item{i}.txt').write_text('needle\n')
        (self.root / 'excluded.bin').write_text('needle\n')
        self.index()
        result = self.run_fsx('content', self.db, UGREP, '--literal', 'needle',
                              '--', 'file: ext:txt size:>0')
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(rows), 106)
        self.assertEqual(rows[-1]['candidates'], 105)
        self.assertTrue(rows[-1]['candidates_known'])
        self.assertTrue(rows[-1]['complete'])

    def test_invalid_scope_has_no_confirmed_candidate_total(self):
        self.index()
        result = self.run_fsx('content', self.db, UGREP, '--literal', 'needle',
                              '--', 'regex:"["')
        self.assertEqual(result.returncode, 1)
        summary = json.loads(result.stdout)
        self.assertFalse(summary['candidates_known'])
        self.assertFalse(summary['complete'])
        self.assertEqual(summary['records'], 0)

    def test_no_match_and_pcre2_error_are_distinct(self):
        (self.root / 'text.txt').write_text('aXb\n')
        self.index()
        result = self.search('--literal', 'a.b')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['records'], 0)
        result = self.search('--regex', '[')
        self.assertEqual(result.returncode, 1)
        self.assertIn('io', result.stderr)

    def test_binary_coverage_is_explicit(self):
        (self.root / 'binary').write_bytes(b'needle\0')
        self.index()
        result = self.search('--literal', 'needle')
        self.assertEqual(result.returncode, 3, result.stderr)
        summary = json.loads(result.stdout)
        self.assertFalse(summary['complete'])
        self.assertEqual(summary['skipped'], 1)

    def test_invalid_arguments_do_not_create_database(self):
        for args in [(), (UGREP,), (UGREP, '--unknown', 'x', '--', 'file:')]:
            result = self.run_fsx('content', self.db, *args)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertFalse(self.db.exists())


if __name__ == '__main__':
    unittest.main()
