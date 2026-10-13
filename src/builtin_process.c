#include "mish.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Root implements job-aware external dispatch through this shared API. */
int shell_external(Shell *, int, char **, bool);

static int process_error(const char *name, const char *message)
{
    fprintf(stderr, "%s: %s\n", name, message);
    return 2;
}

static void restore_shell_signals(Shell *shell)
{
    shell_signals_init(shell);
    for (int number = 1; number < 128; ++number) {
        if (shell->traps[number] != NULL) {
            char *action = xstrdup(shell->traps[number]);
            shell_set_trap(shell, number, action);
            free(action);
        }
    }
}

int builtin_invoke(Shell *shell, int argc, char **argv, bool allow_builtin, bool replace)
{
    if (argc == 0) return 0;
    if (allow_builtin && builtin_exists(argv[0])) {
        return builtin_run(shell, argc, argv);
    }
    return shell_external(shell, argc, argv, replace);
}

static int exit_command(Shell *shell, int argc, char **argv)
{
    int64_t status = shell->last_status;
    if (argc > 2) return process_error(argv[0], "too many arguments");
    if (argc == 2 && parse_number(argv[1], &status) != 0) {
        fprintf(stderr, "%s: numeric argument required: %s\n", argv[0], argv[1]);
        return 2;
    }
    if (strcmp(argv[0], "logout") == 0 && !shell->login) {
        return process_error("logout", "not a login shell; use exit");
    }
    shell->flow = FLOW_EXIT;
    shell->flow_status = (int)((uint64_t)status & 255);
    return shell->flow_status;
}

static int flow_command(Shell *shell, int argc, char **argv)
{
    if (strcmp(argv[0], "return") == 0) {
        int64_t status = shell->last_status;
        if (argc > 2 || (argc == 2 && parse_number(argv[1], &status) != 0)) {
            return process_error("return", "expected one numeric status");
        }
        if (shell->function_depth == 0 && shell->source_depth == 0) {
            /* Keep mish's original status-producing return at top level. */
            return (int)((uint64_t)status & 255);
        }
        shell->flow = FLOW_RETURN;
        shell->flow_status = (int)((uint64_t)status & 255);
        return shell->flow_status;
    }
    int64_t levels = 1;
    if (argc > 2 || (argc == 2 && parse_number(argv[1], &levels) != 0) || levels < 1) {
        return process_error(argv[0], "expected a positive loop count");
    }
    if (shell->loop_depth == 0) return process_error(argv[0], "not inside a loop");
    if ((uint64_t)levels > shell->loop_depth) levels = shell->loop_depth;
    shell->flow = strcmp(argv[0], "break") == 0 ? FLOW_BREAK : FLOW_CONTINUE;
    shell->flow_level = (unsigned)levels;
    return 0;
}

static int eval_command(Shell *shell, int argc, char **argv)
{
    StrVec words;
    strvec_init(&words);
    for (int i = 1; i < argc; ++i) strvec_push(&words, argv[i]);
    char *source = strvec_join(&words, " ");
    strvec_free(&words);
    int status = shell_run_string(shell, source, "eval");
    free(source);
    return status;
}

static char *source_path(Shell *shell, const char *name)
{
    if (strchr(name, '/') != NULL) return xstrdup(name);
    const char *path = shell_get(shell, "PATH");
    if (path != NULL) {
        char *paths = xstrdup(path);
        char *cursor = paths;
        do {
            char *colon = strchr(cursor, ':');
            if (colon != NULL) *colon = '\0';
            char *candidate = path_join(cursor[0] == '\0' ? "." : cursor, name);
            struct stat info;
            if (stat(candidate, &info) == 0 && S_ISREG(info.st_mode) && access(candidate, R_OK) == 0) {
                free(paths);
                return candidate;
            }
            free(candidate);
            cursor = colon == NULL ? NULL : colon + 1;
        } while (cursor != NULL);
        free(paths);
    }
    return xstrdup(name);
}

static int source_command(Shell *shell, int argc, char **argv)
{
    int index = 1;
    if (index < argc && strcmp(argv[index], "--") == 0) ++index;
    if (index == argc) return process_error(argv[0], "missing filename");
    char *path = source_path(shell, argv[index]);
    int result = shell_run_file(shell, path, argc - index - 1, argv + index + 1);
    free(path);
    return result;
}

static int alias_command(Shell *shell, int argc, char **argv)
{
    int index = 1;
    if (index < argc && (strcmp(argv[index], "-p") == 0 || strcmp(argv[index], "--") == 0)) ++index;
    if (index == argc) {
        bool any = false;
        for (Alias *alias = shell->aliases; alias != NULL; alias = alias->next) {
            printf("%s=\"\033[1m%s\033[0m\"\n", alias->name, alias->value);
            any = true;
        }
        if (!any) puts("<no aliases set>");
        return 0;
    }
    int result = 0;
    for (; index < argc; ++index) {
        char *copy = xstrdup(argv[index]);
        char *equal = strchr(copy, '=');
        if (equal == NULL) {
            const char *value = alias_get(shell, copy);
            if (value == NULL) {
                fprintf(stderr, "alias: %s: not found\n", copy);
                result = 1;
            } else {
                char *quoted = shell_quote(value);
                printf("alias %s=%s\n", copy, quoted);
                free(quoted);
            }
        } else {
            *equal++ = '\0';
            if (copy[0] == '\0' || strpbrk(copy, " /\t\n$") != NULL || alias_set(shell, copy, equal) != 0) {
                fprintf(stderr, "alias: invalid name: %s\n", copy);
                result = 1;
            }
        }
        free(copy);
    }
    return result;
}

static int unalias_command(Shell *shell, int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "-a") == 0) {
        while (shell->aliases != NULL) {
            char *name = xstrdup(shell->aliases->name);
            alias_unset(shell, name);
            free(name);
        }
        return 0;
    }
    if (argc == 1) return process_error("unalias", "missing alias name");
    int result = 0;
    int index = argc > 1 && strcmp(argv[1], "--") == 0 ? 2 : 1;
    for (; index < argc; ++index) {
        if (alias_unset(shell, argv[index]) != 0) {
            fprintf(stderr, "unalias: %s: not found\n", argv[index]);
            result = 1;
        }
    }
    return result;
}

static void describe_command(Shell *shell, const char *name, bool short_form,
                             bool path_only, bool terse, bool which, int *result)
{
    const char *alias = path_only ? NULL : alias_get(shell, name);
    Function *function = path_only ? NULL : function_find(shell, name);
    if (alias != NULL) {
        if (terse) puts("alias");
        else if (short_form) puts(name);
        else if (which) printf("\"%s\" <aliased to> -> \"%s\"\n", name, alias);
        else printf("%s is aliased to `%s'\n", name, alias);
    } else if (function != NULL) {
        if (terse) puts("function");
        else if (short_form) puts(name);
        else printf("%s is a shell function\n", name);
    } else if (!path_only && builtin_exists(name)) {
        if (terse) puts("builtin");
        else if (short_form) puts(name);
        else if (which) printf("\"%s\" -> <built-in>\n", name);
        else printf("%s is a shell builtin\n", name);
    } else {
        char *path = command_resolve(shell, name, false);
        if (path == NULL) {
            if (!terse) fprintf(stderr, "%s: %s: not found\n", which ? "which" : "type", name);
            *result = 1;
        } else {
            if (terse) puts("file");
            else if (short_form || path_only) puts(path);
            else if (which) printf("\"%s\" -> \"%s\"\n", name, path);
            else printf("%s is %s\n", name, path);
            free(path);
        }
    }
}

static int describe_all(Shell *shell, const char *name, bool paths_only,
                        bool terse, bool which, bool bare_paths)
{
    bool found = false;
    if (!paths_only) {
        const char *alias = alias_get(shell, name);
        if (alias != NULL) {
            if (terse) puts("alias");
            else if (which) printf("\"%s\" <aliased to> -> \"%s\"\n", name, alias);
            else printf("%s is aliased to `%s'\n", name, alias);
            found = true;
        }
        Function *function = function_find(shell, name);
        if (function != NULL) {
            if (terse) puts("function");
            else printf("%s is a shell function\n", name);
            found = true;
        }
        if (builtin_exists(name)) {
            if (terse) puts("builtin");
            else if (which) printf("\"%s\" -> <built-in>\n", name);
            else printf("%s is a shell builtin\n", name);
            found = true;
        }
    }
    if (strchr(name, '/') != NULL) {
        char *path = command_resolve(shell, name, false);
        if (path != NULL) {
            if (terse) puts("file");
            else if (paths_only || bare_paths) puts(path);
            else if (which) printf("\"%s\" -> \"%s\"\n", name, path);
            else printf("%s is %s\n", name, path);
            free(path);
            found = true;
        }
    } else {
        const char *path = shell_get(shell, "PATH");
        char *paths = path == NULL ? NULL : xstrdup(path);
        char *cursor = paths;
        while (cursor != NULL) {
            char *colon = strchr(cursor, ':');
            if (colon != NULL) *colon = '\0';
            char *candidate = path_join(cursor[0] == '\0' ? "." : cursor, name);
            struct stat info;
            if (stat(candidate, &info) == 0 && S_ISREG(info.st_mode) && access(candidate, X_OK) == 0) {
                if (terse) puts("file");
                else if (paths_only || bare_paths) puts(candidate);
                else if (which) printf("\"%s\" -> \"%s\"\n", name, candidate);
                else printf("%s is %s\n", name, candidate);
                found = true;
            }
            free(candidate);
            cursor = colon == NULL ? NULL : colon + 1;
        }
        free(paths);
    }
    if (!found && !terse) fprintf(stderr, "%s: %s: not found\n", which ? "which" : "type", name);
    return found ? 0 : 1;
}

static int type_command(Shell *shell, int argc, char **argv)
{
    bool which = strcmp(argv[0], "which") == 0;
    bool short_form = false;
    bool path_only = false;
    bool terse = false;
    bool which_path = false;
    bool all = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        if (which && (strcmp(argv[index], "-h") == 0 || strcmp(argv[index], "--help") == 0)) {
            builtin_help("which", stdout);
            return 0;
        }
        if (which && strcmp(argv[index], "--path-only") == 0) which_path = true;
        else if (strcmp(argv[index], "-p") == 0 || strcmp(argv[index], "-P") == 0) { path_only = true; short_form = true; }
        else if (strcmp(argv[index], "-t") == 0 && !which) terse = true;
        else if (strcmp(argv[index], "-a") == 0) all = true;
        else if (which && argv[index][1] != '-' && argv[index][1] != '\0') {
            bool help = false;
            for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
                if (*p == 'c') which_path = true;
                else if (*p == 'a') all = true;
                else if (*p == 'h') help = true;
                else return process_error("which", "unknown option");
            }
            if (help) { builtin_help("which", stdout); return 0; }
        }
        else return process_error(argv[0], "unknown option");
        ++index;
    }
    if (index == argc) return process_error(argv[0], "missing command name");
    int result = 0;
    for (; index < argc; ++index) {
        if (all) {
            if (describe_all(shell, argv[index], path_only, terse, which, which_path) != 0) result = 1;
            continue;
        }
        bool executable = alias_get(shell, argv[index]) == NULL && !builtin_exists(argv[index]) && function_find(shell, argv[index]) == NULL;
        describe_command(shell, argv[index], short_form, path_only || (which_path && executable), terse, which, &result);
    }
    return result;
}

static int command_command(Shell *shell, int argc, char **argv)
{
    int index = 1;
    bool describe = false;
    bool short_form = false;
    bool default_path = false;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        if (strcmp(argv[index], "-v") == 0) { describe = true; short_form = true; }
        else if (strcmp(argv[index], "-V") == 0) describe = true;
        else if (strcmp(argv[index], "-p") == 0) default_path = true;
        else return process_error("command", "invalid option");
        ++index;
    }
    if (index == argc) return 0;
    int result = 0;
    if (describe) {
        for (; index < argc; ++index) describe_command(shell, argv[index], short_form, false, false, false, &result);
        return result;
    }
    char *oldpath = NULL;
    bool present = false;
    if (default_path) {
        const char *path = shell_get(shell, "PATH");
        present = path != NULL;
        oldpath = path == NULL ? NULL : xstrdup(path);
        if (shell_set(shell, "PATH", "/usr/bin:/bin") != 0) { free(oldpath); return 1; }
    }
    result = builtin_invoke(shell, argc - index, argv + index, true, false);
    if (default_path) {
        if (present) shell_set(shell, "PATH", oldpath);
        else shell_unset(shell, "PATH");
        free(oldpath);
    }
    return result;
}

static int exec_command(Shell *shell, int argc, char **argv)
{
    bool clear = false;
    const char *name = NULL;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        if (strcmp(argv[index], "-c") == 0) clear = true;
        else if (strcmp(argv[index], "-a") == 0 && index + 1 < argc) name = argv[++index];
        else return process_error("exec", "invalid option");
        ++index;
    }
    if (index == argc) return 0;
    if (!clear && name == NULL) return shell_external(shell, argc - index, argv + index, true);
    char *path = command_resolve(shell, argv[index], true);
    if (path == NULL) {
        fprintf(stderr, "exec: %s: command not found\n", argv[index]);
        return 127;
    }
    char **environment = clear ? xcalloc(1, sizeof(char *)) : shell_environment(shell);
    char **arguments = xcalloc((size_t)(argc - index + 1), sizeof(char *));
    for (int i = index; i < argc; ++i) arguments[i - index] = argv[i];
    if (name != NULL) arguments[0] = (char *)name;
    fflush(NULL);
    shell_signals_child();
    execve(path, arguments, environment);
    int saved = errno;
    fprintf(stderr, "exec: %s: %s\n", argv[index], strerror(saved));
    free(path);
    free(arguments);
    environment_free(environment);
    restore_shell_signals(shell);
    return saved == ENOENT ? 127 : 126;
}

static int hash_command(Shell *shell, int argc, char **argv)
{
    bool show = false;
    bool reusable = false;
    bool remove = false;
    const char *path = NULL;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "-r") == 0) cache_clear(shell);
        else if (strcmp(argv[index], "-t") == 0) show = true;
        else if (strcmp(argv[index], "-l") == 0) reusable = true;
        else if (strcmp(argv[index], "-d") == 0) remove = true;
        else if (strcmp(argv[index], "-p") == 0 && index + 1 < argc) path = argv[++index];
        else if (strcmp(argv[index], "--") == 0) { ++index; break; }
        else return process_error("hash", "invalid option");
        ++index;
    }
    if (index == argc) {
        if (argc > 1 && !reusable && !show) return 0;
        if (shell->cache == NULL) puts("<no hashes defined>");
        for (CommandCache *entry = shell->cache; entry != NULL; entry = entry->next) {
            if (reusable) {
                char *quoted = shell_quote(entry->path);
                printf("hash -p %s %s\n", quoted, entry->name);
                free(quoted);
            } else printf("%s=\033[1m%s\033[0m\n", entry->name, entry->path);
        }
        return 0;
    }
    int result = 0;
    for (; index < argc; ++index) {
        CommandCache **link = &shell->cache;
        while (*link != NULL && strcmp((*link)->name, argv[index]) != 0) link = &(*link)->next;
        if (remove) {
            if (*link == NULL) result = 1;
            else {
                CommandCache *entry = *link;
                *link = entry->next;
                free(entry->name);
                free(entry->path);
                free(entry);
            }
        } else if (path != NULL) {
            if (*link == NULL) {
                *link = xcalloc(1, sizeof(CommandCache));
                (*link)->name = xstrdup(argv[index]);
            }
            free((*link)->path);
            (*link)->path = xstrdup(path);
        } else if (show) {
            if (*link == NULL) { fprintf(stderr, "hash: %s: not found\n", argv[index]); result = 1; }
            else puts((*link)->path);
        } else {
            char *resolved = command_resolve(shell, argv[index], true);
            if (resolved == NULL) { fprintf(stderr, "hash: %s: not found\n", argv[index]); result = 1; }
            free(resolved);
        }
    }
    return result;
}

static int trap_command(Shell *shell, int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "-l") == 0) {
        for (int i = 1; i < 128; ++i) {
            const char *name = signal_name(i);
            if (name != NULL) printf("%d) %s\n", i, name);
        }
        return 0;
    }
    int index = 1;
    bool print = index == argc || strcmp(argv[index], "-p") == 0;
    if (print && index < argc) ++index;
    if (print) {
        for (int i = 0; i < 128; ++i) {
            bool selected = index == argc;
            for (int j = index; j < argc; ++j) if (signal_number(argv[j]) == i) selected = true;
            if (selected && shell->traps[i] != NULL) {
                char *quoted = shell_quote(shell->traps[i]);
                const char *name = signal_name(i);
                printf("trap -- %s %s\n", quoted, name == NULL ? "EXIT" : name);
                free(quoted);
            }
        }
        return 0;
    }
    if (strcmp(argv[index], "--") == 0) ++index;
    if (index == argc) return process_error("trap", "missing action");
    const char *action = argv[index++];
    if (index == argc) return process_error("trap", "missing signal");
    int result = 0;
    for (; index < argc; ++index) {
        int number = signal_number(argv[index]);
        if (number < 0 || number >= 128 || shell_set_trap(shell, number, strcmp(action, "-") == 0 ? NULL : action) != 0) {
            fprintf(stderr, "trap: invalid or uncatchable signal: %s\n", argv[index]);
            result = 1;
        }
    }
    return result;
}

static int umask_command(int argc, char **argv)
{
    bool symbolic = false;
    bool reusable = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "-S") == 0) symbolic = true;
        else if (strcmp(argv[index], "-p") == 0) reusable = true;
        else return process_error("umask", "invalid option");
        ++index;
    }
    mode_t mask = umask(0);
    umask(mask);
    if (index == argc) {
        if (reusable) fputs("umask ", stdout);
        if (!symbolic) printf("%04o\n", (unsigned)mask);
        else {
            mode_t allowed = (mode_t)~mask;
            for (int who = 0; who < 3; ++who) {
                printf("%c=", "ugo"[who]);
                unsigned value = (unsigned)(allowed >> ((2 - who) * 3)) & 7;
                if (value & 4) putchar('r');
                if (value & 2) putchar('w');
                if (value & 1) putchar('x');
                putchar(who == 2 ? '\n' : ',');
            }
        }
        return 0;
    }
    if (argc - index != 1) return process_error("umask", "too many arguments");
    const char *text = argv[index];
    if (text[0] == '\0') return process_error("umask", "empty mask");
    if (text[0] >= '0' && text[0] <= '7') {
        char *end;
        unsigned long value = strtoul(text, &end, 8);
        if (*end != '\0' || value > 0777) return process_error("umask", "invalid octal mask");
        umask((mode_t)value);
        return 0;
    }
    mode_t allowed = (mode_t)(~mask & 0777);
    const char *cursor = text;
    while (*cursor != '\0') {
        unsigned who = 0;
        while (*cursor != '\0' && strchr("ugoa", *cursor) != NULL) {
            if (*cursor == 'u') who |= 0700;
            if (*cursor == 'g') who |= 0070;
            if (*cursor == 'o') who |= 0007;
            if (*cursor == 'a') who |= 0777;
            ++cursor;
        }
        if (who == 0) who = 0777;
        char operation = *cursor++;
        if (operation != '=' && operation != '+' && operation != '-') return process_error("umask", "invalid symbolic mask");
        unsigned permission = 0;
        while (*cursor != '\0' && *cursor != ',') {
            if (*cursor == 'r') permission |= 0444;
            else if (*cursor == 'w') permission |= 0222;
            else if (*cursor == 'x') permission |= 0111;
            else return process_error("umask", "invalid permission");
            ++cursor;
        }
        permission &= who;
        if (operation == '=') allowed = (mode_t)((allowed & ~who) | permission);
        else if (operation == '+') allowed = (mode_t)(allowed | permission);
        else allowed = (mode_t)(allowed & ~permission);
        if (*cursor == ',') {
            ++cursor;
            if (*cursor == '\0') return process_error("umask", "trailing comma");
        }
    }
    umask((mode_t)(~allowed & 0777));
    return 0;
}

typedef struct {
    char option;
    int resource;
    unsigned long scale;
    const char *description;
} Limit;

static const Limit limits[] = {
    { 't', RLIMIT_CPU, 1, "cpu time (seconds)" },
    { 'f', RLIMIT_FSIZE, 1024, "file size (kilobytes)" },
    { 'd', RLIMIT_DATA, 1024, "data size (kilobytes)" },
    { 's', RLIMIT_STACK, 1024, "stack size (kilobytes)" },
    { 'c', RLIMIT_CORE, 1024, "core size (kilobytes)" },
    { 'n', RLIMIT_NOFILE, 1, "open files" },
#ifdef RLIMIT_AS
    { 'v', RLIMIT_AS, 1024, "virtual memory (kilobytes)" },
#endif
#ifdef RLIMIT_RSS
    { 'm', RLIMIT_RSS, 1024, "resident memory (kilobytes)" },
#endif
#ifdef RLIMIT_NPROC
    { 'u', RLIMIT_NPROC, 1, "processes" },
#endif
#ifdef RLIMIT_MEMLOCK
    { 'l', RLIMIT_MEMLOCK, 1024, "locked memory (kilobytes)" },
#endif
#ifdef RLIMIT_LOCKS
    { 'x', RLIMIT_LOCKS, 1, "file locks" },
#endif
#ifdef RLIMIT_SIGPENDING
    { 'i', RLIMIT_SIGPENDING, 1, "pending signals" },
#endif
#ifdef RLIMIT_MSGQUEUE
    { 'q', RLIMIT_MSGQUEUE, 1, "message queue bytes" },
#endif
#ifdef RLIMIT_NICE
    { 'e', RLIMIT_NICE, 1, "scheduling priority" },
#endif
#ifdef RLIMIT_RTPRIO
    { 'r', RLIMIT_RTPRIO, 1, "real time priority" },
#endif
};

static int ulimit_command(int argc, char **argv)
{
    bool hard = false;
    bool soft = false;
    bool all = false;
    const Limit *selected = &limits[1];
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            if (*p == 'H') hard = true;
            else if (*p == 'S') soft = true;
            else if (*p == 'a') all = true;
            else {
                selected = NULL;
                for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
                    if (limits[i].option == *p) selected = &limits[i];
                }
                if (selected == NULL) return process_error("ulimit", "invalid resource option");
            }
        }
        ++index;
    }
    if (argc - index > 1 || (all && index < argc)) return process_error("ulimit", "invalid operands");
    if (index == argc) {
        for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
            const Limit *item = &limits[i];
            if (!all && item != selected) continue;
            struct rlimit limit;
            if (getrlimit(item->resource, &limit) != 0) return process_error("ulimit", strerror(errno));
            rlim_t value = hard && !soft ? limit.rlim_max : limit.rlim_cur;
            if (all) printf("%-28s (-%c) ", item->description, item->option);
            if (value == RLIM_INFINITY) puts("unlimited");
            else printf("%llu\n", (unsigned long long)(value / item->scale));
        }
        return 0;
    }
    struct rlimit limit;
    if (getrlimit(selected->resource, &limit) != 0) return process_error("ulimit", strerror(errno));
    rlim_t value;
    if (strcmp(argv[index], "unlimited") == 0) value = RLIM_INFINITY;
    else if (strcmp(argv[index], "soft") == 0) value = limit.rlim_cur;
    else if (strcmp(argv[index], "hard") == 0) value = limit.rlim_max;
    else {
        int64_t number;
        if (parse_number(argv[index], &number) != 0 || number < 0 ||
            (uint64_t)number > (uint64_t)RLIM_INFINITY / selected->scale) return process_error("ulimit", "invalid limit");
        value = (rlim_t)((uint64_t)number * selected->scale);
    }
    if (!hard && !soft) hard = soft = true;
    if (soft) limit.rlim_cur = value;
    if (hard) limit.rlim_max = value;
    if (setrlimit(selected->resource, &limit) != 0) return process_error("ulimit", strerror(errno));
    return 0;
}

static void print_ticks(clock_t ticks, long frequency)
{
    double seconds = (double)ticks / (double)frequency;
    unsigned long minutes = (unsigned long)(seconds / 60.0);
    printf("%lum%.3fs", minutes, seconds - (double)minutes * 60.0);
}

static int times_command(void)
{
    struct tms usage;
    if (times(&usage) == (clock_t)-1) return process_error("times", strerror(errno));
    long frequency = sysconf(_SC_CLK_TCK);
    if (frequency <= 0) frequency = 100;
    print_ticks(usage.tms_utime, frequency);
    putchar(' ');
    print_ticks(usage.tms_stime, frequency);
    putchar('\n');
    print_ticks(usage.tms_cutime, frequency);
    putchar(' ');
    print_ticks(usage.tms_cstime, frequency);
    putchar('\n');
    return 0;
}

static int sleep_command(int argc, char **argv)
{
    if (argc < 2) return process_error("sleep", "missing duration");
    double total = 0;
    for (int i = 1; i < argc; ++i) {
        char *end;
        errno = 0;
        double value = strtod(argv[i], &end);
        if (end == argv[i] || errno != 0 || !isfinite(value) || value < 0) return process_error("sleep", "invalid duration");
        if (*end == 'm' && end[1] == '\0') value *= 60;
        else if (*end == 'h' && end[1] == '\0') value *= 3600;
        else if (*end == 'd' && end[1] == '\0') value *= 86400;
        else if (*end != '\0' && !(*end == 's' && end[1] == '\0')) return process_error("sleep", "invalid duration suffix");
        total += value;
        if (!isfinite(total) || total > 2147483647) return process_error("sleep", "duration too large");
    }
    struct timespec duration = { .tv_sec = (time_t)total, .tv_nsec = (long)((total - (double)(time_t)total) * 1000000000.0) };
    while (nanosleep(&duration, &duration) != 0) {
        if (errno != EINTR) return process_error("sleep", strerror(errno));
    }
    return 0;
}

static int let_command(Shell *shell, int argc, char **argv)
{
    if (argc < 2) return process_error("let", "missing expression");
    int64_t value = 0;
    for (int i = 1; i < argc; ++i) {
        char *error = NULL;
        if (arithmetic_eval(shell, argv[i], &value, &error) != 0) {
            fprintf(stderr, "let: %s\n", error == NULL ? "invalid arithmetic" : error);
            free(error);
            return 2;
        }
    }
    return value == 0 ? 1 : 0;
}

int builtin_process_run(Shell *shell, int argc, char **argv)
{
    const char *name = argv[0];
    if (strcmp(name, "exit") == 0 || strcmp(name, "logout") == 0) return exit_command(shell, argc, argv);
    if (strcmp(name, "return") == 0 || strcmp(name, "break") == 0 || strcmp(name, "continue") == 0) return flow_command(shell, argc, argv);
    if (strcmp(name, "eval") == 0) return eval_command(shell, argc, argv);
    if (strcmp(name, "source") == 0 || strcmp(name, ".") == 0) return source_command(shell, argc, argv);
    if (strcmp(name, "alias") == 0) return alias_command(shell, argc, argv);
    if (strcmp(name, "unalias") == 0) return unalias_command(shell, argc, argv);
    if (strcmp(name, "type") == 0 || strcmp(name, "which") == 0) return type_command(shell, argc, argv);
    if (strcmp(name, "command") == 0) return command_command(shell, argc, argv);
    if (strcmp(name, "exec") == 0) return exec_command(shell, argc, argv);
    if (strcmp(name, "builtin") == 0) {
        int index = argc > 1 && strcmp(argv[1], "--") == 0 ? 2 : 1;
        if (index == argc) return 0;
        if (!builtin_exists(argv[index])) return process_error("builtin", "not a shell builtin");
        return builtin_run(shell, argc - index, argv + index);
    }
    if (strcmp(name, "hash") == 0) return hash_command(shell, argc, argv);
    if (strcmp(name, "trap") == 0) return trap_command(shell, argc, argv);
    if (strcmp(name, "umask") == 0) return umask_command(argc, argv);
    if (strcmp(name, "ulimit") == 0) return ulimit_command(argc, argv);
    if (strcmp(name, "times") == 0) return times_command();
    if (strcmp(name, "sleep") == 0) return sleep_command(argc, argv);
    if (strcmp(name, "let") == 0) return let_command(shell, argc, argv);
    if (strcmp(name, "true") == 0 || strcmp(name, ":") == 0) return 0;
    if (strcmp(name, "false") == 0) return 1;
    return -1;
}
