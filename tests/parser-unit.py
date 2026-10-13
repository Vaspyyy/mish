#!/usr/bin/env python3
"""Standalone parser/arithmetic ownership and semantics checks.

The harness links real parser modules and the real utility implementation. A
minimal variable store isolates arithmetic from the unfinished process runner.
"""
import os
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "mish.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
const char *shell_get(const Shell *shell, const char *name) {
    for (const Var *v = shell->variables; v; v = v->next)
        if (!strcmp(v->name, name)) return v->value;
    return NULL;
}
int shell_set(Shell *shell, const char *name, const char *value) {
    Var *v;
    for (v = shell->variables; v; v = v->next) if (!strcmp(v->name, name)) break;
    if (!v) { v = xcalloc(1, sizeof(*v)); v->name = xstrdup(name);
        v->next = shell->variables; shell->variables = v; }
    if (v->readonly) return -1;
    free(v->value); v->value = xstrdup(value); return 0;
}
int main(int argc, char **argv) {
    if (argc != 3) return 99;
    if (!strcmp(argv[1], "parse") || !strcmp(argv[1], "source")) {
        ParseError error = {0};
        Node *node = parse_source(argv[2], &error);
        if (!node) {
            printf("ERROR %d %zu %zu %s\n", error.incomplete,
                error.line, error.column, error.message ? error.message : "missing diagnostic");
            parse_error_free(&error); return 2;
        }
        Node *clone = node_clone(node);
        node_free(node);
        if (!strcmp(argv[1], "source")) {
            char *source = node_source(clone);
            if (!source) { node_free(clone); return 98; }
            fputs(source, stdout); free(source);
        } else {
            node_print(clone, stdout, 0);
        }
        node_free(clone);
        return 0;
    }
    if (!strcmp(argv[1], "end")) {
        ParseError error = {0}; size_t closer = 0;
        int status = lexer_expansion_end(argv[2], 0, &closer, &error);
        if (status) printf("ERROR %d %s\n", error.incomplete, error.message);
        else printf("%zu\n", closer);
        parse_error_free(&error); return status ? 2 : 0;
    }
    Shell shell = {0};
    int64_t value;
    char *error = NULL;
    int status = arithmetic_eval(&shell, argv[2], &value, &error);
    if (status) printf("ERROR %s\n", error ? error : "missing diagnostic");
    else printf("%" PRId64 "\n", value);
    free(error);
    while (shell.variables) {
        Var *next = shell.variables->next;
        printf("%s=%s\n", shell.variables->name, shell.variables->value);
        free(shell.variables->name); free(shell.variables->value);
        free(shell.variables); shell.variables = next;
    }
    return status ? 2 : 0;
}
'''


class ParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mish-parser-")
        root = Path(cls.directory.name)
        harness = root / "harness.c"
        harness.write_text(HARNESS)
        cls.binary = root / "harness"
        flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-g"]
        if os.environ.get("MISH_TEST_SANITIZE"):
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([os.environ.get("CC", "cc"), *flags, "-I", str(ROOT / "src"),
                        str(harness), *(str(ROOT / "src" / name) for name in
                         ["util.c", "lexer.c", "parse.c", "ast.c", "arithmetic.c"]),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_harness(self, mode, source):
        result = subprocess.run([str(self.binary), mode, source], capture_output=True,
                                text=True, timeout=4)
        self.assertNotIn("runtime error:", result.stderr)
        self.assertNotIn("AddressSanitizer", result.stderr)
        return result

    def parsed(self, source):
        result = self.run_harness("parse", source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def arithmetic(self, source, expected, variables=()):
        result = self.run_harness("arithmetic", source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(lines[0], str(expected), source)
        for variable in variables:
            self.assertIn(variable, lines)

    def invalid_arithmetic(self, source, diagnostic):
        result = self.run_harness("arithmetic", source)
        self.assertEqual(result.returncode, 2, source)
        self.assertIn(diagnostic, result.stdout)

    def test_raw_word_quotes_custom_and_standard_substitutions(self):
        tree = self.parsed("echo 'a b' \"$[echo 'x ] y']\" $(HOME) ${x:-$(echo hi)} $((2+(3))) `echo x` $'a\\'b'")
        self.assertIn("simple", tree)
        self.assertIn("$(HOME)", tree)
        self.assertIn("$[echo", tree)
        self.assertIn("$((2+(3)))", tree)

    def test_operators_groups_background_functions(self):
        tree = self.parsed("f() { echo x; }; function g { (f && echo y) | cat & }; ! false || g\n")
        for kind in ["function", "group", "subshell", "and", "pipeline", "background", "not", "or"]:
            self.assertIn(kind, tree)

    def test_if_elif_else_and_loops(self):
        tree = self.parsed("if false; then echo no; elif true; then while false; do :; done; else until true; do :; done; fi")
        self.assertEqual(tree.count("if line="), 2)
        self.assertIn("while line=", tree)
        self.assertIn("until line=", tree)
        tree = self.parsed("for x in a 'b c'; do echo $x; done; for y; do :; done")
        self.assertIn('name="x"', tree)
        self.assertIn('$@', tree)

    def test_case_multiple_patterns_empty_arm(self):
        tree = self.parsed("case $x in (a|b) echo one;; c) ;; *) echo fallback; esac")
        self.assertEqual(tree.count("case-arm patterns="), 3)
        self.assertIn('patterns=["a", "b"]', tree)

    def test_redirections_all_forms_and_order(self):
        tree = self.parsed("cat 3<input 4>output >>log >|clobber 5<&0 6>&1 7<>rw <<<hello &>both &>>append")
        for operator in ["<", ">", ">>", ">|", "<&", ">&", "<>", "<<<"]:
            self.assertIn("operator=" + operator, tree)
        self.assertEqual(tree.count("redirect fd="), 12)
        self.assertLess(tree.index("fd=3"), tree.index("fd=4"))

    def test_heredocs_are_queued_and_quotes_control_expansion(self):
        tree = self.parsed("cat <<EOF <<-'LITERAL'\nhello $x\nEOF\n\t$x\n\tLITERAL\necho after\n")
        self.assertIn('expand=true body="hello $x\\n"', tree)
        self.assertIn('expand=false body="$x\\n"', tree)
        self.assertEqual(tree.count("simple line="), 2)

    def test_heredoc_delimiter_can_be_empty_or_escaped(self):
        tree = self.parsed("cat <<''\nbody\n\n")
        self.assertIn('body="body\\n"', tree)
        tree = self.parsed("cat <<E\\OF\nbody\nEOF\n")
        self.assertIn("expand=false", tree)

    def test_incomplete_and_invalid_syntax_have_distinct_diagnostics(self):
        for source in ["echo '", "echo $[x", "echo \\", "if true; then echo x", "cat <<EOF\nbody\n", "echo |", "f()"]:
            result = self.run_harness("parse", source)
            self.assertEqual(result.returncode, 2, source)
            self.assertTrue(result.stdout.startswith("ERROR 1 "), result.stdout)
        for source in ["; echo x", "echo ;;", "then", "{ echo x }", "echo && ;", "for 9x; do :; done", "for x do :; done"]:
            result = self.run_harness("parse", source)
            self.assertEqual(result.returncode, 2, source)
            self.assertTrue(result.stdout.startswith("ERROR 0 "), result.stdout)

    def test_nested_substitution_comments_heredocs_and_ansi_literals(self):
        sources = ["echo $[cat <<EOF\n] ) \"\nEOF\n]",
                   "echo $(echo yes # ) \" ignored\n)",
                   "echo $'$(literal ] unmatched'", "echo $((1<<3))"]
        for source in sources:
            self.parsed(source)
        for source in ["$[cat <<EOF\n] ) \"\nEOF\n]tail", "$(echo yes # )\n)tail", "${x:-$(HOME)}tail"]:
            result = self.run_harness("end", source)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(int(result.stdout), len(source) - 5)
        self.run_harness("end", "$[cat <<EOF\nmissing\n")
        self.parsed("cat <<$'E\\x4fF'\nbody\nEOF\n")
        self.parsed('cat <<"E\\a"\nbody\nE\\a\n')

    def test_arithmetic_commands_and_c_style_for(self):
        tree = self.parsed("((a=1<<3)); for ((i=0; i<3; i++)); do echo $i; done")
        self.assertIn('words=["let",', tree)
        self.assertIn("arithmetic-for", tree)
        self.parsed("for ((;;)) do break; done")
        for source in ["for ((i=0;i<3)); do :; done", "((a)b)"]:
            result = self.run_harness("parse", source)
            self.assertEqual(result.returncode, 2, source)

    def test_canonical_source_roundtrips_with_heredocs_and_compounds(self):
        sources = ["f() { if true; then echo 'hello world'; else :; fi; }; f",
                   "cat <<'EOF' | cat && echo after\n$x \" ) ]\nEOF\n",
                   "case $x in (a|b) echo hit;; *) ;; esac",
                   "for ((i=0;i<2;i++)); do (echo $i); done",
                   "while false; do :; done & echo done",
                   "cat <<EOF\n__MISH_DOCUMENT_0__\nEOF\n"]
        for source in sources:
            original = self.parsed(source)
            serialized = self.run_harness("source", source)
            self.assertEqual(serialized.returncode, 0, serialized.stdout + serialized.stderr)
            reparsed = self.parsed(serialized.stdout)
            # Source positions and generated heredoc delimiters can differ;
            # command structure and each original raw word must survive.
            for kind in ["simple", "if", "pipeline", "case-arm", "arithmetic-for", "while", "background"]:
                self.assertEqual(original.count(kind), reparsed.count(kind), serialized.stdout)

    def test_precedence_and_shell_numeric_bases(self):
        cases = [("2+3*4", 14), ("(2+3)*4", 20), ("1<<2+1", 8),
                 ("4>3==1", 1), ("1|2^3&1", 3), ("2**3**2", 512),
                 ("-2**2", 4), ("010+0x10+0b11", 27), ("16#ff+2#101", 260),
                 ("64#_", 63), ("64#@", 62), ("36#Z", 35), ("", 0)]
        for source, expected in cases:
            self.arithmetic(source, expected)

    def test_assignments_increment_and_conditional_short_circuit(self):
        self.arithmetic("a=2, b=a++ + ++a, a*10+b", 46, ["a=4", "b=6"])
        self.arithmetic("a=2, ++a**2", 9, ["a=3"])
        self.arithmetic("a=b=7, a+=3, b*=2, a+b", 24, ["a=10", "b=14"])
        self.arithmetic("a=0, 0&&(a=1), 1||(a=2), a", 0, ["a=0"])
        self.arithmetic("a=1, a ? (a=8) : 1/0", 8, ["a=8"])
        self.arithmetic("0 ? 1/0 : 4", 4)
        self.arithmetic("1 ? 5 : (a=7)", 5)
        self.arithmetic("a=32, a>>=2, a|=1, a^=3, a&=6, a", 2)
        self.arithmetic("a=2, a**=4", 16, ["a=16"])
        self.arithmetic("a=0, a ? 1 : a=9", 9, ["a=9"])

    def test_signed_extremes_and_overflow(self):
        self.arithmetic("-9223372036854775808", -(2**63))
        self.arithmetic("-9223372036854775808 % -1", 0)
        self.arithmetic("-1>>63", -1)
        self.arithmetic("-1<<63", -(2**63))
        self.arithmetic("~0", -1)
        for source in ["9223372036854775807+1", "-9223372036854775808-1",
                       "3037000500*3037000500", "-9223372036854775808/-1",
                       "1<<63", "-(-9223372036854775808)", "2**63"]:
            self.invalid_arithmetic(source, "overflow")
        self.invalid_arithmetic("1<<64", "shift count")
        self.invalid_arithmetic("1>>-1", "shift count")
        self.invalid_arithmetic("1/0", "division by zero")
        self.invalid_arithmetic("2**-1", "negative exponent")
        self.invalid_arithmetic("9223372036854775808", "signed 64-bit range")

    def test_arithmetic_syntax_and_targets(self):
        for source in ["1+", "(1", "1 2", "a?1", "1=2", "++(1+2)", "(a++)++", "08", "1#0", "65#0", "0x"]:
            result = self.run_harness("arithmetic", source)
            self.assertEqual(result.returncode, 2, source)
            self.assertIn("ERROR", result.stdout)
        self.invalid_arithmetic("(" * 300 + "1" + ")" * 300, "nesting limit")

    def test_bounded_deterministic_adversarial_fragments(self):
        randomizer = random.Random(103)
        alphabet = "abcé\\\"'$[](){};|&<># \n"
        for _ in range(160):
            source = "".join(randomizer.choice(alphabet) for _ in range(randomizer.randrange(1, 70)))
            result = self.run_harness("parse", source)
            self.assertIn(result.returncode, (0, 2), source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
