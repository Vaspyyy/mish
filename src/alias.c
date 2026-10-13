#include "mish.h"
#include <stdlib.h>
#include <string.h>

const char *alias_get(const Shell *shell, const char *name)
{
    for (const Alias *alias = shell->aliases; alias; alias = alias->next) {
        if (!strcmp(alias->name, name))
            return alias->value;
    }
    return NULL;
}

int alias_set(Shell *shell, const char *name, const char *value)
{
    if (!name || !*name || strpbrk(name, " \t\n=/;|&(){}"))
        return 1;
    for (Alias *alias = shell->aliases; alias; alias = alias->next) {
        if (!strcmp(alias->name, name)) {
            char *copy = xstrdup(value);
            free(alias->value);
            alias->value = copy;
            return 0;
        }
    }
    Alias *alias = xcalloc(1, sizeof(*alias));
    alias->name = xstrdup(name);
    alias->value = xstrdup(value);
    alias->next = shell->aliases;
    shell->aliases = alias;
    return 0;
}

int alias_unset(Shell *shell, const char *name)
{
    Alias **link = &shell->aliases;
    while (*link) {
        Alias *alias = *link;
        if (!strcmp(alias->name, name)) {
            *link = alias->next;
            free(alias->name);
            free(alias->value);
            free(alias);
            return 0;
        }
        link = &alias->next;
    }
    return 1;
}

Function *function_find(Shell *shell, const char *name)
{
    for (Function *function = shell->functions; function; function = function->next) {
        if (!strcmp(function->name, name))
            return function;
    }
    return NULL;
}

int function_set(Shell *shell, const char *name, const Node *body)
{
    if (!valid_name(name))
        return 1;
    Node *copy = node_clone(body);
    Function *function = function_find(shell, name);
    if (!function) {
        function = xcalloc(1, sizeof(*function));
        function->name = xstrdup(name);
        function->next = shell->functions;
        shell->functions = function;
    }
    node_free(function->body);
    function->body = copy;
    return 0;
}

int function_unset(Shell *shell, const char *name)
{
    Function **link = &shell->functions;
    while (*link) {
        Function *function = *link;
        if (!strcmp(function->name, name)) {
            *link = function->next;
            free(function->name);
            node_free(function->body);
            free(function);
            return 0;
        }
        link = &function->next;
    }
    return 1;
}
