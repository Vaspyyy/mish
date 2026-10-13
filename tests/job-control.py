#!/usr/bin/env python3
"""Foreground groups, suspended jobs and interrupts on a controlling terminal."""
import importlib.util
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest

# Reuse the tested terminal driver without importing its unittest cases.
spec = importlib.util.spec_from_file_location('mish_terminal', Path(__file__).with_name('interactive-pty.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
Terminal = module.Terminal
ANSI = module.ANSI

class JobControl(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='mish-jobs-')
        self.terminal = Terminal(self.directory.name)

    def tearDown(self):
        self.terminal.close()
        self.directory.cleanup()

    def launch(self, command):
        self.terminal.drain()
        start = len(self.terminal.output)
        self.terminal.send(command+'\r')
        time.sleep(.12)
        self.terminal.drain()
        return start

    def test_foreground_read_and_interrupt(self):
        start = self.launch('/bin/cat')
        self.terminal.send('foreground-input\r')
        self.terminal.wait(b'foreground-input\r\n', start)
        self.terminal.send(b'\x03')
        self.terminal.wait(b'READY> ', start)
        self.assertIn(b'130\r\n', self.terminal.line('echo $?'))

    def test_pipeline_interrupt_group(self):
        start = self.launch('/bin/cat | /bin/cat')
        self.terminal.send(b'\x03')
        self.terminal.wait(b'READY> ', start)
        self.assertIn(b'130\r\n', self.terminal.line('echo $?'))

    def test_stop_background_foreground(self):
        start = self.launch('/bin/sleep 30')
        self.terminal.send(b'\x1a')
        self.terminal.wait(b'Stopped', start)
        self.terminal.wait(b'READY> ', start)
        output = self.terminal.line('jobs; bg; jobs')
        self.assertIn(b'Stopped', output)
        self.assertIn(b'Running', output)
        start = self.launch('fg')
        self.terminal.send(b'\x03')
        self.terminal.wait(b'READY> ', start)
        self.assertIn(b'130\r\n', self.terminal.line('echo $?'))

    def test_subshell_job_group(self):
        start = self.launch('(/bin/sleep 30)')
        self.terminal.send(b'\x1a')
        self.terminal.wait(b'Stopped', start)
        self.terminal.wait(b'READY> ', start)
        start = self.launch('fg')
        self.terminal.send(b'\x03')
        self.terminal.wait(b'READY> ', start)
        self.assertIn(b'130\r\n', self.terminal.line('echo $?'))

    def test_termination_restores_terminal(self):
        os.kill(self.terminal.child.pid, signal.SIGTERM)
        self.assertEqual(self.terminal.child.wait(timeout=5), 143)
        import termios
        self.assertEqual(termios.tcgetattr(self.terminal.slave), self.terminal.original_modes)

if __name__ == '__main__':
    unittest.main(verbosity=2)
