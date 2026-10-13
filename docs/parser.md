# Parsing, command trees, and arithmetic

The lexer keeps each word's original spelling. Quotes, escaped whitespace,
`$(NAME)`, `$[command]`, `${parameter}`, `$(command)`, `$((arithmetic))`, and
backticks therefore reach runtime expansion without losing argument boundaries.
Comments start at token boundaries. Escaped newlines join logical words.
Standalone braces delimit groups; braces inside `{a,b}`, `dir/{a,b}`, and
`{1..3}` stay in ordinary words for brace expansion.

The parser supports:

- Sequential lists separated by newlines or semicolons.
- Left-associative `&&` and `||`, pipelines, `|&`, `!`, and background `&`.
- Subshells `(commands)` and groups `{ commands; }`.
- `if` / `elif` / `else` / `fi`.
- `while` and `until` loops.
- `for name in words; do commands; done` and positional `for name; do ...`.
- Arithmetic commands `((expression))` and C-style
  `for ((initialization; condition; step)); do commands; done`.
- `case word in (pattern|pattern) commands;; ... esac`.
- `name() { commands; }` and `function name { commands; }` definitions.

Redirections retain source order and may occur inside simple commands or after
compound commands. Supported operators are `<`, `>`, `>>`, `>|`, `<&`, `>&`,
`<>`, `<<`, `<<-`, `<<<`, `&>`, and `&>>`, with explicit numeric descriptors.
`&>` and `&>>` become an output redirection followed by `2>&1`, so ordering stays
observable. The `|&` shorthand similarly appends `2>&1` to the preceding stage.
Descriptor numbers above `INT_MAX` produce a diagnostic.

Here documents are collected after the complete command header's newline.
Several documents on the same header are read in source order. Quoting any part
of a delimiter disables body expansion. `<<-` removes leading tabs from both
body lines and the delimiter line. Empty quoted delimiters work. Delimiter
quote removal also handles backslash escapes and ANSI C `$'...'` escapes.
Document bodies are never tokenized as commands. The same scanner handles
comments and here documents inside command substitutions, so literal quotes,
`]`, and `)` inside a body do not terminate its enclosing substitution.

A `ParseError` includes line, column, message, and an `incomplete` flag. Open
quotes, trailing escapes, unfinished substitutions, missing command bodies,
and missing here document delimiters request continuation. A definite grammar
error, such as `echo && ;` or a group missing its final separator, does not.
The source is limited to 16 MiB and nesting to `MISH_MAX_DEPTH`; the parser also
validates the assembled tree so long logical-operator chains cannot bypass the
nesting limit. Tree destruction uses an explicit stack.

`node_clone` makes a fully independent copy, including words, redirections,
case arms, and here document bodies. `node_print` emits an escaped debug tree.
`node_source` emits canonical shell source, preserving raw expansion syntax and
using nonconflicting generated here document delimiters. Serialization is used
for inspectable function definitions and can be parsed again.

Arithmetic uses signed 64-bit integers and a parsed expression tree. It supports
comma, assignments and compound assignments, ternary `?:`, `||`, `&&`, bitwise
operators, comparisons, shifts, addition, subtraction, multiplication,
division, remainder, exponentiation `**`, unary operators, parentheses, and
prefix/postfix increment and decrement. Assignments require a variable target.
Logical and ternary operands short-circuit without applying skipped assignments
or raising skipped division errors. Unary operators bind before exponentiation;
exponentiation and assignment associate to the right.

Literals accept decimal, octal, hexadecimal, binary, and `base#digits` for bases
2 through 64. Bases above 36 distinguish uppercase letters and support `@` and
`_`. Shell variable values are themselves arithmetic expressions; unset values
are zero unless `nounset` is active. Cyclic variable references are rejected.
The evaluator checks overflow, division by zero, negative exponents, and shift
counts. Right shifting negative values has a defined arithmetic-shift result.
`-9223372036854775808` is supported without first overflowing its positive
magnitude. Evaluation work and recursively parsed text have explicit budgets.

The language intentionally does not claim Bash compatibility for arrays,
process substitution, case fall-through `;&` / `;;&`, or extended glob syntax.

Run `python3 tests/parser-unit.py` for standalone syntax, clone/free,
serialization, arithmetic, diagnostic, and deterministic adversarial tests.
`MISH_TEST_SANITIZE=1 CC=clang python3 tests/parser-unit.py` runs the same suite
with AddressSanitizer and UndefinedBehaviorSanitizer.
