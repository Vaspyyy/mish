#!/usr/bin/env python3
"""Run actual binary, parser ownership/math, builtins and pseudo-terminal suites."""
from pathlib import Path
import os
import subprocess
import sys
root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1] if len(sys.argv) > 1 else root/'bin/mish').resolve()
suites = [root/'tests/runtime.py', root/'tests/job-control.py', *sorted((root/'tests').glob('parser-*.py')),
          *sorted((root/'tests').glob('builtins-*.py')), *sorted((root/'tests').glob('interactive-*.py'))]
for suite in suites:
    print(f'Running {suite.name}', flush=True)
    arguments = [] if suite.name.startswith('parser-') or suite.name == 'builtins-runtime.py' else [str(binary)]
    environment = os.environ.copy()
    environment['MISH_BIN'] = str(binary)
    subprocess.run([sys.executable, str(suite), *arguments], cwd=root, check=True, env=environment)
