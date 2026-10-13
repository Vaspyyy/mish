#define _XOPEN_SOURCE 700
#include "mish.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fs_error(const char *command, const char *path)
{
    fprintf(stderr, "%s: %s: %s\n", command, path, strerror(errno));
    return 1;
}

static char *current_directory(void)
{
    size_t size = 256;
    for (;;) {
        char *result = xmalloc(size);
        if (getcwd(result, size) != NULL) {
            return result;
        }
        int saved = errno;
        free(result);
        if (saved != ERANGE || size > MISH_MAX_SOURCE) {
            errno = saved;
            return NULL;
        }
        size *= 2;
    }
}

static char *logical_directory(const char *base, const char *path)
{
    char *joined = path[0] == '/' ? xstrdup(path) : path_join(base, path);
    StrVec parts;
    strvec_init(&parts);
    char *save = NULL;
    char *part = strtok_r(joined, "/", &save);
    while (part != NULL) {
        if (strcmp(part, "..") == 0) {
            if (parts.len > 0) {
                free(parts.items[--parts.len]);
            }
        } else if (strcmp(part, ".") != 0 && part[0] != '\0') {
            strvec_push(&parts, part);
        }
        part = strtok_r(NULL, "/", &save);
    }
    char *suffix = strvec_join(&parts, "/");
    char *result = str_printf("/%s", suffix);
    free(suffix);
    free(joined);
    strvec_free(&parts);
    return result;
}

static int change_directory(Shell *shell, const char *target, bool physical,
                            bool print, bool search)
{
    Var *pwd = shell_find_var(shell, "PWD");
    Var *oldpwd = shell_find_var(shell, "OLDPWD");
    if ((pwd != NULL && pwd->readonly) || (oldpwd != NULL && oldpwd->readonly)) {
        fputs("cd: PWD or OLDPWD is readonly\n", stderr);
        return 1;
    }
    char *old = current_directory();
    if (old == NULL) {
        const char *previous = shell_get(shell, "PWD");
        old = xstrdup(previous == NULL ? "/" : previous);
    }
    char *destination = NULL;
    bool from_cdpath = false;
    const char *cdpath = shell_get(shell, "CDPATH");
    if (search && cdpath != NULL && target[0] != '/' &&
        strncmp(target, "./", 2) != 0 && strncmp(target, "../", 3) != 0 &&
        strcmp(target, ".") != 0 && strcmp(target, "..") != 0) {
        char *paths = xstrdup(cdpath);
        char *cursor = paths;
        do {
            char *colon = strchr(cursor, ':');
            if (colon != NULL) {
                *colon = '\0';
            }
            char *candidate = path_join(cursor[0] == '\0' ? "." : cursor, target);
            struct stat info;
            if (stat(candidate, &info) == 0 && S_ISDIR(info.st_mode) &&
                access(candidate, X_OK) == 0) {
                destination = candidate;
                from_cdpath = cursor[0] != '\0';
                break;
            }
            free(candidate);
            cursor = colon == NULL ? NULL : colon + 1;
        } while (cursor != NULL);
        free(paths);
    }
    if (destination == NULL) {
        destination = xstrdup(target);
    }
    const char *logical_pwd = shell_get(shell, "PWD");
    struct stat logical_info;
    struct stat actual_info;
    bool verified_pwd = logical_pwd != NULL && logical_pwd[0] == '/' &&
        stat(logical_pwd, &logical_info) == 0 && stat(old, &actual_info) == 0 &&
        logical_info.st_dev == actual_info.st_dev && logical_info.st_ino == actual_info.st_ino;
    const char *base = verified_pwd ? logical_pwd : old;
    char *logical = logical_directory(base, destination);
    const char *move = physical ? destination : logical;
    if (chdir(move) != 0) {
        int result = fs_error("cd", target);
        free(old);
        free(destination);
        free(logical);
        return result;
    }
    char *newpwd = physical ? current_directory() : xstrdup(logical);
    if (newpwd == NULL) {
        fputs("cd: unable to determine new working directory\n", stderr);
        free(old);
        free(destination);
        free(logical);
        return 1;
    }
    shell_export(shell, "OLDPWD", old);
    shell_export(shell, "PWD", newpwd);
    cache_clear(shell);
    if (print || from_cdpath) {
        puts(newpwd);
    }
    free(old);
    free(destination);
    free(logical);
    free(newpwd);
    return 0;
}

static int cd_command(Shell *shell, int argc, char **argv)
{
    bool physical = false;
    int index = 1;
    while (index < argc) {
        if (strcmp(argv[index], "-P") == 0) {
            physical = true;
        } else if (strcmp(argv[index], "-L") == 0) {
            physical = false;
        } else if (strcmp(argv[index], "--") == 0) {
            ++index;
            break;
        } else {
            break;
        }
        ++index;
    }
    if (argc - index > 1) {
        fputs("cd: too many arguments\n", stderr);
        return 2;
    }
    const char *target = index == argc ? shell_get(shell, "HOME") : argv[index];
    bool print = false;
    bool search = index != argc;
    if (target != NULL && strcmp(target, "-") == 0) {
        target = shell_get(shell, "OLDPWD");
        print = true;
        search = false;
        if (target == NULL) {
            fputs("cd: OLDPWD is not set\n", stderr);
            return 1;
        }
    }
    if (target == NULL) {
        fputs("cd: HOME is not set\n", stderr);
        return 1;
    }
    return change_directory(shell, target, physical, print, search);
}

static int pwd_command(Shell *shell, int argc, char **argv)
{
    bool physical = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-P") == 0) {
            physical = true;
        } else if (strcmp(argv[i], "-L") == 0) {
            physical = false;
        } else if (strcmp(argv[i], "--") != 0) {
            fprintf(stderr, "pwd: invalid option: %s\n", argv[i]);
            return 2;
        }
    }
    const char *logical = shell_get(shell, "PWD");
    char *actual = current_directory();
    if (actual == NULL) {
        return fs_error("pwd", ".");
    }
    struct stat left;
    struct stat right;
    if (!physical && logical != NULL && logical[0] == '/' &&
        stat(logical, &left) == 0 && stat(actual, &right) == 0 &&
        left.st_dev == right.st_dev && left.st_ino == right.st_ino) {
        puts(logical);
    } else {
        puts(actual);
    }
    free(actual);
    return 0;
}

static void print_directory(Shell *shell, const char *path, bool full)
{
    const char *home = shell_get(shell, "HOME");
    size_t length = home == NULL ? 0 : strlen(home);
    if (!full && length > 0 && strncmp(path, home, length) == 0 &&
        (path[length] == '/' || path[length] == '\0')) {
        printf("~%s", path + length);
    } else {
        fputs(path, stdout);
    }
}

static int dirs_command(Shell *shell, int argc, char **argv)
{
    bool lines = false;
    bool numbered = false;
    bool full = false;
    int selection = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0) {
            strvec_free(&shell->directory_stack);
            strvec_init(&shell->directory_stack);
            return 0;
        }
        if (strcmp(argv[i], "-p") == 0) {
            lines = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            numbered = true;
            lines = true;
        } else if (strcmp(argv[i], "-l") == 0) {
            full = true;
        } else if (argv[i][0] == '+' || argv[i][0] == '-') {
            int64_t number;
            if (parse_number(argv[i] + 1, &number) != 0 || number < 0 ||
                (uint64_t)number > shell->directory_stack.len) {
                fprintf(stderr, "dirs: invalid stack index: %s\n", argv[i]);
                return 1;
            }
            selection = argv[i][0] == '+' ? (int)number :
                (int)(shell->directory_stack.len - (size_t)number);
        } else {
            fprintf(stderr, "dirs: invalid argument: %s\n", argv[i]);
            return 2;
        }
    }
    char *cwd = current_directory();
    if (cwd == NULL) {
        return fs_error("dirs", ".");
    }
    for (size_t i = 0; i <= shell->directory_stack.len; ++i) {
        if (selection >= 0 && i != (size_t)selection) {
            continue;
        }
        if (numbered) {
            printf("%zu  ", i);
        }
        print_directory(shell, i == 0 ? cwd : shell->directory_stack.items[i - 1], full);
        if (lines || selection >= 0 || i == shell->directory_stack.len) {
            putchar('\n');
        } else {
            putchar(' ');
        }
    }
    free(cwd);
    return 0;
}

static int pushd_command(Shell *shell, int argc, char **argv)
{
    bool no_cd = argc > 1 && strcmp(argv[1], "-n") == 0;
    int index = no_cd ? 2 : 1;
    if (argc - index > 1) {
        fputs("pushd: too many arguments\n", stderr);
        return 2;
    }
    char *cwd = current_directory();
    if (cwd == NULL) {
        return fs_error("pushd", ".");
    }
    if (index == argc) {
        if (shell->directory_stack.len == 0) {
            free(cwd);
            fputs("pushd: directory stack is empty\n", stderr);
            return 1;
        }
        char *target = shell->directory_stack.items[0];
        if (!no_cd && change_directory(shell, target, false, false, false) != 0) {
            free(cwd);
            return 1;
        }
        shell->directory_stack.items[0] = cwd;
        free(target);
    } else {
        char *target = xstrdup(argv[index]);
        if (!no_cd && change_directory(shell, target, false, false, true) != 0) {
            free(target);
            free(cwd);
            return 1;
        }
        strvec_push_owned(&shell->directory_stack, no_cd ? target : cwd);
        for (size_t i = shell->directory_stack.len - 1; i > 0; --i) {
            shell->directory_stack.items[i] = shell->directory_stack.items[i - 1];
        }
        shell->directory_stack.items[0] = no_cd ? target : cwd;
        free(no_cd ? cwd : target);
    }
    char *arguments[] = { "dirs", NULL };
    return dirs_command(shell, 1, arguments);
}

static int popd_command(Shell *shell, int argc, char **argv)
{
    bool no_cd = argc > 1 && strcmp(argv[1], "-n") == 0;
    int index = no_cd ? 2 : 1;
    size_t remove = 0;
    if (argc - index > 1) {
        fputs("popd: too many arguments\n", stderr);
        return 2;
    }
    if (shell->directory_stack.len == 0) {
        fputs("popd: directory stack is empty\n", stderr);
        return 1;
    }
    if (index < argc) {
        int64_t number;
        if ((argv[index][0] != '+' && argv[index][0] != '-') ||
            parse_number(argv[index] + 1, &number) != 0 || number < 0 ||
            (uint64_t)number > shell->directory_stack.len) {
            fputs("popd: invalid directory stack index\n", stderr);
            return 2;
        }
        size_t position = argv[index][0] == '+' ? (size_t)number :
            shell->directory_stack.len - (size_t)number;
        if (position > 0) {
            remove = position - 1;
            no_cd = true;
        }
    }
    if (!no_cd && change_directory(shell, shell->directory_stack.items[0],
                                    false, false, false) != 0) {
        return 1;
    }
    free(shell->directory_stack.items[remove]);
    for (size_t i = remove + 1; i < shell->directory_stack.len; ++i) {
        shell->directory_stack.items[i - 1] = shell->directory_stack.items[i];
    }
    --shell->directory_stack.len;
    char *arguments[] = { "dirs", NULL };
    return dirs_command(shell, 1, arguments);
}

static int path_command(Shell *shell)
{
    const char *path = shell_get(shell, "PATH");
    if (path == NULL) {
        puts("<no paths have been defined>");
        return 0;
    }
    char *paths = xstrdup(path);
    char *cursor = paths;
    size_t count = 0;
    do {
        char *colon = strchr(cursor, ':');
        if (colon != NULL) {
            *colon = '\0';
        }
        const char *directory = cursor[0] == '\0' ? "." : cursor;
        DIR *dir = opendir(directory);
        if (dir != NULL) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char *file = path_join(directory, entry->d_name);
                struct stat info;
                if (stat(file, &info) == 0 && S_ISREG(info.st_mode) && access(file, X_OK) == 0) {
                    printf("%s/\033[1m%s\033[0m\n", directory, entry->d_name);
                    ++count;
                }
                free(file);
            }
            closedir(dir);
        }
        cursor = colon == NULL ? NULL : colon + 1;
    } while (cursor != NULL);
    free(paths);
    if (count == 0) {
        puts("<no paths have been defined>");
    }
    return 0;
}

static int basename_command(int argc, char **argv)
{
    bool multiple = false;
    bool zero = false;
    const char *suffix = NULL;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "--") == 0) {
            ++index;
            break;
        }
        if (strcmp(argv[index], "-a") == 0) {
            multiple = true;
        } else if (strcmp(argv[index], "-z") == 0) {
            zero = true;
        } else if (strcmp(argv[index], "-s") == 0 && index + 1 < argc) {
            suffix = argv[++index];
            multiple = true;
        } else {
            fprintf(stderr, "basename: invalid option: %s\n", argv[index]);
            return 2;
        }
        ++index;
    }
    if (index == argc || (!multiple && argc - index > 2)) {
        fputs("basename: usage: basename [-az] [-s SUFFIX] PATH [SUFFIX]\n", stderr);
        return 2;
    }
    if (!multiple && index + 1 < argc) {
        suffix = argv[index + 1];
    }
    int end = multiple ? argc : index + 1;
    for (; index < end; ++index) {
        char *copy = xstrdup(argv[index]);
        size_t length = strlen(copy);
        while (length > 1 && copy[length - 1] == '/') {
            copy[--length] = '\0';
        }
        char *base = strrchr(copy, '/');
        base = base == NULL ? copy : base + 1;
        if (length == 1 && copy[0] == '/') {
            base = copy;
        }
        if (suffix != NULL) {
            size_t slen = strlen(suffix);
            size_t blen = strlen(base);
            if (slen < blen && strcmp(base + blen - slen, suffix) == 0) {
                base[blen - slen] = '\0';
            }
        }
        fputs(base, stdout);
        putchar(zero ? '\0' : '\n');
        free(copy);
    }
    return 0;
}

static int dirname_command(int argc, char **argv)
{
    bool zero = argc > 1 && strcmp(argv[1], "-z") == 0;
    int index = zero ? 2 : 1;
    if (index < argc && strcmp(argv[index], "--") == 0) {
        ++index;
    }
    if (index == argc) {
        fputs("dirname: missing operand\n", stderr);
        return 2;
    }
    for (; index < argc; ++index) {
        char *copy = xstrdup(argv[index]);
        size_t length = strlen(copy);
        while (length > 1 && copy[length - 1] == '/') {
            copy[--length] = '\0';
        }
        char *slash = strrchr(copy, '/');
        if (slash == NULL) {
            fputs(".", stdout);
            putchar(zero ? '\0' : '\n');
        } else {
            while (slash > copy && slash[-1] == '/') {
                --slash;
            }
            if (slash == copy) {
                copy[1] = '\0';
            } else {
                *slash = '\0';
            }
            fputs(copy, stdout);
            putchar(zero ? '\0' : '\n');
        }
        free(copy);
    }
    return 0;
}

static int realpath_command(int argc, char **argv)
{
    bool lexical = false;
    bool zero = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "--") == 0) {
            ++index;
            break;
        }
        if (strcmp(argv[index], "-m") == 0) {
            lexical = true;
        } else if (strcmp(argv[index], "-z") == 0) {
            zero = true;
        } else if (strcmp(argv[index], "-e") != 0) {
            fprintf(stderr, "realpath: invalid option: %s\n", argv[index]);
            return 2;
        }
        ++index;
    }
    if (index == argc) {
        fputs("realpath: missing operand\n", stderr);
        return 2;
    }
    int result = 0;
    char *cwd = current_directory();
    if (cwd == NULL) {
        return fs_error("realpath", ".");
    }
    for (; index < argc; ++index) {
        char *path = lexical ? logical_directory(cwd, argv[index]) : realpath(argv[index], NULL);
        if (path == NULL) {
            result = fs_error("realpath", argv[index]);
        } else {
            fputs(path, stdout);
            putchar(zero ? '\0' : '\n');
            free(path);
        }
    }
    free(cwd);
    return result;
}

static const char *file_type(mode_t mode)
{
    if (S_ISREG(mode)) return "regular file";
    if (S_ISDIR(mode)) return "directory";
    if (S_ISLNK(mode)) return "symbolic link";
    if (S_ISFIFO(mode)) return "fifo";
    if (S_ISSOCK(mode)) return "socket";
    if (S_ISCHR(mode)) return "character device";
    if (S_ISBLK(mode)) return "block device";
    return "unknown";
}

static void stat_format(const char *format, const char *path, const struct stat *info)
{
    for (const char *p = format; *p != '\0'; ++p) {
        if (*p != '%' || p[1] == '\0') {
            putchar(*p);
            continue;
        }
        switch (*++p) {
        case '%': putchar('%'); break;
        case 'n': fputs(path, stdout); break;
        case 's': printf("%lld", (long long)info->st_size); break;
        case 'a': printf("%o", (unsigned)(info->st_mode & 07777)); break;
        case 'f': printf("%x", (unsigned)info->st_mode); break;
        case 'F': fputs(file_type(info->st_mode), stdout); break;
        case 'u': printf("%lu", (unsigned long)info->st_uid); break;
        case 'g': printf("%lu", (unsigned long)info->st_gid); break;
        case 'i': printf("%llu", (unsigned long long)info->st_ino); break;
        case 'h': printf("%lu", (unsigned long)info->st_nlink); break;
        case 'd': printf("%llu", (unsigned long long)info->st_dev); break;
        case 'b': printf("%lld", (long long)info->st_blocks); break;
        case 'X': printf("%lld", (long long)info->st_atime); break;
        case 'Y': printf("%lld", (long long)info->st_mtime); break;
        case 'Z': printf("%lld", (long long)info->st_ctime); break;
        default: putchar('%'); putchar(*p); break;
        }
    }
}

static int stat_command(int argc, char **argv)
{
    bool follow = false;
    const char *format = NULL;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "--") == 0) {
            ++index;
            break;
        }
        if (strcmp(argv[index], "-L") == 0) {
            follow = true;
        } else if ((strcmp(argv[index], "-c") == 0 || strcmp(argv[index], "--format") == 0) && index + 1 < argc) {
            format = argv[++index];
        } else {
            fprintf(stderr, "stat: invalid option: %s\n", argv[index]);
            return 2;
        }
        ++index;
    }
    if (index == argc) {
        fputs("stat: missing operand\n", stderr);
        return 2;
    }
    int result = 0;
    for (; index < argc; ++index) {
        struct stat info;
        int status = follow ? stat(argv[index], &info) : lstat(argv[index], &info);
        if (status != 0) {
            result = fs_error("stat", argv[index]);
        } else if (format != NULL) {
            stat_format(format, argv[index], &info);
            putchar('\n');
        } else {
            printf("  File: %s\n  Size: %lld\tBlocks: %lld\t%s\n", argv[index],
                   (long long)info.st_size, (long long)info.st_blocks, file_type(info.st_mode));
            printf("Device: %llu\tInode: %llu\tLinks: %lu\n", (unsigned long long)info.st_dev,
                   (unsigned long long)info.st_ino, (unsigned long)info.st_nlink);
            printf("Access: (%04o)\tUid: %lu\tGid: %lu\n", (unsigned)(info.st_mode & 07777),
                   (unsigned long)info.st_uid, (unsigned long)info.st_gid);
            printf("Access: %lld\nModify: %lld\nChange: %lld\n", (long long)info.st_atime,
                   (long long)info.st_mtime, (long long)info.st_ctime);
        }
    }
    return result;
}

static int readlink_command(int argc, char **argv)
{
    bool newline = true;
    bool canonical = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "-n") == 0) newline = false;
        else if (strcmp(argv[index], "-f") == 0) canonical = true;
        else if (strcmp(argv[index], "--") == 0) { ++index; break; }
        else { fprintf(stderr, "readlink: invalid option: %s\n", argv[index]); return 2; }
        ++index;
    }
    if (index == argc) {
        fputs("readlink: missing operand\n", stderr);
        return 2;
    }
    int result = 0;
    for (; index < argc; ++index) {
        char *target = NULL;
        if (canonical) {
            target = realpath(argv[index], NULL);
        } else {
            size_t size = 256;
            for (;;) {
                target = xmalloc(size + 1);
                ssize_t count = readlink(argv[index], target, size);
                if (count < 0) {
                    free(target);
                    target = NULL;
                    break;
                }
                if ((size_t)count < size) {
                    target[count] = '\0';
                    break;
                }
                free(target);
                target = NULL;
                if (size >= MISH_MAX_SOURCE) {
                    errno = ENAMETOOLONG;
                    break;
                }
                size *= 2;
            }
        }
        if (target == NULL) {
            result = fs_error("readlink", argv[index]);
        } else {
            fputs(target, stdout);
            if (newline) putchar('\n');
            free(target);
        }
    }
    return result;
}

static int mkdir_command(int argc, char **argv)
{
    bool parents = false;
    bool verbose = false;
    bool explicit_mode = false;
    mode_t mode = 0777;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "-p") == 0) parents = true;
        else if (strcmp(argv[index], "-v") == 0) verbose = true;
        else if (strcmp(argv[index], "--") == 0) { ++index; break; }
        else if (strcmp(argv[index], "-m") == 0 && index + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++index], &end, 8);
            if (end == argv[index] || *end != '\0' || value > 07777) {
                fputs("mkdir: invalid octal mode\n", stderr);
                return 2;
            }
            mode = (mode_t)value;
            explicit_mode = true;
        } else { fprintf(stderr, "mkdir: invalid option: %s\n", argv[index]); return 2; }
        ++index;
    }
    if (index == argc) {
        fputs("mkdir: missing operand\n", stderr);
        return 2;
    }
    int result = 0;
    for (; index < argc; ++index) {
        char *copy = xstrdup(argv[index]);
        if (parents) {
            for (char *p = copy + (copy[0] == '/'); *p != '\0'; ++p) {
                if (*p != '/' || p == copy || p[-1] == '/') continue;
                *p = '\0';
                if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
                    result = fs_error("mkdir", copy);
                    *p = '/';
                    break;
                }
                *p = '/';
            }
        }
        if (mkdir(copy, mode) != 0) {
            struct stat info;
            if (!parents || errno != EEXIST || stat(copy, &info) != 0 || !S_ISDIR(info.st_mode)) {
                result = fs_error("mkdir", copy);
            }
        } else {
            if (explicit_mode && chmod(copy, mode) != 0) result = fs_error("mkdir", copy);
            if (verbose) printf("mkdir: created directory '%s'\n", copy);
        }
        free(copy);
    }
    return result;
}

static int rmdir_command(int argc, char **argv)
{
    bool parents = false;
    int index = 1;
    if (index < argc && strcmp(argv[index], "-p") == 0) {
        parents = true;
        ++index;
    }
    if (index < argc && strcmp(argv[index], "--") == 0) ++index;
    if (index == argc) {
        fputs("rmdir: missing operand\n", stderr);
        return 2;
    }
    int result = 0;
    for (; index < argc; ++index) {
        char *copy = xstrdup(argv[index]);
        for (;;) {
            if (rmdir(copy) != 0) {
                result = fs_error("rmdir", copy);
                break;
            }
            if (!parents) break;
            char *slash = strrchr(copy, '/');
            if (slash == NULL || slash == copy) break;
            *slash = '\0';
        }
        free(copy);
    }
    return result;
}

static int touch_command(int argc, char **argv)
{
    bool create = true;
    int index = 1;
    if (index < argc && strcmp(argv[index], "-c") == 0) { create = false; ++index; }
    if (index < argc && strcmp(argv[index], "--") == 0) ++index;
    if (index == argc) {
        fputs("touch: missing operand\n", stderr);
        return 2;
    }
    int result = 0;
    for (; index < argc; ++index) {
        if (utimensat(AT_FDCWD, argv[index], NULL, 0) == 0) continue;
        if (errno == ENOENT && !create) continue;
        if (errno != ENOENT) { result = fs_error("touch", argv[index]); continue; }
        int fd = open(argv[index], O_WRONLY | O_CREAT, 0666);
        if (fd < 0) result = fs_error("touch", argv[index]);
        else if (close(fd) != 0) result = fs_error("touch", argv[index]);
    }
    return result;
}

int builtin_fs_run(Shell *shell, int argc, char **argv)
{
    const char *name = argv[0];
    if (strcmp(name, "cd") == 0) return cd_command(shell, argc, argv);
    if (strcmp(name, "pwd") == 0) return pwd_command(shell, argc, argv);
    if (strcmp(name, "dirs") == 0) return dirs_command(shell, argc, argv);
    if (strcmp(name, "pushd") == 0) return pushd_command(shell, argc, argv);
    if (strcmp(name, "popd") == 0) return popd_command(shell, argc, argv);
    if (strcmp(name, "path") == 0) return path_command(shell);
    if (strcmp(name, "basename") == 0) return basename_command(argc, argv);
    if (strcmp(name, "dirname") == 0) return dirname_command(argc, argv);
    if (strcmp(name, "realpath") == 0) return realpath_command(argc, argv);
    if (strcmp(name, "stat") == 0) return stat_command(argc, argv);
    if (strcmp(name, "readlink") == 0) return readlink_command(argc, argv);
    if (strcmp(name, "mkdir") == 0) return mkdir_command(argc, argv);
    if (strcmp(name, "rmdir") == 0) return rmdir_command(argc, argv);
    if (strcmp(name, "touch") == 0) return touch_command(argc, argv);
    return -1;
}
