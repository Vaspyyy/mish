#include "mish.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

typedef struct SavedVar {
    char *name;
    char *value;
    bool existed;
    bool exported;
    bool readonly;
    struct SavedVar *next;
} SavedVar;

struct Scope {
    SavedVar *saved;
    Scope *previous;
};

Var *shell_find_var(Shell *shell, const char *name)
{
    for (Var *variable = shell->variables; variable; variable = variable->next) {
        if (!strcmp(variable->name, name))
            return variable;
    }
    return NULL;
}

const char *shell_get(const Shell *shell, const char *name)
{
    for (const Var *variable = shell->variables; variable; variable = variable->next) {
        if (!strcmp(variable->name, name))
            return variable->value;
    }
    return NULL;
}

int shell_set(Shell *shell, const char *name, const char *value)
{
    if (!valid_name(name)) {
        fprintf(stderr, "mish: invalid variable name: %s\n", name);
        return 1;
    }
    Var *variable = shell_find_var(shell, name);
    if (variable && variable->readonly) {
        fprintf(stderr, "mish: %s: readonly variable\n", name);
        return 1;
    }
    if (!variable) {
        variable = xcalloc(1, sizeof(*variable));
        variable->name = xstrdup(name);
        variable->next = shell->variables;
        shell->variables = variable;
    }
    char *copy = xstrdup(value);
    free(variable->value);
    variable->value = copy;
    if (shell->allexport)
        variable->exported = true;
    if (!strcmp(name, "PATH"))
        cache_clear(shell);
    return 0;
}

int shell_export(Shell *shell, const char *name, const char *value)
{
    Var *variable = shell_find_var(shell, name);
    if (value || !variable) {
        if (shell_set(shell, name, value ? value : ""))
            return 1;
        variable = shell_find_var(shell, name);
    }
    variable->exported = true;
    return 0;
}

int shell_unset(Shell *shell, const char *name)
{
    Var **link = &shell->variables;
    while (*link) {
        Var *variable = *link;
        if (!strcmp(variable->name, name)) {
            if (variable->readonly) {
                fprintf(stderr, "mish: %s: readonly variable\n", name);
                return 1;
            }
            *link = variable->next;
            free(variable->name);
            free(variable->value);
            free(variable);
            if (!strcmp(name, "PATH"))
                cache_clear(shell);
            return 0;
        }
        link = &variable->next;
    }
    return 0;
}

char **shell_environment(const Shell *shell)
{
    StrVec environment;
    strvec_init(&environment);
    for (const Var *variable = shell->variables; variable; variable = variable->next) {
        if (variable->exported)
            strvec_push_owned(&environment, str_printf("%s=%s", variable->name, variable->value));
    }
    return strvec_take(&environment);
}

void environment_free(char **environment)
{
    if (!environment)
        return;
    for (size_t i = 0; environment[i]; ++i)
        free(environment[i]);
    free(environment);
}

void shell_set_positional(Shell *shell, size_t count, char *const *values)
{
    char **copy = xcalloc(count + 1, sizeof(char *));
    for (size_t i = 0; i < count; ++i)
        copy[i] = xstrdup(values[i]);
    environment_free(shell->positional);
    shell->positional = copy;
    shell->positional_count = count;
}

void shell_push_scope(Shell *shell)
{
    Scope *scope = xcalloc(1, sizeof(*scope));
    scope->previous = shell->scopes;
    shell->scopes = scope;
}

int shell_local(Shell *shell, const char *name, const char *value)
{
    if (!shell->scopes) {
        fprintf(stderr, "local: not in a function\n");
        return 1;
    }
    for (SavedVar *saved = shell->scopes->saved; saved; saved = saved->next) {
        if (!strcmp(saved->name, name))
            return value ? shell_set(shell, name, value) : 0;
    }
    SavedVar *saved = xcalloc(1, sizeof(*saved));
    saved->name = xstrdup(name);
    Var *variable = shell_find_var(shell, name);
    if (variable) {
        saved->existed = true;
        saved->value = xstrdup(variable->value);
        saved->exported = variable->exported;
        saved->readonly = variable->readonly;
    }
    saved->next = shell->scopes->saved;
    shell->scopes->saved = saved;
    return shell_set(shell, name, value ? value : (variable ? variable->value : ""));
}

void shell_pop_scope(Shell *shell)
{
    Scope *scope = shell->scopes;
    if (!scope)
        return;
    SavedVar *saved = scope->saved;
    while (saved) {
        SavedVar *next = saved->next;
        Var *variable = shell_find_var(shell, saved->name);
        if (variable)
            variable->readonly = false;
        if (saved->existed) {
            (void)shell_set(shell, saved->name, saved->value);
            variable = shell_find_var(shell, saved->name);
            variable->exported = saved->exported;
            variable->readonly = saved->readonly;
        } else {
            (void)shell_unset(shell, saved->name);
        }
        free(saved->name);
        free(saved->value);
        free(saved);
        saved = next;
    }
    shell->scopes = scope->previous;
    free(scope);
}

int shell_init(Shell *shell, const char *argv0)
{
    memset(shell, 0, sizeof(*shell));
    shell->argv0 = xstrdup(argv0);
    shell->source_name = xstrdup(argv0);
    shell->shell_pid = getpid();
    shell->shell_pgid = getpgrp();
    shell->terminal_fd = STDIN_FILENO;
    shell->next_job_id = 1;
    shell->history_enabled = true;
    shell->emacs_mode = true;
    shell->braceexpand = true;
    strvec_init(&shell->directory_stack);
    strvec_init(&shell->expanding_aliases);
    for (char **entry = environ; entry && *entry; ++entry) {
        char *equal = strchr(*entry, '=');
        if (!equal)
            continue;
        char *name = xstrndup(*entry, (size_t)(equal - *entry));
        if (valid_name(name))
            (void)shell_export(shell, name, equal + 1);
        free(name);
    }
    if (!shell_get(shell, "PATH"))
        shell_set(shell, "PATH", "/usr/local/bin:/usr/bin:/bin");
    if (!shell_get(shell, "IFS"))
        shell_set(shell, "IFS", " \t\n");
    if (!shell_get(shell, "PS1"))
        shell_set(shell, "PS1", "\\u@\\h:\\w\\$ ");
    if (!shell_get(shell, "PS2"))
        shell_set(shell, "PS2", "> ");
    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
        fprintf(stderr, "mish: current directory: %s\n", strerror(errno));
        return 1;
    }
    shell_export(shell, "PWD", cwd);
    free(cwd);
    const char *level = shell_get(shell, "SHLVL");
    int64_t value = 0;
    if (level && parse_number(level, &value))
        value = 0;
    if (value < 0 || value > 1000)
        value = 0;
    char *next_level = str_printf("%lld", (long long)value + 1);
    shell_export(shell, "SHLVL", next_level);
    free(next_level);
    shell_set_positional(shell, 0, NULL);
    return 0;
}

void shell_destroy(Shell *shell)
{
    editor_reset_terminal();
    history_destroy(shell);
    editor_state_destroy(shell);
    completion_state_destroy(shell);
    while (shell->scopes)
        shell_pop_scope(shell);
    while (shell->variables) {
        Var *next = shell->variables->next;
        free(shell->variables->name);
        free(shell->variables->value);
        free(shell->variables);
        shell->variables = next;
    }
    while (shell->aliases) {
        Alias *next = shell->aliases->next;
        free(shell->aliases->name);
        free(shell->aliases->value);
        free(shell->aliases);
        shell->aliases = next;
    }
    while (shell->functions) {
        Function *next = shell->functions->next;
        free(shell->functions->name);
        node_free(shell->functions->body);
        free(shell->functions);
        shell->functions = next;
    }
    while (shell->jobs)
        job_remove(shell, shell->jobs);
    cache_clear(shell);
    strvec_free(&shell->directory_stack);
    strvec_free(&shell->expanding_aliases);
    environment_free(shell->positional);
    for (size_t i = 0; i < 128; ++i)
        free(shell->traps[i]);
    free(shell->argv0);
    free(shell->source_name);
    memset(shell, 0, sizeof(*shell));
}
