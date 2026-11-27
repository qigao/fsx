"""Formal CLI coverage for the embedded discovery and matching backends."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FSX = sys.argv.pop(1)


class NativeContentCli(unittest.TestCase):
    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix='fsx-native-cli-', dir=Path.cwd())
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

    def rows(self, result, code=0):
        self.assertEqual(result.returncode, code, result.stderr)
        return [json.loads(row) for row in result.stdout.splitlines()]

    def test_indexed_content_needs_no_tool_paths(self):
        (self.root / '猫.txt').write_text('猫 needle needle\n', encoding='utf-8')
        self.index()
        rows = self.rows(self.run_fsx('content', self.db, '--literal', 'needle',
                                      '--', 'file:'))
        self.assertEqual([row['offset'] for row in rows[:-1]], [4, 11])
        self.assertEqual(rows[0]['text'], '猫 needle needle')
        self.assertTrue(rows[0]['path'].endswith('猫.txt'))
        self.assertEqual(rows[-1]['source'], 'index_scope')
        self.assertTrue(rows[-1]['complete'])
        self.assertTrue(rows[-1]['candidates_known'])

    def test_pcre2_multiline_and_literal_semantics(self):
        (self.root / 'text.txt').write_text('prefixone\ntwo\naXb\n', encoding='utf-8')
        self.index()
        rows = self.rows(self.run_fsx('content', self.db, '--regex',
                                      r'(?<=prefix)one\ntwo', '--', 'file:'))
        self.assertEqual(rows[0]['text'], 'prefixone\ntwo')
        self.assertEqual(rows[0]['offset'], 6)
        rows = self.rows(self.run_fsx('content', self.db, '--literal', 'a.b',
                                      '--', 'file:'))
        self.assertEqual(rows[-1]['records'], 0)
        self.assertTrue(rows[-1]['complete'])

    def test_parallel_scope_uses_the_entire_metadata_selection(self):
        for i in range(105):
            (self.root / f'item{i}.txt').write_text('needle needle\n')
        (self.root / 'excluded.bin').write_text('needle\n')
        self.index()
        rows = self.rows(self.run_fsx('content', self.db, '--parallel-files', '2',
                                      '--literal', 'needle', '--', 'file: ext:txt'))
        self.assertEqual(len(rows), 211)
        self.assertEqual(rows[-1]['parallel_files'], 2)
        self.assertEqual(rows[-1]['candidates'], 105)
        self.assertTrue(rows[-1]['complete'])

    def test_invalid_regex_fails_even_without_candidates(self):
        self.index()
        rows = self.rows(self.run_fsx('content', self.db, '--regex', '[',
                                      '--', 'file:'), code=1)
        self.assertFalse(rows[-1]['complete'])
        self.assertFalse(rows[-1]['candidates_known'])
        self.assertEqual(rows[-1]['records'], 0)

    def test_native_binary_coverage_is_explicit(self):
        (self.root / 'binary').write_bytes(b'needle\0')
        self.index()
        rows = self.rows(self.run_fsx('content', self.db, '--literal', 'needle',
                                      '--', 'file:'), code=3)
        self.assertEqual(rows[-1]['skipped'], 1)
        self.assertFalse(rows[-1]['complete'])

    def test_invalid_options_do_not_open_or_create_a_database(self):
        for args in [(), ('--literal',), ('--unknown', 'x', '--', 'file:'),
                     ('--parallel-files', '3', '--literal', 'x', '--', 'file:')]:
            result = self.run_fsx('content', self.db, *args)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertFalse(self.db.exists())

    @unittest.skipUnless(sys.platform in ('linux', 'darwin'), 'bftw requires POSIX')
    def test_live_discovery_needs_no_database_or_tool_paths(self):
        name = '-e 猫\n.txt'
        (self.root / name).write_text('needle needle\n', encoding='utf-8')
        (self.root / '.hidden').write_text('needle\n')
        (self.root / 'link').symlink_to(self.root, target_is_directory=True)
        rows = self.rows(self.run_fsx('content-live', '--parallel-files', '2',
                                      '--literal', 'needle', '--', self.root))
        self.assertEqual(len(rows), 4)
        self.assertIn(str(self.root / name), [r['path'] for r in rows[:-1]])
        self.assertTrue(all(r['metadata_epoch'] == 0 for r in rows[:-1]))
        self.assertEqual(rows[-1]['candidates'], 2)
        self.assertTrue(rows[-1]['complete'])
        self.assertFalse(self.db.exists())

    @unittest.skipUnless(sys.platform in ('linux', 'darwin'), 'bftw requires POSIX')
    def test_live_empty_and_missing_directories_are_distinct(self):
        rows = self.rows(self.run_fsx('content-live', '--literal', 'x',
                                      '--', self.root))
        self.assertTrue(rows[-1]['complete'])
        self.assertEqual(rows[-1]['candidates'], 0)
        rows = self.rows(self.run_fsx('content-live', '--literal', 'x',
                                      '--', self.root / 'missing'), code=1)
        self.assertFalse(rows[-1]['complete'])
        self.assertFalse(rows[-1]['candidates_known'])

    @unittest.skipUnless(sys.platform == 'win32', 'Windows backend contract')
    def test_windows_live_discovery_is_explicitly_unsupported(self):
        result = self.run_fsx('content-live', '--literal', 'x', '--', self.root)
        self.assertEqual(result.returncode, 1)
        self.assertIn('unsupported', result.stderr)
        self.assertFalse(self.db.exists())


if __name__ == '__main__':
    unittest.main()
