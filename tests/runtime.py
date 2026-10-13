#!/usr/bin/env python3
"""Actual-process regression coverage for execution, expansion and job tracking."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else 'bin/mish').resolve()
sys.argv = [sys.argv[0]]

class Runtime(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='mish-runtime-')
        self.root = Path(self.directory.name)
        self.env = {'HOME': str(self.root), 'PATH': '/usr/bin:/bin', 'LC_ALL': 'C',
                    'HISTFILE': str(self.root / 'history'), 'PS1': 'PROMPT> '}

    def tearDown(self):
        self.directory.cleanup()

    def run_shell(self, source, *, code=0, output=None, stdin=None, args=()):
        invocation = [str(BINARY), '--norc']
        if stdin is None:
            invocation += ['-c', source, 'mish-test', *args]
            data = ''
        else:
            invocation += ['-s', *args]
            data = stdin
        result = subprocess.run(invocation, input=data, text=True, capture_output=True,
                                cwd=self.root, env=self.env, timeout=10)
        self.assertEqual(result.returncode, code, (source, result.stdout, result.stderr))
        self.assertNotIn('runtime error:', result.stderr)
        self.assertNotIn('AddressSanitizer', result.stderr)
        if output is not None:
            self.assertEqual(result.stdout, output, (source, result.stderr))
        return result

    def test_quotes_and_parameters(self):
        self.run_shell("x='a b'; printf '<%s>\\n' \"$x\" '' '\\literal' '世界'", output='<a b>\n<>\n<\\literal>\n<世界>\n')
        self.run_shell('x=abc; echo ${#x} ${x:-missing} ${missing:-default}; echo $(x)', output='3 abc default\nabc\n')
        self.run_shell('x=abcdef; echo ${x#abc} ${x%def} ${x:1:3} ${x/abc/xyz} ${x^^}', output='def abc bcd xyzdef ABCDEF\n')
        self.run_shell('set -- "a b" "" c; printf "<%s>\\n" "$@"; echo $#', output='<a b>\n<>\n<c>\n3\n')

    def test_local_export_assignment_scope(self):
        self.run_shell("x=outer; x=inner echo \"$x\"; echo $x; export x; /bin/sh -c 'echo $x'", output='inner\nouter\nouter\n')
        self.run_shell('x=outer; f() { local x=inner; echo $x; }; f; echo $x', output='inner\nouter\n')
        self.run_shell('readonly x=one; x=two; echo $x', output='one\n')

    def test_functions_and_control(self):
        self.run_shell('f() { echo "$1"; return 7; echo no; }; f hello; echo $?', output='hello\n7\n')
        self.run_shell('if false; then echo bad; elif true; then echo yes; else echo bad; fi', output='yes\n')
        self.run_shell('i=0; while test $i -lt 3; do echo $i; let i++; done', output='0\n1\n2\n')
        self.run_shell('for x in a b c; do test "$x" = b && continue; echo $x; done', output='a\nc\n')
        self.run_shell('for x in a b; do for y in 1 2; do echo $x$y; break 2; done; done', output='a1\n')
        self.run_shell('case hello in x*) echo bad;; h*) echo yes;; *) echo bad;; esac', output='yes\n')
        self.run_shell('for ((i=0;i<3;i++)); do echo $i; done', output='0\n1\n2\n')
        self.run_shell('((x=2+3)); echo $x', output='5\n')

    def test_logic_status_and_errexit(self):
        self.run_shell('false && echo no; true || echo no; ! false && echo yes', output='yes\n')
        self.run_shell('/bin/sh -c "exit 9"; echo $?', output='9\n')
        self.run_shell('set -e; false; echo no', code=1, output='')
        self.run_shell('set -e; false || echo handled; if false; then echo bad; fi; echo okay', output='handled\nokay\n')
        self.run_shell('set -u; echo $ABSENT', code=1)
        self.run_shell('set -e; false && echo no; ! true; echo survived', output='survived\n')
        self.run_shell("trap 'echo error' ERR; false", code=1, output='error\n')

    def test_pipelines_and_large_output(self):
        self.run_shell("printf 'hello\\nworld\\n' | /usr/bin/tr a-z A-Z | /bin/cat", output='HELLO\nWORLD\n')
        self.run_shell('/bin/sh -c "exit 4" | true; echo $?', output='0\n')
        self.run_shell('set -o pipefail; /bin/sh -c "exit 4" | true; echo $?', output='4\n')
        self.run_shell('/usr/bin/head -c 262144 /dev/zero | /usr/bin/wc -c', output='262144\n')

    def test_redirect_restore_and_fd_order(self):
        self.run_shell('echo first > out; echo second >> out; cat < out; echo visible', output='first\nsecond\nvisible\n')
        self.run_shell('{ echo grouped; echo two; } > out; cat out', output='grouped\ntwo\n')
        result = self.run_shell("/bin/sh -c 'echo out; echo err >&2' > out 2>&1; cat out", output='out\nerr\n')
        self.assertEqual(result.stderr, '')
        self.run_shell('exec 3> descriptor; echo saved >&3; exec 3>&-; cat descriptor', output='saved\n')
        self.run_shell('set -o noclobber; echo one > f; echo two > f; echo forced >| f; cat f', output='forced\n')

    def test_heredocs_and_capture(self):
        self.run_shell('x=value; cat <<EOF\nhello $x\nEOF\n', output='hello value\n')
        self.run_shell("x=value; cat <<'EOF'\nhello $x\nEOF\n", output='hello $x\n')
        self.run_shell('cat <<-EOF\n\ttab\n\tEOF\n', output='tab\n')
        self.run_shell('cat <<< "here string"', output='here string\n')
        self.run_shell('x=outer; echo $[x=inner; echo $x]; echo $x', output='inner\nouter\n')
        self.run_shell('echo "$[echo \"a ] b\"]"', output='a ] b\n')
        self.run_shell('echo "$[cat <<EOF\n] ) \' \"\nEOF\n]"', output='] ) \' "\n')
        result = self.run_shell("echo $[/bin/sh -c 'echo captured; echo warning >&2; exit 2']", output='captured\n')
        self.assertEqual(result.stderr, 'warning\n')
        self.run_shell('x=$[/usr/bin/head -c 262144 /dev/zero | /usr/bin/tr "\\000" x]; echo ${#x}', output='262144\n')

    def test_aliases_and_subshell_isolation(self):
        self.run_shell("alias say='echo hello'; say world; alias echo='echo -n'; echo x; unalias echo; echo y", output='hello world\nxy\n')
        self.run_shell('x=outer; (x=inner; cd /; echo $x); echo $x; pwd', output='inner\nouter\n'+str(self.root)+'\n')

    def test_braces_and_globs(self):
        self.run_shell('echo x{a,b}{1,2} {03..01} {a..e..2}', output='xa1 xa2 xb1 xb2 03 02 01 a c e\n')
        (self.root / 'a.txt').write_text('')
        (self.root / 'b.txt').write_text('')
        self.run_shell('printf "%s\\n" *.txt "*.txt"', output='a.txt\nb.txt\n*.txt\n')
        self.run_shell('shopt -s nullglob; echo nothing-*.none done', output='done\n')
        self.run_shell('shopt -s failglob; echo nothing-*.none', code=1)
        (self.root / 'dir').mkdir()
        (self.root / 'dir' / 'nested.txt').write_text('')
        self.run_shell('shopt -s globstar; printf "%s\\n" **/*.txt', output='a.txt\nb.txt\ndir/nested.txt\n')

    def test_source_eval_and_cli(self):
        (self.root / 'source.mish').write_text('x=sourced\nf() { echo function; }\nreturn 4\necho no\n')
        self.run_shell('source ./source.mish; echo $x $?; f', output='sourced 4\nfunction\n')
        self.run_shell("eval 'echo' 'evaluated'", output='evaluated\n')
        self.run_shell('', stdin='\n echo one\nif true; then\necho two\nfi\n', output='one\ntwo\n')
        self.run_shell('', stdin='/bin/cat\nchild input\n', output='child input\n')
        self.run_shell('', stdin='exit 3\necho no\n', code=3, output='')
        self.run_shell('echo "$0" "$1" "$2"', args=('one', 'two'), output='mish-test one two\n')
        result = subprocess.run([str(BINARY), '--norc', '-n', '-c', 'echo no > created'], cwd=self.root, env=self.env, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertFalse((self.root/'created').exists())

    def test_background_and_traps(self):
        self.run_shell('/bin/sh -c "exit 6" & wait $!; echo $?', output='6\n')
        self.run_shell('sleep 0.02 & jobs -p; wait', output=None)
        self.run_shell("trap 'echo cleanup:$?' EXIT; return 7", code=7, output='cleanup:7\n')
        self.run_shell("trap 'echo handled' USR1; kill -USR1 $$; echo after", output='handled\nafter\n')

    def test_errors_and_recovery(self):
        self.run_shell('no-such-command-ever', code=127)
        path = self.root / 'notexec'; path.write_text('echo unsafe\n')
        self.run_shell('./notexec', code=126)
        self.run_shell('echo "unterminated', code=2)
        self.run_shell('echo $((1/0))', code=1)
        self.run_shell('', stdin='echo "bad\n', code=2)

if __name__ == '__main__':
    unittest.main(verbosity=2)
