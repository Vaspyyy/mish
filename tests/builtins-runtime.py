#!/usr/bin/env python3
"""Exercise the active C builtin library through the real shell process."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else os.environ.get("MISH_BIN", ROOT / "bin/mish")).resolve()


class Builtins(unittest.TestCase):
    def run_shell(self, source, input=None, cwd=None):
        return subprocess.run([str(BINARY), "-c", source], input=input,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, cwd=cwd, timeout=8)

    def check(self, source, expected, status=0, input=None, cwd=None):
        result = self.run_shell(source, input, cwd)
        self.assertEqual(result.returncode, status, (source, result.stdout, result.stderr))
        self.assertEqual(result.stdout, expected, (source, result.stderr))
        return result

    def test_echo_and_printf(self):
        self.check("echo -ne 'a\\tb'; printf ':%06d:%s:%b\\n' 7 x 'c\\nd'", "a\tb:000007:x:c\nd\n")
        self.check("printf '%s=%d\\n' x 1 y 2", "x=1\ny=2\n")
        self.check("printf -v answer '%04x' 42; echo $answer", "002a\n")
        self.check("printf '%*.*s\\n' 5 2 abcdef", "   ab\n")
        self.check("printf '%.*s\\n' -1 abc", "abc\n")
        self.check("printf '%bX' 'a\\cstop'", "a")
        self.assertEqual(self.run_shell("printf '%n' x").returncode, 2)

    def test_variable_attributes(self):
        self.check("x=one; export x; printenv x; export -n x; printenv x", "one\n", 1)
        self.check("readonly fixed=one; fixed=two; echo $fixed", "one\n")
        self.check("readonly fixed=one; unset fixed", "", 1)
        self.check("x=outside; f() { local x=inside; echo $x; }; f; echo $x", "inside\noutside\n")
        self.check("declare -i answer='2+3*4'; echo $answer", "14\n")
        self.check("x=old; f() { declare -g x=new; }; f; echo $x", "new\n")
        self.check("readonly x=old; env x=new printenv x; echo $x", "new\nold\n")
        self.assertEqual(self.run_shell("local x=bad").returncode, 2)
        self.assertEqual(self.run_shell("export '=bad'").returncode, 1)

    def test_positional_options_and_getopts(self):
        self.check("set -- one two three; echo $#:$1:$2; shift 2; echo $#:$1", "3:one:two\n1:three\n")
        self.check("set -- a; shift 2", "", 2)
        self.check("set -- -ab value operand; while getopts 'ab:' option; do echo $option:$OPTARG; done; echo $OPTIND", "a:\nb:value\n3\n")
        self.check("set -- -z; getopts ':a' option; echo $option:$OPTARG", "?:z\n")
        self.check("shopt -s globstar; shopt -q globstar; shopt -u globstar; shopt -q globstar", "", 1)
        self.check("set -o pipefail; false | true", "", 1)

    def test_read(self):
        self.check("read first rest; printf '<%s>|<%s>\\n' \"$first\" \"$rest\"", "<a>|<b  c>\n", input="  a b  c  \n")
        self.check('IFS=:; read a b c; echo "$a:$b:$c"', "one:two:three:four\n", input="one:two:three:four\n")
        self.check("read -r; printf '%s\\n' \"$REPLY\"", "a\\b\n", input="a\\b\n")
        self.check('read a b; printf "<%s>|<%s>\\n" "$a" "$b"', "<one two>|<three>\n", input="one\\ two three\n")
        self.check('IFS=" :"; read a b; printf "<%s>|<%s>\\n" "$a" "$b"', "<one>|<two>\n", input="one : two : \n")
        self.check('read -N 3 a b; printf "<%s>|<%s>\\n" "$a" "$b"', "<a b>|<>\n", input="a bxxx")
        self.check("read -N 3 part; echo $part", "abc\n", input="abcdef\n")
        self.check("read -d : part; echo $part", "abc\n", input="abc:def\n")
        self.check("read part", "", 1, input="")

    def test_read_timeout(self):
        process = subprocess.Popen([str(BINARY), "-c", "read -t 0.02 value"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.wait(timeout=2), 142)
        finally:
            process.communicate(timeout=2)

    def test_status_flow_eval_and_source(self):
        self.check("return 23; echo $($); exit 7", "23\n", 7)
        self.check("f() { echo before; return 6; echo wrong; }; f; echo $?", "before\n6\n")
        self.check("eval 'x=ok; echo $x'", "ok\n")
        self.check("for x in 1 2 3; do if test $x -eq 2; then continue; fi; echo $x; done", "1\n3\n")
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "script.mish"
            script.write_text("inside=changed; echo $1; return 12; echo wrong\n")
            self.check(f"source '{script}' value; echo $inside:$?", "value\nchanged:12\n")
        self.assertEqual(self.run_shell("break").returncode, 2)
        self.assertEqual(self.run_shell("exit bad").returncode, 2)

    def test_alias_resolution_and_cache(self):
        self.check("alias greet='echo old'; alias greet='echo new'; greet; unalias greet; type -t echo", "new\nbuiltin\n")
        self.check("alias x='echo one'; unalias -a; alias", "<no aliases set>\n")
        self.check("f() { echo function; }; command echo builtin; builtin echo direct; type -t f", "builtin\ndirect\nfunction\n")
        result = self.run_shell("which --path-only /bin/sh; hash -p /bin/sh fixture; hash -t fixture; hash -d fixture; hash")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("/bin/sh\n/bin/sh\n"), result.stdout)
        self.assertIn("<no hashes defined>", result.stdout)

    def test_directories_and_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "a").mkdir()
            (base / "b").mkdir()
            self.check("cd a; pwd -P; cd -; pwd -P", f"{base}/a\n{base}\n{base}\n", cwd=base)
            self.check("pushd a > /dev/null; popd > /dev/null; pwd -P", f"{base}\n", cwd=base)
            self.check("CDPATH='.'; cd a; pwd", f"{base}/a\n{base}/a\n", cwd=base)
            self.check("basename /a/b/file.txt .txt; dirname /a/b/file.txt; realpath -m a/../missing", f"file\n/a/b\n{base}/missing\n", cwd=base)
            self.check("mkdir -p parent/child; touch parent/child/file; stat -c '%s %F' parent/child/file", "0 regular file\n", cwd=base)
            self.check("umask 077; mkdir -m 755 explicit; stat -c '%a' explicit", "755\n", cwd=base)

            self.check("cd missing; pwd -P", f"{base}\n", cwd=base)
            denied = base / "denied"
            denied.mkdir()
            denied.chmod(0o600)
            if os.geteuid() != 0:
                self.assertNotEqual(self.run_shell("cd denied", cwd=base).returncode, 0)
            denied.chmod(0o700)

    def test_predicates_and_arithmetic(self):
        true_expressions = ["-n x", "-z ''", "7 -gt 4", "010 -eq 10", "x = x", "! ''", "'(' 1 -eq 1 -a x != y ')'", "-d /", "-e /dev/null", "-c /dev/null"]
        for expression in true_expressions:
            self.check(f"test {expression}", "")
        for expression in ["-n ''", "4 -gt 7", "x != x", "! x", "-e /definitely-absent-mish"]:
            self.check(f"[ {expression} ]", "", 1)
        self.check("test abc -eq 1", "", 2)
        self.check("[ x", "", 2)
        self.check("let 'x=3*4'; echo $x; expr 4 + 5", "12\n9\n")
        self.check("match '*.c' main.c other.h", "main.c\n")
        self.check("match -r '(a+)' baa", "aa\naa\n")

    def test_stream_utilities(self):
        self.check("printf 'c\\na\\na\\nb\\n' | sort -u | head -n 2", "a\nb\n")
        self.check("printf '10\\n2\\n1\\n' | sort -nr", "10\n2\n1\n")
        self.check("printf 'a\\na\\nb\\n' | uniq -c", "      2 a\n      1 b\n")
        self.check("printf 'a b\\ncc\\n' | wc -lw", "2 3\n")
        self.check("printf '1:2:3\\na:b:c\\n' | cut -d : -f 1,3", "1:3\na:c\n")
        self.check("printf 'abcdef\\n' | cut -c 2-4", "bcd\n")
        self.check("printf 'aaabbc\\n' | tr -s a-c", "abc\n")
        self.check("printf 'AbC\\n' | tr '[:upper:]' '[:lower:]'", "abc\n")
        self.check("printf '1\\n2\\n3\\n' | tail -n 2", "2\n3\n")
        self.check("printf 'abcdef' | tail -c 3", "def")
        self.check("seq -s , 1 2 5", "1,3,5\n")
        self.check("printf 'a\\n\\n\\nb\\n' | cat -sn", "     1\ta\n     2\t\n     3\tb\n")

    def test_resources_and_help(self):
        self.check("umask 077; umask; umask u=rwx,g=rx,o=; umask", "0077\n0027\n")
        self.check("umask 999", "", 2)
        self.assertTrue(self.run_shell("ulimit -n").stdout.strip().isdigit())
        self.assertEqual(self.run_shell("trap 'echo done' EXIT; echo body").stdout, "body\ndone\n")
        self.assertEqual(self.run_shell("help -s printf").stdout, "printf [-v NAME] FORMAT [ARGUMENT ...]\n")
        self.assertEqual(self.run_shell("help missing-command").returncode, 1)
        self.assertTrue(self.run_shell("getconf PAGESIZE").stdout.strip().isdigit())
        self.assertIn("mish", self.run_shell("version").stdout)


if __name__ == "__main__":
    unittest.main()
