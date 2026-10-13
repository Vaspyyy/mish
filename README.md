# mish

A small Unix shell written in dependency-free Rust. It runs external programs,
keeps local and exported variables, and supports aliases and explicit expansion.
It uses its own syntax; it is not a POSIX shell.

Build with Rust 1.74 or newer:

```sh
cargo build --release
./target/release/mish
```

Run a single command, a script, or commands from standard input:

```sh
./target/release/mish -c 'echo hello'
./target/release/mish example.mish
printf 'echo first\necho second\n' | ./target/release/mish
./target/release/mish --help
./target/release/mish --version
```

With no arguments, mish reads every input line until EOF. It shows a prompt only
when standard input is a terminal. A final line does not need a newline. Blank
lines preserve the previous status. Scripts use the same line-oriented syntax;
quoting and substitutions cannot continue across lines.

## Syntax

Separate arguments with whitespace. Single quotes preserve literal text; double
quotes permit expansions. Backslashes escape characters outside single quotes.
Quoted empty strings remain arguments. Expansions retain their whitespace inside
the argument that contains them; there is no word splitting or globbing.

```text
echo 'literal $(HOME)' "home: $(HOME)"
NAME='two words' COPY=$(NAME) echo $(COPY)
echo $[pwd]
echo $[echo $[echo nested]]
echo $($)
```

- `$(NAME)` expands a variable, or an empty string when unset. Bare `$NAME` is
  literal text. `$($)` expands the previous command's status.
- `$[command]` captures the command's stdout and removes trailing newlines.
  Nested substitutions are supported, with a depth limit of 32. Stderr remains
  stderr. A nonzero captured command still supplies its stdout.
- Leading `NAME=value` words assign local variables sequentially and persist
  after the command. A later assignment or argument can use an earlier value.
  Once a command name is reached, `NAME=value` is an ordinary argument.
- `~` at the start of an unquoted argument expands `HOME`.
- Aliases replace the first raw command token once, before parsing. They do not
  recursively expand other aliases.

Substitutions execute with a copy of shell state, so their assignments, exports,
aliases, `cd`, and `exit` do not change the parent shell. External commands receive
exported variables and the shell's working directory.

## Builtins

All 13 builtins are available without a `PATH`:

| Builtin | Usage and behavior |
| --- | --- |
| `exit` | `exit [status]`: end the shell; default is the previous status. |
| `which` | `which [-c\|--path-only] [-h\|--help] NAME...`: identify aliases, builtins, and executable paths. `-c` prints only paths for external programs; aliases and builtins keep their classifications. |
| `echo` | `echo [ARG...]`: print arguments separated by spaces, followed by a newline. No option or escape interpretation. |
| `cd` | `cd [DIRECTORY]`: change directory; default is `HOME`. Update exported `PWD` and `OLDPWD` on success. `cd -` refers to a directory literally named `-`. |
| `pwd` | Print the current physical working directory. |
| `env` | List exported variables. |
| `export` | `export NAME[=VALUE]...`: export an existing local value or set an exported value. Later assignments update exported variables. |
| `unset` | `unset NAME...`: remove local and exported values. |
| `path` | List executable files found in `PATH`. |
| `alias` | `alias NAME='COMMAND'...`: create or replace aliases. With no arguments, list aliases. |
| `unalias` | `unalias NAME...`: remove aliases. |
| `return` | `return [status]`: set command status without leaving the shell; default is zero. |
| `hash` | List cached executable resolutions. |

`env`, `path`, `alias`, and `hash` retain the original shell's ANSI bold formatting.
Numeric status arguments accept signed decimal, octal (`010`), and hexadecimal
(`0x10`). Process exit codes are normalized to 0–255. Missing commands return
127, execution failures return 126, and parse errors return 2. EOF returns the
last command's status.

`PATH` lookup selects the first executable match, including executable symlinks.
An empty `PATH` component refers to the current directory. Relative components
follow `cd`. Changing or unsetting `PATH`, or changing directory, refreshes the
command cache. A command containing `/` runs directly relative to the current
directory.

## Differences from the original C implementation

The Rust rewrite retains mish's expansion syntax and builtin names while fixing
the original implementation's EOF loop, single-line redirected input, incomplete
`unalias` registration, alias replacement, and external status handling. It also
provides `-c`, script arguments, help, and version output.

Quoted empty arguments and Unicode text survive parsing. Malformed quotes,
escapes, and substitutions produce diagnostics. Command capture drains stdout
and stderr while the child runs, including output larger than pipe capacity.
Execution failures cannot fall through into a second shell. Directory and
environment state is owned by each shell, and failed `cd` leaves it intact.
Local variables reach children only after export.

This remains a deliberately limited shell: no pipelines, redirections,
background jobs, globbing, command separators, `&&`/`||`, control flow, comments,
history, line editing, or job control. It does not install interactive Ctrl-C
handling. Use a mature shell for those features. Command and environment strings
must be UTF-8; inherited environment entries containing non-UTF-8 text are
omitted. Captured non-UTF-8 stdout is converted with replacement characters.
Unix is required.

## Development

```sh
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test --all-targets
cargo build --release
```

Integration tests launch the actual binary with isolated temporary directories
and environments. They cover CLI modes, all builtins, variables and child
inheritance, executable lookup, aliases, nested and large captures, error
recovery, and exit status. A ten-second timeout bounds each child invocation.
Unix fixtures use `/bin/sh`, `/bin/cat`, and `/bin/rm`; Cargo dependencies are unnecessary.
CI runs these checks on stable Rust and tests/builds on Rust 1.74.0.

Licensed under GPL-3.0-only; see [LICENSE](LICENSE).
