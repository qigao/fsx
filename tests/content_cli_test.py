"""Formal CLI content integration using the actual packaged ugrep."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FSX, UGREP, BFS = sys.argv[1:4]
del sys.argv[1:4]


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
        self.assertEqual(rows[-1]['parallel_files'], 1)

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

    def test_parallel_scope_reports_exact_global_results(self):
        for i in range(20):
            (self.root / f'item{i}.txt').write_text('needle needle\n')
        self.index()
        result = self.run_fsx('content', self.db, UGREP, '--parallel-files', '2',
                              '--literal', 'needle', '--', 'file:')
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(rows), 41)
        self.assertEqual(len({r['path'] for r in rows[:-1]}), 20)
        self.assertEqual(rows[-1]['parallel_files'], 2)
        self.assertEqual(rows[-1]['records'], 40)
        self.assertEqual(rows[-1]['candidates'], 20)
        self.assertTrue(rows[-1]['complete'])

    def test_invalid_parallelism_is_rejected_before_opening_database(self):
        for value in ('0', '3', '-1', '', 'two'):
            result = self.run_fsx('content', self.db, UGREP, '--parallel-files',
                                  value, '--literal', 'x', '--', 'file:')
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertFalse(self.db.exists())

    @unittest.skipUnless(sys.platform in ('linux', 'darwin'), 'BFS_LIVE requires POSIX')
    def test_parallel_live_search_reports_exact_global_results(self):
        for i in range(20):
            (self.root / f'item{i}.txt').write_text('needle\n')
        result = self.run_fsx('content-live', BFS, UGREP, '--parallel-files', '2',
                              '--literal', 'needle', '--', self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(rows), 21)
        self.assertEqual(rows[-1]['parallel_files'], 2)
        self.assertEqual(rows[-1]['candidates'], 20)
        self.assertTrue(rows[-1]['complete'])
        self.assertFalse(self.db.exists())

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

    @unittest.skipUnless(sys.platform in ('linux', 'darwin'), 'BFS_LIVE requires POSIX')
    def test_live_search_needs_no_database_and_preserves_path_bytes(self):
        name = '-e 猫\n.txt'
        (self.root / name).write_text('needle needle\n', encoding='utf-8')
        (self.root / '.hidden').write_text('needle\n')
        (self.root / 'link').symlink_to(self.root, target_is_directory=True)
        result = self.run_fsx('content-live', BFS, UGREP, '--literal', 'needle',
                              '--', self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(rows), 4)
        self.assertIn(str(self.root / name), [r['path'] for r in rows[:-1]])
        self.assertTrue(all(r['metadata_epoch'] == 0 for r in rows[:-1]))
        self.assertEqual(rows[-1]['source'], 'bfs_live')
        self.assertTrue(rows[-1]['complete'])
        self.assertTrue(rows[-1]['candidates_known'])
        self.assertEqual(rows[-1]['candidates'], 2)
        self.assertFalse(self.db.exists())

    @unittest.skipUnless(sys.platform in ('linux', 'darwin'), 'BFS_LIVE requires POSIX')
    def test_live_missing_bfs_and_invalid_root_fail_explicitly(self):
        result = self.run_fsx('content-live', '/fsx-missing-bfs', UGREP,
                              '--literal', 'x', '--', self.root)
        self.assertEqual(result.returncode, 1)
        summary = json.loads(result.stdout)
        self.assertFalse(summary['complete'])
        self.assertFalse(summary['candidates_known'])
        result = self.run_fsx('content-live', BFS, UGREP, '--literal', 'x',
                              '--', self.root / 'missing')
        self.assertEqual(result.returncode, 1)
        self.assertFalse(json.loads(result.stdout)['complete'])

    @unittest.skipUnless(sys.platform in ('linux', 'darwin'), 'BFS_LIVE requires POSIX')
    def test_live_binary_and_empty_directory_have_distinct_completeness(self):
        result = self.run_fsx('content-live', BFS, UGREP, '--literal', 'x',
                              '--', self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)['complete'])
        (self.root / 'binary').write_bytes(b'needle\0')
        result = self.run_fsx('content-live', BFS, UGREP, '--literal', 'needle',
                              '--', self.root)
        self.assertEqual(result.returncode, 3, result.stderr)
        summary = json.loads(result.stdout)
        self.assertFalse(summary['complete'])
        self.assertEqual(summary['skipped'], 1)


if __name__ == '__main__':
    unittest.main()
