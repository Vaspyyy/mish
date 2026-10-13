#include "mish.h"
#include <errno.h>
#include <fnmatch.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *name;
    size_t offset;
    const char *description;
} Option;

#define OPTION(name, field, description) { name, offsetof(Shell, field), description }
static const Option options[] = {
    OPTION("allexport", allexport, "Export every variable assignment."),
    OPTION("errexit", errexit, "Exit when an untested command fails."),
    OPTION("nounset", nounset, "Reject expansion of unset variables."),
    OPTION("noglob", noglob, "Disable pathname wildcard expansion."),
    OPTION("xtrace", xtrace, "Print expanded commands before execution."),
    OPTION("noexec", noexec, "Parse input without executing commands."),
    OPTION("pipefail", pipefail, "Use the rightmost failed pipeline status."),
    OPTION("noclobber", noclobber, "Protect existing files from output redirection."),
    OPTION("monitor", monitor, "Enable foreground process group management."),
    OPTION("verbose", verbose, "Print shell input as it is read."),
    OPTION("history", history_enabled, "Record commands in interactive history."),
    OPTION("ignoreeof", ignoreeof, "Require exit rather than interactive EOF."),
    OPTION("posix", posix, "Select POSIX compatibility behavior."),
    OPTION("vi", vi_mode, "Use vi-style line editing."),
    OPTION("emacs", emacs_mode, "Use emacs-style line editing."),
    OPTION("globstar", globstar, "Enable recursive double-star pathname patterns."),
    OPTION("braceexpand", braceexpand, "Expand brace lists and numeric ranges."),
    OPTION("nullglob", nullglob, "Remove unmatched pathname patterns."),
    OPTION("failglob", failglob, "Reject unmatched pathname patterns."),
    OPTION("dotglob", dotglob, "Include hidden names in pathname patterns.")
};
#undef OPTION

int builtin_option_state(Shell *shell, const char *name, bool *enabled)
{
    if (strcmp(name, "hashall") == 0) {
        *enabled = true;
        return 0;
    }
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        if (strcmp(options[i].name, name) != 0) continue;
        const bool *value = (const bool *)((const unsigned char *)shell + options[i].offset);
        *enabled = *value;
        return 0;
    }
    return 1;
}

int shell_option(Shell *shell, const char *name, bool enabled)
{
    if (strcmp(name, "hashall") == 0) {
        if (!enabled) cache_clear(shell);
        return 0;
    }
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        if (strcmp(options[i].name, name) != 0) continue;
        bool *value = (bool *)((unsigned char *)shell + options[i].offset);
        *value = enabled;
        if (enabled && strcmp(name, "vi") == 0) shell->emacs_mode = false;
        if (enabled && strcmp(name, "emacs") == 0) shell->vi_mode = false;
        return 0;
    }
    fprintf(stderr, "set: unknown shell option: %s\n", name);
    return 1;
}

void shell_print_options(Shell *shell, FILE *stream, bool reusable)
{
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        const bool *value = (const bool *)((const unsigned char *)shell + options[i].offset);
        if (reusable) fprintf(stream, "set %so %s\n", *value ? "-" : "+", options[i].name);
        else fprintf(stream, "%-16s %s\n", options[i].name, *value ? "on" : "off");
    }
}

typedef struct {
    const char *name;
    const char *usage;
    const char *description;
} HelpEntry;

static const HelpEntry help[] = {
    { "exit", "exit [STATUS]", "Exit this shell with STATUS, defaulting to the last command status." },
    { "logout", "logout [STATUS]", "Exit a login shell. Use exit in a non-login shell." },
    { "which", "which [-c|--path-only] [-h|--help] COMMAND ...", "Identify aliases, functions, builtins, and executable paths. -c prints binary paths alone." },
    { "echo", "echo [-n] [-e|-E] [TEXT ...]", "Print arguments separated by spaces. -n omits the newline; -e interprets backslash escapes." },
    { "printf", "printf [-v NAME] FORMAT [ARGUMENT ...]", "Format arguments using string, integer, floating point, character, %b escape and %q quote conversions. Reuse FORMAT for remaining arguments." },
    { "cd", "cd [-L|-P] [DIRECTORY|-]", "Change the working directory. Default to HOME; - uses OLDPWD. Search CDPATH for relative names and maintain exported PWD/OLDPWD." },
    { "pwd", "pwd [-L|-P]", "Print a verified logical working directory or the physical directory with -P." },
    { "pushd", "pushd [-n] [DIRECTORY]", "Push the current directory and change to DIRECTORY. With no directory, exchange the current and top stack directories." },
    { "popd", "popd [-n] [+N|-N]", "Remove a directory stack entry. Removing the current entry changes to the next directory unless -n is supplied." },
    { "dirs", "dirs [-clpv] [+N|-N]", "Display the directory stack. -c clears it, -l prints full paths, -p prints lines, and -v numbers entries." },
    { "env", "env [-i] [-0] [-u NAME] [NAME=VALUE ...] [COMMAND ...]", "Print exported variables or run a command with environment changes confined to a child process." },
    { "printenv", "printenv [-0] [NAME ...]", "Print exported variable values, or all exported NAME=VALUE pairs. Return failure for missing names." },
    { "export", "export [-np] [NAME[=VALUE] ...]", "Set values and export attributes. -n removes export attributes and -p prints declarations." },
    { "readonly", "readonly [-p] [NAME[=VALUE] ...]", "Prevent further assignment or removal of named variables." },
    { "local", "local [-xri] NAME[=VALUE] ...", "Create variables scoped to the current function. Restore their previous values and attributes when the scope ends." },
    { "declare", "declare [-pxrifg] [NAME[=VALUE] ...]", "Inspect or create variable declarations. -x exports, -r protects, -i evaluates arithmetic values, and -f lists functions." },
    { "typeset", "typeset [-pxrifg] [NAME[=VALUE] ...]", "An alternative spelling of declare." },
    { "variables", "variables [NAME ...]", "Display shell variable values using reusable quoting." },
    { "unset", "unset [-v|-f] [NAME ...]", "Remove variables or functions. Readonly variables remain protected." },
    { "set", "set [-aefmnuvxC] [-o OPTION] [-- ARGUMENT ...]", "Inspect variables, change shell options, and replace positional arguments. set -o lists options; set +o prints reusable settings." },
    { "shift", "shift [COUNT]", "Remove COUNT positional arguments, defaulting to one. Reject counts outside the available argument range." },
    { "shopt", "shopt [-suqp] [OPTION ...]", "Set, unset, query, or display shell options. -q performs a status-only query; -p prints reusable declarations." },
    { "getopts", "getopts OPTSTRING NAME [ARGUMENT ...]", "Parse short option groups using OPTIND and OPTARG. A leading colon requests silent error handling." },
    { "read", "read [-rs] [-p PROMPT] [-d DELIMITER] [-n|-N COUNT] [-t TIMEOUT] [-u FD] [NAME ...]", "Read one input record, split using IFS, and assign variables or REPLY. -r preserves backslashes; timeout returns 142." },
    { "eval", "eval [ARGUMENT ...]", "Join arguments with spaces and execute them as shell source in the current shell." },
    { "source", "source FILE [ARGUMENT ...]", "Execute FILE in the current shell, with temporary positional arguments when supplied. Search PATH for names without slashes." },
    { ".", ". FILE [ARGUMENT ...]", "An alternative spelling of source." },
    { "return", "return [STATUS]", "Return from a function or sourced file. At top level preserve mish's status-producing return behavior." },
    { "break", "break [LEVELS]", "Leave the requested number of enclosing loops." },
    { "continue", "continue [LEVELS]", "Begin the next iteration of the requested enclosing loop." },
    { "exec", "exec [-c] [-a NAME] [COMMAND ...]", "Replace the shell process with COMMAND. -c supplies an empty environment; -a overrides argv[0]. Without COMMAND, retain redirections." },
    { "command", "command [-p] [-v|-V] [COMMAND ...]", "Execute a command without function lookup, or describe its resolution. -p uses /usr/bin:/bin temporarily." },
    { "builtin", "builtin COMMAND [ARGUMENT ...]", "Invoke a shell builtin directly, bypassing functions of the same name." },
    { "type", "type [-ptP] COMMAND ...", "Classify names as aliases, functions, builtins, or files. -t prints the class, -p/-P requests executable paths." },
    { "hash", "hash [-rld] [-t NAME ...] [-p PATH NAME] [NAME ...]", "Inspect and populate the command cache. -r clears it, -d removes entries, -t prints paths, and -l prints reusable commands." },
    { "alias", "alias [-p] [NAME[=VALUE] ...]", "Define, replace, query, or list command aliases. Expansion applies to unquoted literal command-position words during execution." },
    { "unalias", "unalias [-a] NAME ...", "Remove named aliases, or all aliases with -a." },
    { "path", "path", "Enumerate executable regular files in PATH, highlighting their names." },
    { "true", "true", "Return a successful status." },
    { "false", "false", "Return status one." },
    { ":", ": [ARGUMENT ...]", "Perform expansions and redirections and return success." },
    { "test", "test EXPRESSION", "Evaluate file, string, integer, descriptor, and variable predicates. Combine expressions using !, -a, -o, and quoted parentheses." },
    { "[", "[ EXPRESSION ]", "Evaluate a test expression, requiring a final closing bracket." },
    { "assert", "assert EXPRESSION", "Evaluate a test expression and print a diagnostic when the assertion fails." },
    { "match", "match [-r] PATTERN STRING ...", "Print strings matching a shell wildcard pattern. -r uses extended regular expressions and prints matching capture groups." },
    { "let", "let EXPRESSION ...", "Evaluate integer arithmetic expressions including assignments. Return one when the last value is zero." },
    { "times", "times", "Display accumulated user and system CPU time for the shell and its completed children." },
    { "umask", "umask [-Sp] [MASK]", "Display or set the process file creation mask using octal or symbolic permissions." },
    { "ulimit", "ulimit [-HS] [-a|-RESOURCE] [LIMIT]", "Inspect or set process resource limits. LIMIT can be numeric, unlimited, soft, or hard." },
    { "trap", "trap [-lp] [ACTION SIGNAL ...]", "Register signal or EXIT handlers. An empty action ignores a signal; - restores its default shell disposition." },
    { "kill", "kill [-s SIGNAL|-SIGNAL] PID|%JOB ...", "Send a signal to a process or job. -l lists signal names." },
    { "jobs", "jobs [-lp]", "Display background and stopped jobs; -l includes process ids and -p prints process group ids." },
    { "fg", "fg [%JOB]", "Continue a job in the foreground and wait for completion or a stop." },
    { "bg", "bg [%JOB]", "Continue a stopped job in the background." },
    { "wait", "wait [PID|%JOB ...]", "Wait for background processes or jobs and return the final status." },
    { "history", "history [-c|-d OFFSET|-r|-w|-a [FILE]] [COUNT]", "Inspect, clear, remove, load, write, or append interactive command history." },
    { "complete", "complete [-r|-p] [-W WORDS] [-A ACTION] [-P PREFIX] [-S SUFFIX] COMMAND ...", "Configure completion rules for command names and inspect existing rules." },
    { "bind", "bind [-l|-p|-r KEY] [KEY:ACTION]", "Inspect or configure line editor key bindings." },
    { "prompt", "prompt [--preview|--reset|TEXT]", "Set or preview the interactive shell prompt." },
    { "help", "help [-dms] [PATTERN ...]", "List builtin commands or print usage and descriptions for names matching patterns." },
    { "version", "version", "Print the mish version and implementation information." },
    { "basename", "basename [-az] [-s SUFFIX] PATH [SUFFIX]", "Remove directory components and an optional filename suffix." },
    { "dirname", "dirname [-z] PATH ...", "Print parent directory names. -z terminates records with NUL." },
    { "realpath", "realpath [-emz] PATH ...", "Resolve physical absolute paths. -m performs lexical normalization without requiring existence." },
    { "stat", "stat [-L] [-c FORMAT] FILE ...", "Inspect file metadata. Formats include %n name, %s size, %a mode, %F type, %u/%g owner, %i inode and %X/%Y/%Z times." },
    { "readlink", "readlink [-nf] PATH ...", "Print symbolic link targets, or canonical paths with -f." },
    { "touch", "touch [-c] FILE ...", "Update access and modification times, creating absent files unless -c is supplied." },
    { "mkdir", "mkdir [-pv] [-m OCTAL_MODE] DIRECTORY ...", "Create directories. -p creates parents and accepts existing directories; -v reports new directories." },
    { "rmdir", "rmdir [-p] DIRECTORY ...", "Remove empty directories, optionally removing empty parents." },
    { "cat", "cat [-nbsETv] [FILE ...]", "Copy files to stdout with optional numbering, blank-line squeezing, and visible control characters." },
    { "head", "head [-n COUNT|-c COUNT] [FILE ...]", "Print the first COUNT lines or bytes, defaulting to ten lines." },
    { "tail", "tail [-n COUNT|-c COUNT] [FILE ...]", "Print the last COUNT lines or bytes; +COUNT starts at that line or byte." },
    { "wc", "wc [-lwcmL] [FILE ...]", "Count lines, words, bytes, UTF-8 code points, or maximum line length." },
    { "sort", "sort [-nruf] [FILE ...]", "Sort input lines lexically or numerically, optionally reversing, folding case, or suppressing duplicate lines." },
    { "uniq", "uniq [-cdui] [INPUT [OUTPUT]]", "Filter adjacent duplicate lines, optionally counting or selecting duplicates and unique records." },
    { "cut", "cut [-s] [-d DELIMITER] [-f|-b|-c LIST] [FILE ...]", "Select one-based fields or byte positions using individual numbers, closed ranges, and open-ended ranges." },
    { "tr", "tr [-dsc] SET1 [SET2]", "Translate, delete, or squeeze bytes. Sets support escapes, ranges, and common POSIX character classes." },
    { "seq", "seq [-w] [-s SEPARATOR] [FIRST [INCREMENT]] LAST", "Print a finite numeric sequence with optional equal-width numbers or a custom separator." },
    { "expr", "expr EXPRESSION ...", "Evaluate an integer arithmetic expression using the shell arithmetic engine." },
    { "sleep", "sleep DURATION ...", "Pause for the sum of durations. Suffixes s, m, h, and d select time units." },
    { "clear", "clear", "Clear the terminal display and move the cursor home." },
    { "getconf", "getconf NAME", "Query supported POSIX system configuration values such as ARG_MAX, OPEN_MAX, and PAGESIZE." },
    { "commandline", "commandline [--status|--positional]", "Print the invocation name, last command status, or quoted positional arguments." }
};

void builtin_help(const char *name, FILE *stream)
{
    for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); ++i) {
        if (strcmp(help[i].name, name) == 0) {
            fprintf(stream, "%s\n    %s\n", help[i].usage, help[i].description);
            return;
        }
    }
    fprintf(stream, "%s: no builtin help available\n", name);
}

static int help_command(int argc, char **argv)
{
    bool brief = false;
    bool description = false;
    bool manual = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "-s") == 0) brief = true;
        else if (strcmp(argv[index], "-d") == 0) description = true;
        else if (strcmp(argv[index], "-m") == 0) manual = true;
        else { fprintf(stderr, "help: invalid option: %s\n", argv[index]); return 2; }
        ++index;
    }
    if (index == argc) {
        printf("mish %s builtin commands:\n", MISH_VERSION);
        for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); ++i) printf("  %s\n", help[i].usage);
        fputs("Use help NAME for descriptions and help set for shell options.\n", stdout);
        return 0;
    }
    bool any = false;
    for (; index < argc; ++index) {
        bool matched = false;
        for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); ++i) {
            if (fnmatch(argv[index], help[i].name, 0) != 0) continue;
            any = matched = true;
            if (description) printf("%s - %s\n", help[i].name, help[i].description);
            else if (brief) puts(help[i].usage);
            else if (manual) printf("NAME\n    %s\nSYNOPSIS\n    %s\nDESCRIPTION\n    %s\n\n", help[i].name, help[i].usage, help[i].description);
            else builtin_help(help[i].name, stdout);
        }
        if (!matched) fprintf(stderr, "help: no builtin matches %s\n", argv[index]);
    }
    return any ? 0 : 1;
}

static int getconf_command(int argc, char **argv)
{
    static const struct { const char *name; int value; } configuration[] = {
        { "ARG_MAX", _SC_ARG_MAX }, { "CHILD_MAX", _SC_CHILD_MAX },
        { "CLK_TCK", _SC_CLK_TCK }, { "NGROUPS_MAX", _SC_NGROUPS_MAX },
        { "OPEN_MAX", _SC_OPEN_MAX }, { "STREAM_MAX", _SC_STREAM_MAX },
        { "TZNAME_MAX", _SC_TZNAME_MAX }, { "VERSION", _SC_VERSION },
        { "PAGESIZE", _SC_PAGESIZE }, { "PAGE_SIZE", _SC_PAGESIZE },
        { "JOB_CONTROL", _SC_JOB_CONTROL }, { "SAVED_IDS", _SC_SAVED_IDS },
        { "HOST_NAME_MAX", _SC_HOST_NAME_MAX }, { "LOGIN_NAME_MAX", _SC_LOGIN_NAME_MAX },
        { "RE_DUP_MAX", _SC_RE_DUP_MAX }, { "LINE_MAX", _SC_LINE_MAX },
        { "BC_BASE_MAX", _SC_BC_BASE_MAX }, { "BC_DIM_MAX", _SC_BC_DIM_MAX },
        { "BC_SCALE_MAX", _SC_BC_SCALE_MAX }, { "BC_STRING_MAX", _SC_BC_STRING_MAX }
    };
    if (argc != 2) { fputs("getconf: usage: getconf NAME\n", stderr); return 2; }
    for (size_t i = 0; i < sizeof(configuration) / sizeof(configuration[0]); ++i) {
        if (strcmp(argv[1], configuration[i].name) != 0) continue;
        errno = 0;
        long value = sysconf(configuration[i].value);
        if (value == -1 && errno != 0) { fprintf(stderr, "getconf: %s\n", strerror(errno)); return 1; }
        if (value == -1) puts("undefined");
        else printf("%ld\n", value);
        return 0;
    }
    fprintf(stderr, "getconf: unknown configuration variable: %s\n", argv[1]);
    return 2;
}

int builtin_help_run(Shell *shell, int argc, char **argv)
{
    if (strcmp(argv[0], "help") == 0) return help_command(argc, argv);
    if (strcmp(argv[0], "version") == 0) { printf("mish %s\nPOSIX C shell with original mish expansion syntax\n", MISH_VERSION); return 0; }
    if (strcmp(argv[0], "clear") == 0) { fputs("\033[2J\033[H", stdout); return ferror(stdout) ? 1 : 0; }
    if (strcmp(argv[0], "getconf") == 0) return getconf_command(argc, argv);
    if (strcmp(argv[0], "commandline") == 0) {
        if (argc == 1) puts(shell->argv0);
        else if (argc == 2 && strcmp(argv[1], "--status") == 0) printf("%d\n", shell->last_status);
        else if (argc == 2 && strcmp(argv[1], "--positional") == 0) {
            for (size_t i = 0; i < shell->positional_count; ++i) {
                char *quoted = shell_quote(shell->positional[i]);
                puts(quoted);
                free(quoted);
            }
        } else { fputs("commandline: invalid option\n", stderr); return 2; }
        return 0;
    }
    return -1;
}
