#include "mish.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool is_executable(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode) && access(path, X_OK) == 0;
}

void cache_clear(Shell *shell)
{
    while (shell->cache) {
        CommandCache *next = shell->cache->next;
        free(shell->cache->name);
        free(shell->cache->path);
        free(shell->cache);
        shell->cache = next;
    }
}

static void cache_insert(Shell *shell, const char *name, const char *path)
{
    CommandCache *entry = xcalloc(1, sizeof(*entry));
    entry->name = xstrdup(name);
    entry->path = xstrdup(path);
    entry->hits = 1;
    entry->next = shell->cache;
    shell->cache = entry;
}

char *command_resolve(Shell *shell, const char *name, bool use_cache)
{
    if (!*name)
        return NULL;
    if (strchr(name, '/'))
        return is_executable(name) ? xstrdup(name) : NULL;
    if (use_cache) {
        CommandCache **link = &shell->cache;
        while (*link) {
            CommandCache *entry = *link;
            if (!strcmp(entry->name, name)) {
                if (is_executable(entry->path)) {
                    ++entry->hits;
                    return xstrdup(entry->path);
                }
                *link = entry->next;
                free(entry->name);
                free(entry->path);
                free(entry);
                break;
            }
            link = &entry->next;
        }
    }
    const char *paths = shell_get(shell, "PATH");
    if (!paths)
        return NULL;
    const char *start = paths;
    for (;;) {
        const char *colon = strchr(start, ':');
        char *directory = colon ? xstrndup(start, (size_t)(colon - start)) : xstrdup(start);
        char *candidate = path_join(*directory ? directory : ".", name);
        free(directory);
        if (is_executable(candidate)) {
            if (use_cache)
                cache_insert(shell, name, candidate);
            return candidate;
        }
        free(candidate);
        if (!colon)
            break;
        start = colon + 1;
    }
    return NULL;
}
