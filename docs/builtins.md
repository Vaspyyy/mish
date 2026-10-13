# Builtin command library

All builtin commands are dispatched by `src/keyword.c`; unknown names return to
normal executable resolution. The implementation is dependency-free POSIX C.
`help` lists available commands, `help NAME` provides usage and descriptions,
and `help -s`, `help -d`, and `help -m` select synopsis, summary, and manual forms.
Help patterns accept shell wildcards.

The original commands remain available: `exit`, `which`, `echo`, `cd`, `pwd`,
`env`, `export`, `unset`, `path`, `alias`, `unalias`, `return`, and `hash`.
Original highlighted listing formats remain in `env`, `path`, `alias`, and
`hash`. `which -c` and `which --path-only` print binary paths alone while retaining
alias and builtin descriptions; `which -h` and `which --help` display help.
Combined short flags are accepted. `type` and `which -a` enumerate resolutions.

## Output and variables

- `echo` supports `-n`, `-e`, and `-E`, including combined flags. Escapes include
  control characters, octal bytes, hexadecimal bytes, Unicode values, and `\c`.
- `printf` supports integer, floating point, string, character, `%b`, and `%q`
  conversions, width and precision including `*`, format reuse, and `-v NAME`.
  Unsupported conversions such as `%n` produce diagnostics instead of exposing
  memory writes. Width and precision are bounded. Variables cannot contain NUL.
- `export`, `readonly`, `local`, `declare`, and `typeset` manage variable values
  and independent export/readonly attributes. Declarations support `-p`, `-x`,
  `-r`, and arithmetic `-i`; function declarations can be serialized with `-f`.
  `local` is limited to functions, and `declare -g` selects global variables.
- `unset -f` removes functions; `unset -v` removes variables. Readonly variables
  reject assignment and removal. `printenv` only reports exported variables.
- `env -i`, `env -u NAME`, and leading assignments execute in a child so the
  invoking shell retains its environment and variable attributes. `env -0` and
  `printenv -0` support NUL-delimited output.
- `set --` replaces positional arguments, `shift` removes a validated count,
  and `getopts` maintains `OPTIND`/`OPTARG` while parsing short option groups.
  A leading colon in the option specification enables silent errors.
- `read` handles `IFS`, quoted delimiters, backslash continuations, raw `-r`,
  prompts, silent terminal input, selected descriptors, custom delimiters,
  character counts, and timeouts. Without names it assigns `REPLY`. `-N` reads
  exact character counts without IFS splitting; timed-out reads return 142.

## Directories and files

`cd` supports `HOME`, `OLDPWD` through `cd -`, `CDPATH`, and logical/physical
navigation. Exported `PWD`/`OLDPWD` change only after successful navigation.
Readonly directory variables prevent navigation; failed access checks leave
working directory state unchanged. The command cache is cleared after moves.
`pushd`, `popd`, and `dirs` maintain an owned directory stack; `-n` performs stack
operations without navigation, and `dirs -c/-l/-p/-v` control display or clearing.

`basename`, `dirname`, `realpath`, `readlink`, and `stat` provide pathname and
metadata inspection. `realpath -m` normalizes lexically without requiring an
existing target; ordinary `realpath` resolves symlinks. `stat -c` supports name,
size, mode, type, owner, group, inode, links, device, blocks, and timestamp fields.
`mkdir -p/-m/-v`, `rmdir -p`, and `touch -c` perform explicit filesystem changes.

## Execution, conditions, and resources

`eval`, `source`, and `.` use the normal parser and execution engine. Sources
can receive temporary positional arguments. `return` unwinds a function or
source; at top level it retains the original mish status-producing behavior.
`break` and `continue` support nesting levels. `exit` accepts a validated numeric
status and defaults to the previous status; statuses are normalized to 8 bits.
`logout` requires a login shell.

`command` bypasses functions, `command -v/-V` describes resolution, and
`command -p` temporarily searches the standard binary path. `builtin` invokes a
builtin directly. `exec` replaces the process, supports an empty environment
with `-c` and a custom argument zero with `-a`, and retains bare-exec redirections.
`type`, `which`, `hash`, `alias`, and `unalias` inspect or change resolution;
`hash -r/-d/-t/-p/-l` supports clearing, deletion, lookup, insertion, and reusable
output. Replacing an alias replaces its previous value.

`test` and `[` implement string, decimal-integer, filesystem, descriptor, and
variable predicates, with negation, conjunction, disjunction, and parentheses.
`[` requires its closing `]`; malformed expressions return status 2. The `=~`
extension uses extended regular expressions. `assert` adds failure diagnostics;
`match` prints shell-pattern or regex matches. `let` evaluates the arithmetic
engine, while `expr` exposes its integer-expression subset. `true`, `false`, and
`:` provide constant statuses.

`trap` delegates signal registration and dispositions to the shell signal
manager. `jobs`, `fg`, `bg`, `wait`, and `kill` delegate to the job manager.
`times` reports accumulated CPU usage. `umask` accepts octal and symbolic masks;
`ulimit` supports soft/hard resource queries and changes using resource-specific
units. `sleep` accepts fractional durations and s/m/h/d suffixes.

## Streaming text commands

The library includes `cat`, `head`, `tail`, `wc`, `sort`, `uniq`, `cut`, `tr`, and
`seq` for common shell data pipelines. Each command documents its supported
options through `help`; these are focused implementations rather than claims of
complete GNU utility compatibility. In-memory sorting and retained tail records
are bounded by the shell source limit, and sequences are limited to ten million
items. `wc -m` counts UTF-8 leading bytes; malformed UTF-8 is not validated.
`cut -c` selects byte positions, matching `cut -b` in this implementation.
`tr` uses byte sets, ranges, escapes, and common POSIX character classes.

`help`, `version`, `clear`, `getconf`, and `commandline` provide inspection.
`history`, `complete`, `bind`, and `prompt` are dispatched to the interactive
subsystem. See `docs/interactive.md` for its supported completion and editing
behavior.

## Validation

`MISH_BIN=/absolute/path/to/mish python3 tests/builtins-runtime.py` exercises the real shell:
formatting, variable ownership and scope, option parsing, record input, control
flow, sources, cache changes, filesystem fixtures, permissions, predicates,
arithmetic, pipelines, stream utilities, resources, traps, and help diagnostics.
The test fixtures run as the invoking user and avoid changing the test process's
working directory or environment.
