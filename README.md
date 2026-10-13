# mish

A dependency-free C11 Unix shell with scripting, pipelines, job control, and an
interactive editor. This branch expands the original C implementation; it does
not depend on the separate Rust rewrite.

```sh
make
./bin/mish
./bin/mish -c 'printf "hello\n" | cat'
./bin/mish example.mish first second
printf 'echo first\necho second\n' | ./bin/mish
```

GCC or Clang, a POSIX Unix environment, and Make are sufficient to build. Python
3 is used only for tests. `./bin/mish --help` describes invocation flags; `help`
and `help NAME` describe builtins. `make clean` removes build products.

## Shell language

Commands support newline/semicolon lists, `&&`, `||`, `!`, pipelines (`|` and
`|&`), background `&`, subshells, and brace groups. Redirection includes input,
output, append, descriptor duplication/closure, read-write files, here documents
(including quoted delimiters and `<<-`), here strings, and combined stdout/stderr
redirection. Builtin redirections restore descriptors afterward; bare `exec`
retains them. `noclobber` protects existing output files and `>|` overrides it.

The language includes `if`/`elif`/`else`, `while`, `until`, word and positional
`for`, C-style arithmetic `for`, `case`, functions, and arithmetic commands.

```text
greet() {
    local name="$1"
    printf 'hello, %s\n' "$name"
}
for name in Alice Bob; do
    greet "$name"
done
for ((i=0; i<3; i++)); do
    echo "$i"
done
if test -f notes.txt; then
    cat notes.txt | head -n 5
fi
```

Leading assignments persist when no command follows. Assignments attached to
ordinary commands are temporary and exported to that command. Functions receive
temporary positional arguments; `local` values and attributes restore on return.
`source` executes files in the current shell. `break`, `continue`, `return`, and
`exit` use explicit control flow rather than terminating the implementation's
call stack unexpectedly. Noninteractive syntax errors stop the source; an
interactive shell can recover and read another command.

## Expansion

Mish keeps its original `$(NAME)` variable notation and `$[command]` capture.
It additionally supports `$NAME`, `${NAME}`, positional and special parameters,
backticks, and unambiguous `$(command with arguments)` command capture. A lone
valid variable name inside `$(...)` always means a variable: use `$[pwd]` for a
single-word command. Nested legacy variable names such as `$($(KEY))` work.

```text
NAME='two words'
echo "$(NAME)" "$NAME"
echo "$[pwd]"
echo "${MISSING:-default}"
echo "$((2 + 3 * 4))"
printf '%s\n' file{01..03}.txt
```

Single quotes protect literal text; double quotes permit expansion and preserve
argument boundaries. ANSI C `$'...'` quoting decodes escapes. Unquoted expansion
bytes undergo `IFS` splitting; literal source bytes do not. `"$@"` preserves
positional arguments, including empty ones. Parameter operators include default,
alternative, assignment, required-value, length, substring, prefix/suffix
removal, replacement, and ASCII case conversion. Checked signed 64-bit
arithmetic supports assignments, short-circuit logic, ternaries, exponentiation,
increments, numerical bases, and overflow/division diagnostics.

Tilde expansion supports home, named users, `~+`, and `~-`. Brace lists and
numeric/alphabetic ranges expand before variables. Pathname expansion supports
ordinary wildcards; `shopt -s globstar` enables recursive `**`, with symlink
recursion avoided. `nullglob`, `failglob`, and `dotglob` control unmatched and
hidden names. Quotes retain their literal meaning in pathname and case patterns.
Command capture drains output while children run, strips trailing newlines,
isolates variable/directory changes, and distinguishes malformed source from an
ordinary nonzero captured command.

## Builtins and interactive use

The original thirteen commands remain available. The expanded library includes
formatting and input, variable attributes, positional/options handling,
directory stacks, command inspection, predicates, arithmetic, sources/eval,
resources, signals, jobs, and common streaming text operations. `help` lists the
registry and each command's supported options. The bundled text utilities have
focused option sets; an absolute path such as `/usr/bin/sort` can invoke the
system implementation.

Interactive sessions provide UTF-8 cursor editing, word operations, kill/yank,
undo/redo, history navigation and incremental search, completion, custom key
bindings, a compact vi mode, bracketed paste, and resize-aware redraw. `PS1` and
`PS2` support variable/command expansion and prompt escapes. History persists in
a private, length-framed file with locked atomic writes; concurrent sessions
append without overwriting each other. Completion covers commands, files,
directories, variables, aliases, functions, and configurable static word lists.

Foreground jobs receive the terminal and a process group. Ctrl-C interrupts the
job, Ctrl-Z suspends it, and `jobs`, `bg`, `fg`, `wait`, and `kill` manage it. The
editor restores the original terminal settings before execution, EOF, normal
shutdown, and handled termination. `trap` supports signals and EXIT/DEBUG/ERR
hooks. Options include `errexit`, `nounset`, `pipefail`, `noexec`, `xtrace`, and
`noclobber`; `set -o` lists available options.

Interactive shells read `$HOME/.mishrc`; login shells additionally read
`/etc/profile` and `$HOME/.mish_profile`. `--norc` skips startup files. Startup
files must use mish's supported language, especially its `$(NAME)` ambiguity.

## Scope and verification

This is an expanded mish shell, not a claim of complete POSIX or Bash
compatibility. Arrays, process substitution, extended glob syntax, case
fall-through, coprocesses, and Bash-specific grammar are unsupported. The editor
operates on UTF-8 characters rather than grapheme clusters. Programmable
completion uses static lists/categories rather than executing arbitrary
completion functions. Sources/captures, parser depth, arithmetic work, brace
results, and recursive pathname matches have explicit resource limits.

Detailed feature and option documentation:

- [Parsing and arithmetic](docs/parser.md)
- [Builtin library](docs/builtins.md)
- [Editing, prompts, completion, and history](docs/interactive.md)

```sh
make check CC=clang        # Strict warnings and all suites
make check CC=gcc
make sanitize CC=clang     # AddressSanitizer + UndefinedBehaviorSanitizer
make loc                  # Physical C source/header lines; excludes tests/docs
```

Tests execute the real binary with temporary directories, parser/arithmetic
ownership harnesses, and real controlling pseudo-terminals. Coverage includes
pipelines beyond pipe capacity, descriptor restoration, quoting/IFS edge cases,
functions/loops, syntax error propagation, directory/variable ownership, signal
statuses, suspended jobs, completion, terminal restoration, and concurrent
history writes. Subprocesses have bounded timeouts. CI performs strict compiler
and sanitizer checks.

The expansion requirement is measured against commit `7380ab9`: its
`src/*.c` and `src/*.h` contain 2,869 physical lines, making the five-times target
14,345. `make loc` measures the active C implementation with the same method;
there are no vendored libraries, generated padding, or retained unused legacy
modules included in that total.

Licensed under GPL-3.0-only; see [LICENSE](LICENSE).
