#include "mish.h"
#include <string.h>

int builtin_fs_run(Shell *, int, char **);
int builtin_data_run(Shell *, int, char **);
int builtin_process_run(Shell *, int, char **);
int builtin_test_run(Shell *, int, char **);
int builtin_help_run(Shell *, int, char **);
int builtin_text_run(Shell *, int, char **);

static const char *const names[] = {
    "exit", "which", "echo", "cd", "pwd", "env", "export", "unset",
    "path", "alias", "unalias", "return", "hash", "printf", "pushd",
    "popd", "dirs", "readonly", "local", "declare", "typeset", "set",
    "shift", "shopt", "getopts", "read", "eval", "source", ".", "break",
    "continue", "exec", "command", "builtin", "type", "true", "false",
    ":", "test", "[", "let", "times", "umask", "ulimit", "trap", "kill",
    "jobs", "fg", "bg", "wait", "help", "version", "history", "complete",
    "bind", "prompt", "basename", "dirname", "realpath", "stat", "sleep",
    "printenv", "clear", "logout", "getconf", "readlink", "touch", "mkdir",
    "rmdir", "cat", "head", "tail", "wc", "sort", "uniq", "cut", "tr",
    "seq", "expr", "match", "assert", "commandline", "variables"
};

const char *const *builtin_names(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(names) / sizeof(names[0]);
    }
    return names;
}

bool builtin_exists(const char *name)
{
    size_t count;
    const char *const *list = builtin_names(&count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(name, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool builtin_special(const char *name)
{
    static const char *const special[] = {
        ":", ".", "break", "continue", "eval", "exec", "exit", "export",
        "readonly", "return", "set", "shift", "trap", "unset"
    };
    for (size_t i = 0; i < sizeof(special) / sizeof(special[0]); ++i) {
        if (strcmp(name, special[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int builtin_dispatch(Shell *shell, int argc, char **argv)
{
    if (argc == 0 || argv == NULL || argv[0] == NULL) {
        return -1;
    }
    if (strcmp(argv[0], "history") == 0) {
        return history_builtin(shell, argc, argv);
    }
    if (strcmp(argv[0], "complete") == 0) {
        return complete_builtin(shell, argc, argv);
    }
    if (strcmp(argv[0], "bind") == 0) {
        return bind_builtin(shell, argc, argv);
    }
    if (strcmp(argv[0], "prompt") == 0) {
        return prompt_builtin(shell, argc, argv);
    }
    if (strcmp(argv[0], "jobs") == 0) {
        return jobs_builtin(shell, argc, argv);
    }
    if (strcmp(argv[0], "fg") == 0) {
        return fg_builtin(shell, argc, argv);
    }
    if (strcmp(argv[0], "bg") == 0) {
        return bg_builtin(shell, argc, argv);
    }
    if (strcmp(argv[0], "wait") == 0) {
        return wait_builtin(shell, argc, argv);
    }
    if (strcmp(argv[0], "kill") == 0) {
        return kill_builtin(shell, argc, argv);
    }
    int result = builtin_fs_run(shell, argc, argv);
    if (result != -1) {
        return result;
    }
    result = builtin_data_run(shell, argc, argv);
    if (result != -1) {
        return result;
    }
    result = builtin_process_run(shell, argc, argv);
    if (result != -1) {
        return result;
    }
    result = builtin_test_run(shell, argc, argv);
    if (result != -1) {
        return result;
    }
    result = builtin_text_run(shell, argc, argv);
    if (result != -1) {
        return result;
    }
    return builtin_help_run(shell, argc, argv);
}

int builtin_run(Shell *shell, int argc, char **argv)
{
    if (shell->execution_depth >= MISH_MAX_DEPTH) {
        fputs("mish: builtin nesting limit exceeded\n", stderr);
        return 2;
    }
    ++shell->execution_depth;
    int result = builtin_dispatch(shell, argc, argv);
    --shell->execution_depth;
    return result;
}
