#!/usr/bin/env python3
"""Real controlling-terminal tests; only Python's standard library is required."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
import uuid

BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "bin/mish").resolve()
ANSI = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")


class Terminal:
    def __init__(self, home):
        self.home = Path(home)
        self.master, self.slave = pty.openpty()
        self.original_modes = termios.tcgetattr(self.slave)
        self.resize(100, 30)
        environment = {
            "HOME": str(self.home),
            "PATH": "/usr/bin:/bin",
            "TERM": "xterm-256color",
            "LC_ALL": "C.UTF-8",
            "USER": "terminal-test",
            "PS1": "READY> ",
            "HISTFILE": str(self.home / "history"),
            "HISTSIZE": "1000",
        }

        def controlling_terminal():
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)

        self.child = subprocess.Popen(
            [str(BINARY), "--norc"],
            stdin=self.slave,
            stdout=self.slave,
            stderr=self.slave,
            cwd=self.home,
            env=environment,
            preexec_fn=controlling_terminal,
            close_fds=True,
        )
        self.output = bytearray()
        self.wait(b"READY> ")
        self.drain()

    def resize(self, columns, rows):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        if hasattr(self, "child"):
            os.kill(self.child.pid, signal.SIGWINCH)

    def send(self, text):
        data = text.encode() if isinstance(text, str) else text
        os.write(self.master, data)

    def read(self, timeout=0.1):
        ready, _, _ = select.select([self.master], [], [], timeout)
        if not ready:
            return b""
        try:
            data = os.read(self.master, 65536)
        except OSError as error:
            if error.errno == errno.EIO:
                return b""
            raise
        self.output.extend(data)
        return data

    def drain(self):
        while self.read(0.03):
            pass

    def wait(self, needle, start=0, timeout=5):
        deadline = time.monotonic() + timeout
        while needle not in ANSI.sub(b"", bytes(self.output[start:])):
            if time.monotonic() > deadline:
                raise AssertionError(
                    f"timed out waiting for {needle!r}; exit={self.child.poll()}; "
                    f"output={bytes(self.output[start:])[-4000:]!r}"
                )
            self.read(0.05)
        return bytes(self.output[start:])

    def line(self, command):
        marker = "DONE_" + uuid.uuid4().hex
        self.drain()
        start = len(self.output)
        self.send(command + "; echo " + marker + "\r")
        output = self.wait(b"\r\n" + marker.encode() + b"\r\n", start)
        self.drain()
        return ANSI.sub(b"", output)

    def edited(self, keys, expected):
        self.drain()
        start = len(self.output)
        self.send(keys)
        output = self.wait(b"\r\n" + expected.encode() + b"\r\n", start)
        self.drain()
        return output

    def close(self):
        if self.child.poll() is None:
            self.send(b"\x03")
            self.drain()
            self.send(b"exit\r")
            deadline = time.monotonic() + 3
            while self.child.poll() is None and time.monotonic() < deadline:
                self.read(0.05)
            if self.child.poll() is None:
                os.killpg(self.child.pid, signal.SIGKILL)
        self.child.wait(timeout=3)
        modes = termios.tcgetattr(self.slave)
        os.close(self.master)
        os.close(self.slave)
        return modes


class InteractiveTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mish-terminal-")
        self.terminal = Terminal(self.directory.name)

    def tearDown(self):
        if self.terminal is not None:
            self.terminal.close()
        self.directory.cleanup()

    def test_utf8_arrows_and_backspace(self):
        self.terminal.edited("echo 界éX\x1b[D\x7fß\r", "界ßX")

    def test_home_end_delete_and_insert(self):
        self.terminal.edited(b"echo ABZ\x1b[H\x1b[1;5C\x1b[C\x1b[C\x1b[3~\x1b[F\x7fC\r", "AC")

    def test_kill_yank_undo_redo(self):
        self.terminal.edited(b"echo WRONG\x17right\x1f\x1b\x1fr\r", "rightr")
        self.terminal.edited(b"echo keep\x15\x19\r", "keep")

    def test_ctrl_c_cancels_and_shell_remains_alive(self):
        self.terminal.send(b"echo SHOULD_NOT_RUN\x03")
        self.terminal.wait(b"^C")
        self.assertIsNone(self.terminal.child.poll())
        result = self.terminal.line("echo $?")
        self.assertIn(b"\r\n130\r\n", result)
        self.assertNotIn(b"\r\nSHOULD_NOT_RUN\r\n", self.terminal.output)

    def test_history_navigation_and_reverse_search(self):
        self.terminal.edited(b"echo SEARCH_SEED\r", "SEARCH_SEED")
        self.terminal.edited(b"\x1b[A\r", "SEARCH_SEED")
        self.terminal.edited(b"echo OTHER\r", "OTHER")
        self.terminal.edited(b"\x12SEARCH_SEED\r", "SEARCH_SEED")

    def test_bracketed_paste_does_not_execute_before_enter(self):
        start = len(self.terminal.output)
        self.terminal.send(b"\x1b[200~echo PASTE_ONE\necho PASTE_TWO\x1b[201~")
        deadline = time.monotonic() + 0.25
        while time.monotonic() < deadline:
            self.terminal.read(0.03)
        pending = bytes(self.terminal.output[start:])
        self.assertNotIn(b"\r\nPASTE_ONE\r\n", pending)
        self.terminal.send(b"\r")
        self.terminal.wait(b"\r\nPASTE_ONE\r\n", start)
        self.terminal.wait(b"\r\nPASTE_TWO\r\n", start)

    def test_command_filename_variable_and_programmable_completion(self):
        (Path(self.directory.name) / "space name").write_text("FILE_RESULT\n")
        self.terminal.edited(b"ech\tCOMMAND_RESULT\r", "COMMAND_RESULT")
        self.terminal.edited(b"cat 'sp\t\r", "FILE_RESULT")
        self.terminal.line("COMPLETION_VALUE=VARIABLE_RESULT")
        self.terminal.edited(b"echo $COMPLETION_V\t\r", "VARIABLE_RESULT")
        self.terminal.line("alias picker=echo; complete -W 'choice_one choice_two' picker")
        self.terminal.edited(b"picker choice_o\t\r", "choice_one")

    def test_custom_binding_and_prompt_escape(self):
        self.terminal.line("bind '\\C-x: beginning-of-line'")
        self.terminal.edited(b"echo TAIL\x18echo \r", "echo TAIL")
        result = self.terminal.line("prompt --preview '\\u|\\W|\\s|\\?|$(USER)'")
        self.assertIn(b"terminal-test|", result)
        self.assertIn(b"|mish|", result)
        self.assertIn(b"|terminal-test\r\n", result)

    def test_custom_multibyte_chord_and_vi_mode(self):
        self.terminal.line("bind '\\C-x\\C-e: end-of-line'")
        self.terminal.edited(b"echo CHORD\x01\x18\x05X\r", "CHORDX")
        self.terminal.line("set -o vi")
        self.terminal.send(b"echo VI_BAD")
        self.terminal.send(b"\x1b")
        time.sleep(0.12)
        self.terminal.edited(b"b3xA_GOOD\r", "BAD_GOOD")

    def test_bracketed_paste_normalizes_crlf_and_ignores_nul(self):
        start = len(self.terminal.output)
        self.terminal.send(b"\x1b[200~echo CRLF_ONE\r\necho CRLF_\x00TWO\x1b[201~\r")
        self.terminal.wait(b"\r\nCRLF_ONE\r\n", start)
        self.terminal.wait(b"\r\nCRLF_TWO\r\n", start)

    def test_variable_directory_completion_and_single_quoted_history(self):
        (Path(self.directory.name) / "variable path").write_text("DIRECTORY_RESULT\n")
        self.terminal.edited(b"cat $HOME/variable\\ p\t\r", "DIRECTORY_RESULT")
        self.terminal.edited(b"echo '!!'\r", "!!")
        self.terminal.edited(b"echo SEED\r", "SEED")
        self.terminal.edited(b'echo "x\'!!"\r', "x'echo SEED")

    def test_resize_and_long_utf8_line(self):
        self.terminal.resize(24, 10)
        value = "é界" * 30
        self.terminal.edited("echo " + value + "\r", value)
        self.terminal.resize(80, 24)
        self.terminal.edited(b"echo AFTER_RESIZE\r", "AFTER_RESIZE")

    def test_history_expansion_words_substitution_and_print_only(self):
        self.terminal.edited(b"echo EXPAND_ONE two\r", "EXPAND_ONE two")
        self.terminal.edited(b"!!\r", "EXPAND_ONE two")
        self.terminal.edited(b"echo !$\r", "two")
        self.terminal.edited(b"^two^THREE^\r", "THREE")
        start = len(self.terminal.output)
        self.terminal.send(b"!echo:p\r")
        result = self.terminal.wait(b"\r\necho THREE\r\n", start)
        self.assertNotIn(b"\r\nTHREE\r\n", result)

    def test_history_persistence_mode_and_terminal_restoration(self):
        self.terminal.edited(b"echo PERSISTED_ENTRY\r", "PERSISTED_ENTRY")
        original = self.terminal.original_modes
        modes = self.terminal.close()
        self.terminal = None
        self.assertEqual(modes, original)
        history = Path(self.directory.name) / "history"
        self.assertEqual(history.stat().st_mode & 0o777, 0o600)
        self.assertIn(b"PERSISTED_ENTRY", history.read_bytes())
        self.terminal = Terminal(self.directory.name)
        result = self.terminal.line("history")
        self.assertIn(b"echo PERSISTED_ENTRY", result)

    def test_ctrl_d_exits_empty_line_and_restores_modes(self):
        self.terminal.send(b"\x04")
        self.terminal.child.wait(timeout=5)
        modes = self.terminal.close()
        original = self.terminal.original_modes
        self.terminal = None
        self.assertEqual(modes, original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
