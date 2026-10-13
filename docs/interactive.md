# Interactive editing, completion, prompts, and history

When standard input and standard error are terminals, mish uses its built-in
line editor. It enables raw input and bracketed paste while reading a command,
then restores the original terminal settings before execution, cancellation,
EOF, or normal shutdown. No readline library is required.

The editor uses byte offsets at UTF-8 character boundaries. Movement, deletion,
transposition, and undo preserve whole UTF-8 characters. Display width follows
the active locale, including wide and combining characters. This is character
editing rather than grapheme-cluster editing: a combining mark can be edited
independently. ASCII case conversion leaves non-ASCII text intact.

## Editing keys

| Keys | Action |
| --- | --- |
| Left / Right, Ctrl-B / Ctrl-F | Move one character |
| Home / End, Ctrl-A / Ctrl-E | Beginning / end of input |
| Alt-B / Alt-F, Ctrl-Left / Ctrl-Right | Move by words |
| Backspace / Ctrl-H | Delete the preceding character |
| Delete | Delete the character under the cursor |
| Ctrl-D | Delete under the cursor; on empty input, EOF |
| Ctrl-W / Alt-Backspace | Kill the preceding word |
| Alt-D | Kill the following word |
| Ctrl-U / Ctrl-K | Kill to beginning / end |
| Ctrl-Y | Yank the last killed text |
| Ctrl-_ / Alt-Ctrl-_ | Undo / redo |
| Ctrl-T / Alt-T | Transpose characters / words |
| Alt-U / Alt-L / Alt-C | Uppercase / lowercase / capitalize the next ASCII word |
| Up / Down, Ctrl-P / Ctrl-N | Previous / next history entry |
| Page Up / Page Down, Alt-< / Alt-> | First history entry / current draft |
| Ctrl-R / Ctrl-S | Incremental backward / forward history search |
| Tab | Complete the current word or extend its common prefix |
| Repeated Tab / Alt-? | List possible completions |
| Ctrl-L | Clear the screen and redraw |
| Ctrl-C | Cancel the current input and set status 130 |
| Enter | Accept input |
| Alt-R | Revert the current input |
| Alt-# | Prefix a comment and accept it |

Incremental search accepts typed text, Backspace, and repeated Ctrl-R/Ctrl-S to
find the next match. Enter executes the selected command, Escape returns to
editing, and Ctrl-G restores the input present before the search.

Bracketed paste inserts the entire pasted text into the input buffer. Embedded
newlines do not execute until Enter is pressed. CRLF paste becomes a single
newline, NUL bytes are ignored, and the source-size limit also bounds paste.
Control characters in input are displayed with caret notation, so pasted escape
sequences do not become terminal commands. The editor notices terminal size
changes and redraws wrapped input.

`set -o vi` enables a compact vi-style normal mode. Escape leaves insertion;
`i`, `a`, `I`, `A`, `s`, and `C` resume it. Normal mode supports `h/l`, `j/k`,
`0/$`, `b/w`, `x/X`, `D`, `u`, Ctrl-R, `p/P`, `dd`, `dw`, `db`, `d0`, `d$`,
and their `c` variants. Numeric counts repeat actions. This is a small editing
mode, not a complete vi implementation. `set -o emacs` selects the default mode.

## Custom bindings

`bind -l` lists actions; `bind -p` prints reusable binding definitions. Bind a
key to an editing action with one `KEY: ACTION` argument:

```text
bind '\C-x: beginning-of-line'
bind '"\e[15~": clear-screen'
bind -r '\C-x'
```

Keys accept `\C-a`, `\M-f`, `\e`, octal escapes, and caret notation such as
`^A`. Custom key sequences can contain multiple bytes. Ambiguous prefixes use a
short timeout; longer sequences should arrive together. `bind -r` disables a
binding by assigning `ignore`. Actions are editor functions, not shell commands.
Bindings remain local to the shell session; put definitions in the startup file
to persist them.

## Completion

Command-position completion includes builtins, aliases, functions, and
executable files from `PATH`. An explicit path completes executable files and
directories. Other argument positions complete filenames; `cd`, `pushd`, and
`rmdir` complete directories. Completion omits hidden filenames unless the
prefix starts with `.`. File names are shell-escaped and open single/double
quotes are respected. Directories end in `/`; a unique non-directory completion
adds a space. Variable completion recognizes `$NAME`, `${NAME}`, and `$(NAME)`.

Programmable completion supports static word lists and name categories:

```text
complete -W 'start stop status' service
complete -A directory project
complete -A variable inspect
complete -P '--' -W 'verbose quiet' tool
complete -p service
complete -r service
```

`-A` accepts `command`, `builtin`, `file`, `directory`, `variable`, `alias`, and
`function`. `-P` and `-S` decorate results with a prefix or suffix. Word lists
accept shell quoting. Candidate generation performs no command substitution
and runs no user functions. `complete -r` without command names clears all rules.
Configuration remains local to the shell session.

## History

The default file is `$HOME/.mish_history`, overridden by `HISTFILE` at startup. History is
loaded for interactive sessions. `HISTSIZE` bounds in-memory entries;
`HISTFILESIZE` bounds stored entries. Defaults are 1000 and the maximum is
100000. Zero disables retention. The following `HISTCONTROL` options may be
colon-separated: `ignorespace`, `ignoredups`, `ignoreboth`, and `erasedups`.
`HISTIGNORE` accepts colon-separated filename-style patterns; `&` matches the
previous entry.

History files must be regular files owned by the current user with no group or
other permissions. Symlinks are refused. Writers serialize using a private
`.lock` sidecar, write a private temporary file, and atomically replace the
history file. The length-framed `# mish history v1` format preserves multiline
commands. Private newline-delimited history files can be imported too.

```text
history                 # List retained entries
history 20              # List the last twenty
history -c              # Clear in-memory history
history -d 3            # Delete entry three; negative offsets count from end
history -s 'echo saved'  # Add a command without executing it
history -r file         # Read and add entries
history -w file         # Replace a file with current history
history -a file         # Append entries not previously saved
```

Numbers are one-based within the retained history list. Removing old entries
renumbers this list. `-w` intentionally replaces history; `-a` preserves records
saved by other shell sessions. Lock files remain on disk so concurrent writers
always lock the same inode.

The saved-entry cursor tracks the primary history file (or the first file used
by a noninteractive shell). Exporting to an alternate file does not mark entries
as saved to the primary file. `-a` to an alternate file exports the retained list;
there is no separate saved-entry cursor for every filename.

History expansion is interactive and occurs before parsing. Single quotes and
backslash escapes protect literal `!`. Supported events are `!!`, `!n`, `!-n`,
`!prefix`, `!?substring?`, and `!#` (the line prefix expanded so far).
`^old^new^` replaces the first match in the previous event.

Word designators include `:0`, `:^`, `:$`, `:*`, `:n`, `:n-m`, `:n*`, and
`:n-`. The short forms `!^`, `!$`, and `!*` use the previous event. Additional
modifiers are `:p` (print without execution), `:q` (quote as one word), `:h`
(directory), `:t` (basename), `:r` (remove extension), `:e` (extension),
`:s/old/new/`, and `:gs/old/new/`. Substitution replacement `&` inserts the
matched text; backslash escapes it. An unknown event or malformed modifier
produces an error and executes nothing.

## Prompts

`PS1` controls the primary prompt. Use `prompt` to inspect it, `prompt TEXT` to
set it, `prompt --preview [TEXT]` to expand without setting, and `prompt --reset`
to select `\u@\h:\w\$ `.

| Escape | Meaning |
| --- | --- |
| `\u` | User name |
| `\h` / `\H` | Short / full host name |
| `\w` / `\W` | Directory / basename; home becomes `~` |
| `\$` | `$` for ordinary users or `#` for root |
| `\!` / `\#` | Next retained history number / command number |
| `\?` | Previous command status |
| `\s`, `\v`, `\V` | Shell name / version |
| `\t`, `\T`, `\@`, `\A` | Time in 24h, 12h, 12h with AM/PM, HH:MM |
| `\d`, `\D{FORMAT}` | Date / strftime date format |
| `\e`, `\a`, `\n`, `\r`, `\\` | ESC, bell, newline, carriage return, backslash |
| `\nnn` | An octal byte |
| `\[` / `\]` | Zero-width prompt markers; ANSI sequences are measured directly |

Prompt text also supports normal shell variables and command substitutions.
Literal quote marks remain display text. Prompt expansion preserves the
previous status and shell flow state. For example:

```text
prompt '\[\e[36m\]\u@\h:\w\[\e[0m\]\$ '
```

## Verification

```sh
python3 tests/interactive-state.py bin/mish
python3 tests/interactive-pty.py bin/mish
```

The PTY suite creates a controlling terminal for the real shell. It checks input
editing, UTF-8 movement, history/search, cancellation/EOF, paste timing,
completion, programmable bindings, prompt expansion, resize, persistence, and
restoration of the terminal's exact original modes. Both suites isolate HOME,
working directories, environment, and history files and bound child execution.
