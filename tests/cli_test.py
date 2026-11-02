"""Formal end-to-end tests for the fsx command-line product."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())

class FileSearchCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fsx-cli-", dir=Path.cwd())
        self.base = Path(self.temporary.name)
        self.root = self.base / "tree"
        self.root.mkdir()
        self.database = self.base / "index.db"

    def tearDown(self):
        self.temporary.cleanup()

    def run_cli(self, *args):
        return subprocess.run([EXECUTABLE, *map(str, args)], capture_output=True,
                              encoding="utf-8", timeout=20)

    def test_index_search_persistence_and_unicode_output(self):
        (self.root / "Straße report.txt").write_text("content", encoding="utf-8")
        result = self.run_cli("index", self.database, self.root)
        self.assertEqual(result.returncode, 0, result.stderr + f" parent_exists={self.database.parent.is_dir()} root_exists={self.root.is_dir()}")
        result = self.run_cli("search", self.database, "--json", "--", 'STRASSE ext:txt')
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["total"], 1)
        self.assertTrue(output["coverage_complete"])
        self.assertTrue(output["entries"][0]["path"].endswith("Straße report.txt"))
        info = self.run_cli("info", self.database)
        self.assertEqual(info.returncode, 0, info.stderr)
        self.assertIn("revision=1", info.stdout)

    def test_search_does_not_create_an_unknown_database(self):
        result = self.run_cli("search", self.database, "--", "file")
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.database.exists())
        self.database.touch()
        result = self.run_cli("search", self.database, "--", "file")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.database.stat().st_size, 0)

    def test_invalid_query_returns_diagnostic_and_nonzero_exit(self):
        (self.root / "file.txt").write_text("x", encoding="utf-8")
        self.assertEqual(self.run_cli("index", self.database, self.root).returncode, 0)
        result = self.run_cli("search", self.database, "--", 'regex:"["')
        self.assertEqual(result.returncode, 1)
        self.assertIn("query byte", result.stderr)
        self.assertIn("invalid", result.stderr)

if __name__ == "__main__":
    unittest.main()
