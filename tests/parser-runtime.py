#!/usr/bin/env python3
"""Native process tests for parser/expansion/execution integration."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("MISH_BIN", ROOT / "bin" / "mish"))


@unittest.skipUnless(BINARY.exists(), "build bin/mish before runtime tests")
class RuntimeTests(unittest.TestCase):
    def run_shell(self, source, expected=None, status=0, *, stdin=None, options=()):
        with tempfile.TemporaryDirectory(prefix="mish-parser-runtime-") as directory:
            result = subprocess.run([str(BINARY), "--norc", *options, "-c", source],
                                    cwd=directory, input=stdin, capture_output=True,
                                    text=True, timeout=5)
        self.assertEqual(result.returncode, status, result.stderr)
        if expected is not None:
            self.assertEqual(result.stdout, expected, result.stderr)
        return result

    def test_conditionals_and_boolean_pipeline_precedence(self):
        self.run_shell("if false; then echo no; elif true; then echo yes; else echo no; fi", "yes\n")
        self.run_shell("! false && printf x | cat || echo no", "x")
        self.run_shell("false && echo no || echo yes && echo after", "yes\nafter\n")

    def test_while_until_and_arithmetic_for(self):
        self.run_shell("i=0; while ((i<3)); do echo $i; ((i++)); done", "0\n1\n2\n")
        self.run_shell("i=0; until ((i>=2)); do ((i++)); echo $i; done", "1\n2\n")
        self.run_shell("for ((i=0;i<3;i++)); do echo $i; done", "0\n1\n2\n")
        self.run_shell("for ((;;)); do echo once; break; done", "once\n")

    def test_function_arguments_and_case_matching(self):
        self.run_shell("f() { for x; do echo \"$x\"; done; }; f '' 'a b' c", "\na b\nc\n")
        self.run_shell("function f { case $1 in (a*|b) echo hit;; *) echo miss;; esac; }; f abc", "hit\n")

    def test_groups_and_subshell_state(self):
        self.run_shell("x=outer; (x=inner; echo $x); echo $x; { x=group; }; echo $x", "inner\nouter\ngroup\n")
        self.run_shell("x=outer; echo $[x=inner; echo $x]; echo $x", "inner\nouter\n")

    def test_redirect_order_and_compound_redirects(self):
        self.run_shell("{ echo one; echo two; } >out; cat <out", "one\ntwo\n")
        self.run_shell("printf one >out; printf two >>out; cat out", "onetwo")
        self.run_shell("sh -c 'printf out; printf err >&2' >out 2>&1; cat out", "outerr")
        self.run_shell("sh -c 'printf out; printf err >&2' &>out; cat out", "outerr")
        self.run_shell("cat <<< 'hello world'", "hello world\n")

    def test_heredoc_expansion_tabs_and_multiple_documents(self):
        self.run_shell("x=yes; cat <<EOF\n$x\nEOF\n", "yes\n")
        self.run_shell("x=yes; cat <<'EOF'\n$x\nEOF\n", "$x\n")
        self.run_shell("cat <<-'EOF'\n\t$x\n\tEOF\n", "$x\n")
        self.run_shell("cat <<A <<B\nfirst\nA\nsecond\nB\n", "second\n")
        self.run_shell("cat <<$'E\\x4fF'\nhello\nEOF\n", "hello\n")

    def test_nested_substitutions_containing_comments_and_heredocs(self):
        self.run_shell("echo $[cat <<EOF\n] ) \"\nEOF\n]", '] ) "\n')
        self.run_shell('echo $(echo yes # ) " ignored\n)', "yes\n")
        self.run_shell("echo $[echo $[printf hello]]", "hello\n")
        self.run_shell("echo $[cat <<EOF\n] {a,b}\nEOF\n]", "] {a,b}\n")
        self.run_shell("echo $(echo yes # ) {a,b}\n)", "yes\n")

    def test_raw_quotes_empty_arguments_and_custom_variables(self):
        self.run_shell("x='a b'; printf '<%s>\\n' '' \"$x\" '$(x)' $(x)", "<>\n<a b>\n<$(x)>\n<a>\n<b>\n")
        self.run_shell("echo {a,b} {1..3}", "a b 1 2 3\n")
        self.run_shell("printf '<%s>\\n' $'a\\'b{c,d}'", "<a'b{c,d}>\n")

    def test_arithmetic_values_effects_and_short_circuit(self):
        self.run_shell("((a=2, b=a++ + ++a)); echo $a $b; echo $((0 && 1/0)) $((1 ? 7 : 1/0))", "4 6\n0 7\n")
        self.run_shell("a='b+1'; b=4; echo $((a)) $((2**3**2)) $((16#ff))", "5 512 255\n")
        result = self.run_shell("echo $((1/0))", status=1)
        self.assertIn("division by zero", result.stderr)

    def test_syntax_validation_and_input_continuation(self):
        self.run_shell("echo should-not-run; if true; then :; fi", "", options=("-n",))
        result = self.run_shell("for x do echo wrong; done", status=2)
        self.assertIn("separator", result.stderr)
        source = "if true\nthen\necho continued\nfi\ncat <<EOF\nbody\nEOF\n"
        result = subprocess.run([str(BINARY), "--norc"], input=source, capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "continued\nbody\n")

    def test_ifs_splits_expansion_bytes_without_splitting_source_literals(self):
        self.run_shell("IFS=:; x='a:b'; printf '<%s>\\n' $x a:b", "<a>\n<b>\n<a:b>\n")

    def test_parameter_fallback_and_replacement_keep_inner_quote_masks(self):
        self.run_shell('printf \'<%s>\\n\' ${missing:-"a b"}', "<a b>\n")
        self.run_shell("x='a*b'; printf '<%s>\\n' ${x/'*'/X}", "<aXb>\n")

    def test_nounset_applies_to_parameter_length_and_pattern_operators(self):
        for expression in ["${#MISH_UNSET_REVIEW}", "${MISH_UNSET_REVIEW#x}"]:
            result = self.run_shell("echo " + expression, "", status=1, options=("-u",))
            self.assertIn("unbound", result.stderr)
        self.run_shell("echo ${MISH_UNSET_REVIEW:-default}", "default\n", options=("-u",))

    def test_quoted_positional_expansion_keeps_trailing_empty_fields(self):
        self.run_shell("set -- a ''; printf '<%s>\\n' \"prefix$@\"", "<prefixa>\n<>\n")
        self.run_shell("set -- '' ''; printf '<%s>\\n' \"prefix$@\"", "<prefix>\n<>\n")

    def test_capture_preserves_fatal_parse_errors_and_valid_nonzero_output(self):
        result = self.run_shell("echo $[if]", "", status=2)
        self.assertIn("condition", result.stderr)
        self.run_shell("echo $[sh -c 'printf ordinary; exit 2']", "ordinary\n")

    def test_source_stops_at_syntax_error_and_path_script_fallback_uses_resolved_path(self):
        with tempfile.TemporaryDirectory(prefix="mish-parser-source-") as directory:
            root = Path(directory)
            plain = root / "plain"
            plain.write_text("echo fallback-ok\n")
            plain.chmod(0o755)
            result = subprocess.run([str(BINARY), "--norc", "-c", f"PATH={root} plain"],
                                    cwd="/", capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "fallback-ok\n")
            invalid = root / "invalid"
            invalid.write_text("echo before\nif ; then\necho after\n")
            result = subprocess.run([str(BINARY), "--norc", str(invalid)],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertEqual(result.stdout, "before\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
