#include "mish.h"
#include <errno.h>
#include <fnmatch.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool unary_operator(const char *word)
{
    static const char *const options[] = {
        "-a", "-b", "-c", "-d", "-e", "-f", "-g", "-h", "-k", "-L",
        "-n", "-O", "-G", "-p", "-r", "-s", "-S", "-t", "-u", "-w",
        "-x", "-z", "-v"
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        if (strcmp(word, options[i]) == 0) return true;
    }
    return false;
}

static bool binary_operator(const char *word)
{
    static const char *const options[] = {
        "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt",
        "-ge", "-ef", "-nt", "-ot", "=~"
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        if (strcmp(word, options[i]) == 0) return true;
    }
    return false;
}

static bool unary_test(Shell *shell, const char *operation, const char *operand, int *error)
{
    if (strcmp(operation, "-n") == 0) return operand[0] != '\0';
    if (strcmp(operation, "-z") == 0) return operand[0] == '\0';
    if (strcmp(operation, "-v") == 0) return shell_get(shell, operand) != NULL;
    if (strcmp(operation, "-t") == 0) {
        char *end;
        errno = 0;
        long fd = strtol(operand, &end, 10);
        if (errno != 0 || *end != '\0' || end == operand || fd < 0 || fd > 2147483647) {
            fprintf(stderr, "test: invalid file descriptor: %s\n", operand);
            *error = 2;
            return false;
        }
        return isatty((int)fd) != 0;
    }
    if (strcmp(operation, "-r") == 0) return access(operand, R_OK) == 0;
    if (strcmp(operation, "-w") == 0) return access(operand, W_OK) == 0;
    if (strcmp(operation, "-x") == 0) return access(operand, X_OK) == 0;
    struct stat info;
    if (strcmp(operation, "-L") == 0 || strcmp(operation, "-h") == 0) {
        return lstat(operand, &info) == 0 && S_ISLNK(info.st_mode);
    }
    if (stat(operand, &info) != 0) return false;
    if (strcmp(operation, "-a") == 0 || strcmp(operation, "-e") == 0) return true;
    if (strcmp(operation, "-b") == 0) return S_ISBLK(info.st_mode);
    if (strcmp(operation, "-c") == 0) return S_ISCHR(info.st_mode);
    if (strcmp(operation, "-d") == 0) return S_ISDIR(info.st_mode);
    if (strcmp(operation, "-f") == 0) return S_ISREG(info.st_mode);
    if (strcmp(operation, "-p") == 0) return S_ISFIFO(info.st_mode);
    if (strcmp(operation, "-S") == 0) return S_ISSOCK(info.st_mode);
    if (strcmp(operation, "-s") == 0) return info.st_size > 0;
    if (strcmp(operation, "-u") == 0) return (info.st_mode & S_ISUID) != 0;
    if (strcmp(operation, "-g") == 0) return (info.st_mode & S_ISGID) != 0;
    if (strcmp(operation, "-k") == 0) return (info.st_mode & 01000) != 0;
    if (strcmp(operation, "-O") == 0) return info.st_uid == geteuid();
    if (strcmp(operation, "-G") == 0) return info.st_gid == getegid();
    return false;
}

static int decimal_number(const char *text, long long *number)
{
    char *end;
    errno = 0;
    *number = strtoll(text, &end, 10);
    if (text == end || *end != '\0' || errno != 0) {
        fprintf(stderr, "test: integer expression expected: %s\n", text);
        return 2;
    }
    return 0;
}

static bool binary_test(const char *left, const char *operation, const char *right, int *error)
{
    if (strcmp(operation, "=") == 0 || strcmp(operation, "==") == 0) return strcmp(left, right) == 0;
    if (strcmp(operation, "!=") == 0) return strcmp(left, right) != 0;
    if (strcmp(operation, "<") == 0) return strcmp(left, right) < 0;
    if (strcmp(operation, ">") == 0) return strcmp(left, right) > 0;
    if (strcmp(operation, "=~") == 0) {
        regex_t regex;
        int status = regcomp(&regex, right, REG_EXTENDED | REG_NOSUB);
        if (status != 0) {
            char message[256];
            regerror(status, &regex, message, sizeof(message));
            fprintf(stderr, "test: invalid regular expression: %s\n", message);
            *error = 2;
            return false;
        }
        bool matches = regexec(&regex, left, 0, NULL, 0) == 0;
        regfree(&regex);
        return matches;
    }
    if (strcmp(operation, "-ef") == 0 || strcmp(operation, "-nt") == 0 || strcmp(operation, "-ot") == 0) {
        struct stat a;
        struct stat b;
        bool exists_a = stat(left, &a) == 0;
        bool exists_b = stat(right, &b) == 0;
        if (strcmp(operation, "-ef") == 0) return exists_a && exists_b && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
        if (strcmp(operation, "-nt") == 0) {
            if (!exists_a) return false;
            if (!exists_b) return true;
            return a.st_mtim.tv_sec > b.st_mtim.tv_sec ||
                (a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec > b.st_mtim.tv_nsec);
        }
        if (!exists_b) return false;
        if (!exists_a) return true;
        return a.st_mtim.tv_sec < b.st_mtim.tv_sec ||
            (a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec < b.st_mtim.tv_nsec);
    }
    long long a;
    long long b;
    if (decimal_number(left, &a) != 0 || decimal_number(right, &b) != 0) {
        *error = 2;
        return false;
    }
    if (strcmp(operation, "-eq") == 0) return a == b;
    if (strcmp(operation, "-ne") == 0) return a != b;
    if (strcmp(operation, "-lt") == 0) return a < b;
    if (strcmp(operation, "-le") == 0) return a <= b;
    if (strcmp(operation, "-gt") == 0) return a > b;
    if (strcmp(operation, "-ge") == 0) return a >= b;
    return false;
}

typedef struct {
    Shell *shell;
    int count;
    char **words;
    int position;
    int error;
    unsigned depth;
} TestParser;

static bool parse_test_or(TestParser *parser);

static bool parse_test_primary(TestParser *parser)
{
    if (parser->position >= parser->count) {
        fputs("test: missing expression\n", stderr);
        parser->error = 2;
        return false;
    }
    if (++parser->depth > MISH_MAX_DEPTH) {
        fputs("test: expression nesting too deep\n", stderr);
        parser->error = 2;
        --parser->depth;
        return false;
    }
    const char *word = parser->words[parser->position++];
    bool result;
    if (strcmp(word, "!") == 0) {
        result = !parse_test_primary(parser);
    } else if (strcmp(word, "(") == 0) {
        result = parse_test_or(parser);
        if (parser->position >= parser->count || strcmp(parser->words[parser->position++], ")") != 0) {
            fputs("test: missing closing parenthesis\n", stderr);
            parser->error = 2;
        }
    } else if (parser->position + 1 < parser->count && binary_operator(parser->words[parser->position])) {
        const char *operator = parser->words[parser->position++];
        const char *right = parser->words[parser->position++];
        result = binary_test(word, operator, right, &parser->error);
    } else if (unary_operator(word)) {
        if (parser->position >= parser->count) {
            fprintf(stderr, "test: %s: missing operand\n", word);
            parser->error = 2;
            result = false;
        } else {
            result = unary_test(parser->shell, word, parser->words[parser->position++], &parser->error);
        }
    } else {
        result = word[0] != '\0';
    }
    --parser->depth;
    return result;
}

static bool parse_test_and(TestParser *parser)
{
    bool result = parse_test_primary(parser);
    while (parser->position < parser->count && strcmp(parser->words[parser->position], "-a") == 0) {
        ++parser->position;
        bool right = parse_test_primary(parser);
        result = result && right;
    }
    return result;
}

static bool parse_test_or(TestParser *parser)
{
    bool result = parse_test_and(parser);
    while (parser->position < parser->count && strcmp(parser->words[parser->position], "-o") == 0) {
        ++parser->position;
        bool right = parse_test_and(parser);
        result = result || right;
    }
    return result;
}

static int test_command(Shell *shell, int argc, char **argv)
{
    int count = argc - 1;
    char **words = argv + 1;
    if (strcmp(argv[0], "[") == 0) {
        if (count == 0 || strcmp(words[count - 1], "]") != 0) {
            fputs("[: missing closing ]\n", stderr);
            return 2;
        }
        --count;
    }
    if (count == 0) return 1;
    if (count == 1) return words[0][0] == '\0' ? 1 : 0;
    int error = 0;
    if (count == 2 && strcmp(words[0], "!") == 0) return words[1][0] == '\0' ? 0 : 1;
    if (count == 2 && unary_operator(words[0])) {
        bool result = unary_test(shell, words[0], words[1], &error);
        return error != 0 ? error : result ? 0 : 1;
    }
    if (count == 3 && binary_operator(words[1])) {
        bool result = binary_test(words[0], words[1], words[2], &error);
        return error != 0 ? error : result ? 0 : 1;
    }
    TestParser parser = { .shell = shell, .count = count, .words = words };
    bool result = parse_test_or(&parser);
    if (parser.position != count && parser.error == 0) {
        fprintf(stderr, "test: unexpected argument: %s\n", words[parser.position]);
        parser.error = 2;
    }
    return parser.error != 0 ? parser.error : result ? 0 : 1;
}

static int match_command(int argc, char **argv)
{
    bool regex = argc > 1 && strcmp(argv[1], "-r") == 0;
    int index = regex ? 2 : 1;
    if (argc - index < 2) {
        fputs("match: usage: match [-r] PATTERN STRING ...\n", stderr);
        return 2;
    }
    const char *pattern = argv[index++];
    int result = 1;
    if (regex) {
        regex_t expression;
        int status = regcomp(&expression, pattern, REG_EXTENDED);
        if (status != 0) {
            char message[256];
            regerror(status, &expression, message, sizeof(message));
            fprintf(stderr, "match: %s\n", message);
            return 2;
        }
        for (; index < argc; ++index) {
            regmatch_t matches[16];
            if (regexec(&expression, argv[index], 16, matches, 0) == 0) {
                result = 0;
                for (size_t i = 0; i < 16 && matches[i].rm_so >= 0; ++i) {
                    printf("%.*s\n", (int)(matches[i].rm_eo - matches[i].rm_so), argv[index] + matches[i].rm_so);
                }
            }
        }
        regfree(&expression);
    } else {
        for (; index < argc; ++index) {
            if (fnmatch(pattern, argv[index], 0) == 0) {
                puts(argv[index]);
                result = 0;
            }
        }
    }
    return result;
}

int builtin_test_run(Shell *shell, int argc, char **argv)
{
    if (strcmp(argv[0], "test") == 0 || strcmp(argv[0], "[") == 0) return test_command(shell, argc, argv);
    if (strcmp(argv[0], "match") == 0) return match_command(argc, argv);
    if (strcmp(argv[0], "assert") == 0) {
        int result = test_command(shell, argc, argv);
        if (result != 0) {
            fputs("assert: condition failed:", stderr);
            for (int i = 1; i < argc; ++i) fprintf(stderr, " %s", argv[i]);
            fputc('\n', stderr);
        }
        return result;
    }
    return -1;
}
