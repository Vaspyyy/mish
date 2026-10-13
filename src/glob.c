#include "mish.h"
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GLOB_LIMIT 65536
#define GLOB_DEPTH 64

typedef struct {
    Shell *shell;
    StrVec components;
    StrVec *results;
    char **error;
    bool absolute;
    bool trailing_slash;
} GlobWalk;

static int compare_paths(const void *left, const void *right)
{
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static bool is_directory(const char *path, bool follow)
{
    struct stat info;
    int status = follow ? stat(path, &info) : lstat(path, &info);
    return status == 0 && S_ISDIR(info.st_mode);
}

static int append_match(GlobWalk *walk, const char *path)
{
    if (walk->results->len >= GLOB_LIMIT) {
        *walk->error = xstrdup("pathname expansion exceeds 65536 matches");
        return 1;
    }
    if (!*path)
        return 0;
    if (walk->trailing_slash) {
        if (!is_directory(path, true))
            return 0;
        strvec_push_owned(walk->results, str_printf("%s/", path));
    } else {
        strvec_push(walk->results, path);
    }
    return 0;
}

static int walk_pattern(GlobWalk *walk, const char *prefix, size_t component, unsigned depth)
{
    if (depth >= GLOB_DEPTH) {
        *walk->error = xstrdup("pathname recursion depth exceeded");
        return 1;
    }
    if (component == walk->components.len)
        return append_match(walk, prefix);
    const char *pattern = walk->components.items[component];
    bool recursive = walk->shell->globstar && !strcmp(pattern, "**");
    bool final = component + 1 == walk->components.len;
    if (!strpbrk(pattern, "*?[")) {
        Buffer literal;
        buffer_init(&literal);
        for (size_t i = 0; pattern[i]; ++i) {
            if (pattern[i] == '\\' && pattern[i + 1])
                ++i;
            buffer_putc(&literal, pattern[i]);
        }
        char *path = path_join(prefix, literal.data ? literal.data : "");
        buffer_free(&literal);
        int result = 0;
        struct stat info;
        if (final) {
            if (!stat(path, &info))
                result = append_match(walk, path);
        } else if (is_directory(path, true)) {
            result = walk_pattern(walk, path, component + 1, depth + 1);
        }
        free(path);
        return result;
    }
    if (recursive && !final) {
        if (walk_pattern(walk, prefix, component + 1, depth + 1))
            return 1;
    }
    const char *directory = *prefix ? prefix : ".";
    DIR *stream = opendir(directory);
    if (!stream)
        return 0;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(stream))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        if (*name == '.' && !walk->shell->dotglob && pattern[0] != '.')
            continue;
        int flags = walk->shell->dotglob ? 0 : FNM_PERIOD;
        if (!recursive && fnmatch(pattern, name, flags))
            continue;
        char *path = path_join(prefix, name);
        if (recursive) {
            if (final)
                result = append_match(walk, path);
            if (!result && is_directory(path, false))
                result = walk_pattern(walk, path, component, depth + 1);
        } else if (final) {
            result = append_match(walk, path);
        } else if (is_directory(path, true)) {
            result = walk_pattern(walk, path, component + 1, depth + 1);
        }
        free(path);
        if (result)
            break;
    }
    closedir(stream);
    return result;
}

static void split_components(const char *pattern, StrVec *components)
{
    const char *start = pattern;
    while (*start == '/')
        ++start;
    while (*start) {
        const char *slash = strchr(start, '/');
        size_t length = slash ? (size_t)(slash - start) : strlen(start);
        if (length)
            strvec_push_owned(components, xstrndup(start, length));
        if (!slash)
            break;
        start = slash + 1;
        while (*start == '/')
            ++start;
    }
}

static void deduplicate_matches(StrVec *results, size_t first)
{
    if (results->len <= first + 1)
        return;
    qsort(results->items + first, results->len - first, sizeof(char *), compare_paths);
    size_t destination = first;
    for (size_t i = first; i < results->len; ++i) {
        if (destination > first && !strcmp(results->items[destination - 1], results->items[i])) {
            free(results->items[i]);
        } else {
            results->items[destination++] = results->items[i];
        }
    }
    results->len = destination;
    results->items[destination] = NULL;
}

int expand_glob(Shell *shell, const char *pattern, StrVec *results, char **error)
{
    *error = NULL;
    size_t first = results->len;
    int status = 0;
    if (shell->globstar || shell->dotglob) {
        GlobWalk walk;
        memset(&walk, 0, sizeof(walk));
        walk.shell = shell;
        walk.results = results;
        walk.error = error;
        walk.absolute = pattern[0] == '/';
        size_t length = strlen(pattern);
        walk.trailing_slash = length && pattern[length - 1] == '/';
        strvec_init(&walk.components);
        split_components(pattern, &walk.components);
        status = walk_pattern(&walk, walk.absolute ? "/" : "", 0, 0);
        strvec_free(&walk.components);
        if (!status)
            deduplicate_matches(results, first);
    } else {
        glob_t matches;
        memset(&matches, 0, sizeof(matches));
        int result = glob(pattern, 0, NULL, &matches);
        if (!result) {
            if (matches.gl_pathc > GLOB_LIMIT) {
                *error = xstrdup("pathname expansion exceeds 65536 matches");
                status = 1;
            } else {
                for (size_t i = 0; i < matches.gl_pathc; ++i)
                    strvec_push(results, matches.gl_pathv[i]);
            }
        } else if (result != GLOB_NOMATCH) {
            *error = xstrdup("pathname expansion failed");
            status = 1;
        }
        globfree(&matches);
    }
    if (!status && results->len == first && shell->failglob) {
        *error = str_printf("no pathname matches: %s", pattern);
        status = 1;
    }
    return status;
}
