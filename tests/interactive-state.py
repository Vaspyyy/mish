#!/usr/bin/env python3
"""Nonterminal tests for history storage, binding definitions and prompt state."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor

BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "bin/mish").resolve()


def quoted(value):
    return "'" + str(value).replace("'", "'\\''") + "'"


class InteractiveStateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mish-state-")
        self.home = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def run_shell(self, source):
        return subprocess.run(
            [str(BINARY), "--norc", "-c", source],
            cwd=self.home,
            env={"HOME": str(self.home), "PATH": "/usr/bin:/bin", "LC_ALL": "C.UTF-8"},
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
        )

    def test_history_roundtrip_multiline_delete_clear(self):
        file = quoted(self.home / "saved history")
        output = self.run_shell(
            "history -s 'echo first'; history -s 'echo second\necho third'; "
            f"history -w {file}; history -c; history -r {file}; history -d 1; history"
        )
        self.assertEqual(output.returncode, 0, output.stderr)
        self.assertIn(b"echo second\necho third", output.stdout)
        self.assertNotIn(b"echo first", output.stdout)
        self.assertEqual((self.home / "saved history").stat().st_mode & 0o777, 0o600)

    def test_history_append_does_not_duplicate_saved_entries(self):
        file = quoted(self.home / "history")
        output = self.run_shell(
            f"history -s first; history -w {file}; history -s second; history -a {file}; "
            f"history -a {file}; history -c; history -r {file}; history"
        )
        self.assertEqual(output.returncode, 0, output.stderr)
        self.assertEqual(output.stdout.count(b"first"), 1)
        self.assertEqual(output.stdout.count(b"second"), 1)

    def test_concurrent_history_append_preserves_each_session(self):
        file = quoted(self.home / "shared")
        def append(index):
            return self.run_shell(f"history -s SESSION_{index:02}; history -a {file}")
        with ThreadPoolExecutor(max_workers=8) as executor:
            outputs = list(executor.map(append, range(24)))
        for output in outputs:
            self.assertEqual(output.returncode, 0, output.stderr)
        output = self.run_shell(f"history -r {file}; history")
        self.assertEqual(output.returncode, 0, output.stderr)
        for index in range(24):
            self.assertEqual(output.stdout.count(f"SESSION_{index:02}".encode()), 1)

    def test_exporting_backup_does_not_suppress_primary_append(self):
        primary = quoted(self.home / "primary")
        backup = quoted(self.home / "backup")
        output = self.run_shell(
            f"history -s first; history -w {primary}; history -s second; "
            f"history -w {backup}; history -a {primary}; history -c; "
            f"history -r {primary}; history"
        )
        self.assertEqual(output.returncode, 0, output.stderr)
        self.assertEqual(output.stdout.count(b"first"), 1)
        self.assertEqual(output.stdout.count(b"second"), 1)

    def test_history_imports_plain_text_with_private_mode(self):
        file = self.home / "legacy"
        file.write_text("echo first\necho second\n")
        file.chmod(0o600)
        output = self.run_shell(f"history -r {quoted(file)}; history")
        self.assertEqual(output.returncode, 0, output.stderr)
        self.assertIn(b"echo first", output.stdout)
        self.assertIn(b"echo second", output.stdout)

    def test_history_refuses_symlink_and_public_file(self):
        target = self.home / "target"
        target.write_text("DO_NOT_CHANGE\n")
        target.chmod(0o600)
        link = self.home / "link"
        link.symlink_to(target)
        output = self.run_shell(f"history -s new; history -w {quoted(link)}")
        self.assertEqual(output.returncode, 1)
        self.assertEqual(target.read_text(), "DO_NOT_CHANGE\n")
        target.chmod(0o644)
        output = self.run_shell(f"history -r {quoted(target)}")
        self.assertEqual(output.returncode, 1)
        self.assertTrue(output.stderr)

    def test_history_controls_and_limits(self):
        output = self.run_shell(
            "HISTSIZE=2; HISTCONTROL=ignoreboth; history -s first; "
            "history -s first; history -s ' private'; history -s second; "
            "history -s third; history"
        )
        self.assertEqual(output.returncode, 0, output.stderr)
        self.assertNotIn(b"first", output.stdout)
        self.assertNotIn(b"private", output.stdout)
        self.assertIn(b"second", output.stdout)
        self.assertIn(b"third", output.stdout)

    def test_history_rejects_corrupt_framed_file_without_importing_partial_records(self):
        file = self.home / "corrupt"
        file.write_bytes(b"# mish history v1\n999:truncated\n")
        file.chmod(0o600)
        output = self.run_shell(f"history -r {quoted(file)}")
        self.assertEqual(output.returncode, 1)
        self.assertTrue(output.stderr)

    def test_bind_listing_replacement_and_validation(self):
        output = self.run_shell("bind '\\C-x: beginning-of-line'; bind -p; bind -l")
        self.assertEqual(output.returncode, 0, output.stderr)
        self.assertIn(b"\\C-x", output.stdout)
        self.assertIn(b"beginning-of-line", output.stdout)
        output = self.run_shell("bind '\\C-x: invented-action'")
        self.assertEqual(output.returncode, 2)

    def test_completion_configuration_is_reusable_and_removable(self):
        output = self.run_shell("complete -W 'one two' -P pre -S post demo; complete -p demo")
        self.assertEqual(output.returncode, 0, output.stderr)
        self.assertIn(b"complete -W", output.stdout)
        self.assertIn(b"pre", output.stdout)
        self.assertIn(b"post", output.stdout)
        output = self.run_shell("complete -A directory demo; complete -r demo; complete -p demo")
        self.assertEqual(output.returncode, 1)
        output = self.run_shell("complete -A unknown demo")
        self.assertEqual(output.returncode, 2)

    def test_prompt_preview_literal_quotes_variables_and_escapes(self):
        output = self.run_shell("USER=tester; prompt --preview '\\u|$(USER)|\\s|\\?|\\D{%Y}|\\e[31mred\\e[0m'")
        self.assertEqual(output.returncode, 0, output.stderr)
        self.assertTrue(output.stdout.startswith(b"tester|tester|mish|0|"), output.stdout)
        self.assertIn(b"\x1b[31mred\x1b[0m", output.stdout)
        output = self.run_shell("prompt 'literal \"quotes\"'; prompt --preview")
        self.assertEqual(output.returncode, 0, output.stderr)
        self.assertEqual(output.stdout, b'literal "quotes"\n')


if __name__ == "__main__":
    unittest.main(verbosity=2)
