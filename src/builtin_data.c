#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

int builtin_invoke(Shell *, int, char **, bool, bool);
int builtin_option_state(Shell *, const char *, bool *);

static int data_error(const char *name, const char *message)
{
    fprintf(stderr, "%s: %s\n", name, message);
    return 2;
}

static void utf8_character(FILE *stream, unsigned long value)
{
    if (value <= 0x7f) {
        fputc((int)value, stream);
    } else if (value <= 0x7ff) {
        fputc(0xc0 | (int)(value >> 6), stream);
        fputc(0x80 | (int)(value & 63), stream);
    } else if (value <= 0xffff && !(value >= 0xd800 && value <= 0xdfff)) {
        fputc(0xe0 | (int)(value >> 12), stream);
        fputc(0x80 | (int)((value >> 6) & 63), stream);
        fputc(0x80 | (int)(value & 63), stream);
    } else if (value >= 0x10000 && value <= 0x10ffff) {
        fputc(0xf0 | (int)(value >> 18), stream);
        fputc(0x80 | (int)((value >> 12) & 63), stream);
        fputc(0x80 | (int)((value >> 6) & 63), stream);
        fputc(0x80 | (int)(value & 63), stream);
    }
}

static bool escaped_text(FILE *stream, const char *text, bool printf_mode)
{
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] != '\\' || text[i + 1] == '\0') {
            fputc((unsigned char)text[i], stream);
            continue;
        }
        char escape = text[++i];
        switch (escape) {
        case 'a': fputc('\a', stream); break;
        case 'b': fputc('\b', stream); break;
        case 'c': return false;
        case 'e': case 'E': fputc(27, stream); break;
        case 'f': fputc('\f', stream); break;
        case 'n': fputc('\n', stream); break;
        case 'r': fputc('\r', stream); break;
        case 't': fputc('\t', stream); break;
        case 'v': fputc('\v', stream); break;
        case '\\': fputc('\\', stream); break;
        case 'x': case 'u': case 'U': {
            unsigned limit = escape == 'x' ? 2 : escape == 'u' ? 4 : 8;
            unsigned long value = 0;
            unsigned consumed = 0;
            while (consumed < limit && isxdigit((unsigned char)text[i + 1])) {
                unsigned char digit = (unsigned char)text[++i];
                unsigned number = isdigit(digit) ? (unsigned)(digit - '0') :
                    (unsigned)(tolower(digit) - 'a' + 10);
                value = value * 16 + number;
                ++consumed;
            }
            if (consumed == 0) {
                fputc('\\', stream);
                fputc(escape, stream);
            } else if (escape == 'x') {
                fputc((int)(value & 255), stream);
            } else {
                utf8_character(stream, value);
            }
            break;
        }
        default:
            if (escape >= '0' && escape <= '7' && (printf_mode || escape == '0')) {
                unsigned value = printf_mode ? (unsigned)(escape - '0') : 0;
                unsigned remaining = printf_mode ? 2 : 3;
                while (remaining > 0 && text[i + 1] >= '0' && text[i + 1] <= '7') {
                    value = value * 8 + (unsigned)(text[++i] - '0');
                    --remaining;
                }
                fputc((int)(value & 255), stream);
            } else {
                fputc('\\', stream);
                fputc(escape, stream);
            }
            break;
        }
    }
    return true;
}

static int echo_command(int argc, char **argv)
{
    bool newline = true;
    bool escapes = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
        bool option = true;
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            if (*p != 'n' && *p != 'e' && *p != 'E') option = false;
        }
        if (!option) break;
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            if (*p == 'n') newline = false;
            else escapes = *p == 'e';
        }
        ++index;
    }
    for (int i = index; i < argc; ++i) {
        if (i > index) putchar(' ');
        if (escapes) {
            if (!escaped_text(stdout, argv[i], false)) return ferror(stdout) ? 1 : 0;
        } else {
            fputs(argv[i], stdout);
        }
    }
    if (newline) putchar('\n');
    return ferror(stdout) ? 1 : 0;
}

static long long printf_integer(const char *text, int *status)
{
    if (text[0] == '\'' || text[0] == '"') {
        return (unsigned char)text[1];
    }
    if (text[0] == '\0') return 0;
    char *end;
    errno = 0;
    long long value = strtoll(text, &end, 0);
    if (errno != 0 || *end != '\0') {
        fprintf(stderr, "printf: invalid number: %s\n", text);
        *status = 1;
        return 0;
    }
    return value;
}

static int printf_command(Shell *shell, int argc, char **argv)
{
    int index = 1;
    const char *variable = NULL;
    if (index < argc && strcmp(argv[index], "-v") == 0) {
        if (index + 2 >= argc || !valid_name(argv[index + 1])) {
            return data_error("printf", "usage: printf [-v NAME] FORMAT [ARGUMENT ...]");
        }
        variable = argv[index + 1];
        index += 2;
    }
    if (index < argc && strcmp(argv[index], "--") == 0) ++index;
    if (index == argc) return data_error("printf", "missing format");
    const char *format = argv[index++];
    FILE *stream = stdout;
    char *output = NULL;
    size_t output_length = 0;
    if (variable != NULL) {
        stream = open_memstream(&output, &output_length);
        if (stream == NULL) return data_error("printf", strerror(errno));
    }
    int status = 0;
    bool stop = false;
    do {
        int before = index;
        for (size_t p = 0; format[p] != '\0' && !stop; ++p) {
            if (format[p] == '\\') {
                size_t end = p + 1;
                if (format[end] != '\0') ++end;
                if (strchr("01234567", format[p + 1]) != NULL && format[p + 1] != '\0') {
                    while (end < p + 4 && format[end] >= '0' && format[end] <= '7') ++end;
                } else if (format[p + 1] == 'x' || format[p + 1] == 'u' || format[p + 1] == 'U') {
                    size_t limit = format[p + 1] == 'x' ? 2 : format[p + 1] == 'u' ? 4 : 8;
                    while (end < p + 2 + limit && isxdigit((unsigned char)format[end])) ++end;
                }
                char *part = xstrndup(format + p, end - p);
                stop = !escaped_text(stream, part, true);
                free(part);
                p = end - 1;
                continue;
            }
            if (format[p] != '%') {
                fputc((unsigned char)format[p], stream);
                continue;
            }
            if (format[p + 1] == '%') {
                fputc('%', stream);
                ++p;
                continue;
            }
            Buffer conversion;
            buffer_init(&conversion);
            buffer_putc(&conversion, '%');
            ++p;
            while (format[p] != '\0' && strchr("-+ #0", format[p]) != NULL) {
                buffer_putc(&conversion, format[p++]);
            }
            for (int component = 0; component < 2; ++component) {
                if (component == 1) {
                    if (format[p] != '.') break;
                    buffer_putc(&conversion, '.');
                    ++p;
                }
                if (format[p] == '*') {
                    long long width = index < argc ? printf_integer(argv[index++], &status) : 0;
                    if (width > 1000000 || width < -1000000) {
                        data_error("printf", "width or precision exceeds 1000000");
                        status = 2;
                        stop = true;
                        break;
                    }
                    if (component == 1 && width < 0) {
                        --conversion.len;
                        conversion.data[conversion.len] = '\0';
                        ++p;
                        continue;
                    }
                    char *number = str_printf("%lld", width);
                    buffer_append(&conversion, number);
                    free(number);
                    ++p;
                } else {
                    size_t digits = 0;
                    while (isdigit((unsigned char)format[p])) {
                        buffer_putc(&conversion, format[p++]);
                        ++digits;
                    }
                    if (digits > 6) {
                        data_error("printf", "width or precision too large");
                        status = 2;
                        stop = true;
                        break;
                    }
                }
            }
            if (stop) {
                buffer_free(&conversion);
                break;
            }
            char kind = format[p];
            if (kind == '\0' || strchr("sbcqdiuoxXfFeEgGaA", kind) == NULL) {
                fprintf(stderr, "printf: invalid conversion: %c\n", kind == '\0' ? '?' : kind);
                buffer_free(&conversion);
                status = 2;
                stop = true;
                break;
            }
            const char *argument = index < argc ? argv[index++] : "";
            if (strchr("diuoxX", kind) != NULL) buffer_append(&conversion, "ll");
            buffer_putc(&conversion, kind == 'b' || kind == 'q' ? 's' : kind);
            char *spec = buffer_take(&conversion);
            if (kind == 's') {
                fprintf(stream, spec, argument);
            } else if (kind == 'c') {
                fprintf(stream, spec, (unsigned char)argument[0]);
            } else if (kind == 'b') {
                char *expanded = NULL;
                size_t length = 0;
                FILE *temporary = open_memstream(&expanded, &length);
                if (temporary == NULL) {
                    status = 1;
                    stop = true;
                } else {
                    stop = !escaped_text(temporary, argument, false);
                    fclose(temporary);
                    if (strcmp(spec, "%s") == 0) fwrite(expanded, 1, length, stream);
                    else fprintf(stream, spec, expanded);
                    free(expanded);
                }
            } else if (kind == 'q') {
                char *quoted = shell_quote(argument);
                fprintf(stream, spec, quoted);
                free(quoted);
            } else if (strchr("di", kind) != NULL) {
                fprintf(stream, spec, printf_integer(argument, &status));
            } else if (strchr("uoxX", kind) != NULL) {
                fprintf(stream, spec, (unsigned long long)printf_integer(argument, &status));
            } else {
                char *end;
                errno = 0;
                double number = argument[0] == '\0' ? 0.0 : strtod(argument, &end);
                if (argument[0] != '\0' && (errno != 0 || *end != '\0')) {
                    fprintf(stderr, "printf: invalid floating point number: %s\n", argument);
                    status = 1;
                    number = 0;
                }
                fprintf(stream, spec, number);
            }
            free(spec);
        }
        if (before == index) break;
    } while (index < argc && !stop);
    if (variable != NULL) {
        if (fclose(stream) != 0) status = 1;
        if (memchr(output, '\0', output_length) != NULL) {
            fputs("printf: variable values cannot contain NUL bytes\n", stderr);
            status = 1;
        } else if (shell_set(shell, variable, output) != 0) {
            status = 1;
        }
        free(output);
    } else if (ferror(stdout)) {
        status = 1;
    }
    return status;
}

static void print_variable(const Var *var, const char *prefix, bool attributes)
{
    char *quoted = shell_quote(var->value);
    if (attributes) {
        printf("declare -%s%s%s %s=%s\n", var->exported ? "x" : "",
               var->readonly ? "r" : "", !var->exported && !var->readonly ? "-" : "",
               var->name, quoted);
    } else {
        printf("%s%s=%s\n", prefix, var->name, quoted);
    }
    free(quoted);
}

static int variables_command(Shell *shell, int argc, char **argv)
{
    bool exporting = strcmp(argv[0], "export") == 0;
    bool readonly = strcmp(argv[0], "readonly") == 0;
    bool local = strcmp(argv[0], "local") == 0;
    bool declaration = strcmp(argv[0], "declare") == 0 || strcmp(argv[0], "typeset") == 0;
    bool print = argc == 1;
    bool unexport = false;
    bool integer = false;
    bool function = false;
    bool global = false;
    int index = 1;
    while (index < argc && (argv[index][0] == '-' || argv[index][0] == '+') && argv[index][1] != '\0') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        bool enabled = argv[index][0] == '-';
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            switch (*p) {
            case 'p': print = true; break;
            case 'x': exporting = enabled; unexport = !enabled; break;
            case 'r':
                if (!enabled) return data_error(argv[0], "readonly attributes cannot be removed");
                readonly = true;
                break;
            case 'n': if (exporting) unexport = true; else return data_error(argv[0], "unsupported option"); break;
            case 'i': integer = enabled; break;
            case 'f': case 'F': function = true; break;
            case 'g': local = false; global = true; break;
            default: return data_error(argv[0], "invalid declaration option");
            }
        }
        ++index;
    }
    if (function) {
        int found = 0;
        for (Function *item = shell->functions; item != NULL; item = item->next) {
            bool selected = index == argc;
            for (int i = index; i < argc; ++i) if (strcmp(argv[i], item->name) == 0) selected = true;
            if (selected) {
                char *body = node_source(item->body);
                if (body == NULL) {
                    fprintf(stderr, "%s: cannot serialize function: %s\n", argv[0], item->name);
                    return 1;
                }
                printf("%s() { %s; }\n", item->name, body);
                free(body);
                ++found;
            }
        }
        return index != argc && found == 0 ? 1 : 0;
    }
    if (local && shell->function_depth == 0) {
        return data_error(argv[0], "can only be used inside a function");
    }
    if (index == argc) {
        for (Var *var = shell->variables; var != NULL; var = var->next) {
            if (exporting && !var->exported) continue;
            if (readonly && !var->readonly) continue;
            print_variable(var, exporting ? "export " : readonly ? "readonly " : "", declaration);
        }
        return 0;
    }
    int status = 0;
    for (; index < argc; ++index) {
        char *copy = xstrdup(argv[index]);
        char *equal = strchr(copy, '=');
        if (equal != NULL) *equal++ = '\0';
        if (!valid_name(copy)) {
            fprintf(stderr, "%s: invalid identifier: %s\n", argv[0], copy);
            status = 1;
            free(copy);
            continue;
        }
        Var *var = shell_find_var(shell, copy);
        if (print && equal == NULL) {
            if (var == NULL) status = 1;
            else print_variable(var, exporting ? "export " : readonly ? "readonly " : "", declaration);
            free(copy);
            continue;
        }
        char *value = NULL;
        if (integer && equal != NULL) {
            int64_t result;
            char *error = NULL;
            if (arithmetic_eval(shell, equal, &result, &error) != 0) {
                fprintf(stderr, "%s: %s\n", argv[0], error == NULL ? "invalid arithmetic" : error);
                free(error);
                free(copy);
                status = 1;
                continue;
            }
            value = str_printf("%lld", (long long)result);
            equal = value;
        }
        int result = 0;
        if (local || (declaration && !global && shell->function_depth > 0)) {
            result = shell_local(shell, copy, equal);
        } else if (equal != NULL) {
            result = shell_set(shell, copy, equal);
        } else if (var == NULL) {
            result = shell_set(shell, copy, "");
        }
        if (result == 0 && exporting && !unexport) result = shell_export(shell, copy, equal);
        var = shell_find_var(shell, copy);
        if (result == 0 && var != NULL) {
            if (unexport) var->exported = false;
            if (readonly) var->readonly = true;
        }
        if (result != 0) status = 1;
        free(value);
        free(copy);
    }
    return status;
}

static int unset_command(Shell *shell, int argc, char **argv)
{
    bool functions = false;
    int index = 1;
    if (index < argc && strcmp(argv[index], "-f") == 0) { functions = true; ++index; }
    else if (index < argc && strcmp(argv[index], "-v") == 0) ++index;
    if (index < argc && strcmp(argv[index], "--") == 0) ++index;
    int status = 0;
    for (; index < argc; ++index) {
        if (!valid_name(argv[index])) {
            fprintf(stderr, "unset: invalid identifier: %s\n", argv[index]);
            status = 1;
        } else {
            int result = functions ? function_unset(shell, argv[index]) : shell_unset(shell, argv[index]);
            if (result != 0) status = 1;
        }
    }
    return status;
}

static int env_command(Shell *shell, int argc, char **argv)
{
    bool clear = false;
    bool zero = false;
    StrVec removed;
    strvec_init(&removed);
    int index = 1;
    while (index < argc) {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        if (strcmp(argv[index], "-i") == 0 || strcmp(argv[index], "-") == 0) clear = true;
        else if (strcmp(argv[index], "-0") == 0) zero = true;
        else if (strcmp(argv[index], "-u") == 0 && index + 1 < argc) strvec_push(&removed, argv[++index]);
        else if (argv[index][0] == '-') { strvec_free(&removed); return data_error("env", "invalid option"); }
        else break;
        ++index;
    }
    bool modifying = clear || removed.len > 0 || (index < argc && strchr(argv[index], '=') != NULL);
    if (!modifying && index == argc) {
        for (Var *var = shell->variables; var != NULL; var = var->next) {
            if (var->exported) {
                if (zero) printf("%s=%s%c", var->name, var->value, '\0');
                else printf("%s=\033[1m%s\033[0m\n", var->name, var->value);
            }
        }
        strvec_free(&removed);
        return ferror(stdout) ? 1 : 0;
    }
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) {
        strvec_free(&removed);
        return data_error("env", strerror(errno));
    }
    if (child == 0) {
        shell_signals_child();
        if (clear) for (Var *var = shell->variables; var != NULL; var = var->next) var->exported = false;
        for (size_t i = 0; i < removed.len; ++i) {
            Var *var = shell_find_var(shell, removed.items[i]);
            if (var != NULL) var->exported = false;
        }
        while (index < argc && strchr(argv[index], '=') != NULL) {
            char *copy = xstrdup(argv[index++]);
            char *equal = strchr(copy, '=');
            *equal++ = '\0';
            Var *var = shell_find_var(shell, copy);
            if (var != NULL) var->readonly = false;
            if (!valid_name(copy) || shell_export(shell, copy, equal) != 0) {
                fprintf(stderr, "env: invalid assignment: %s\n", copy);
                free(copy);
                fflush(NULL);
                _exit(125);
            }
            free(copy);
        }
        if (index == argc) {
            for (Var *var = shell->variables; var != NULL; var = var->next) {
                if (var->exported) printf("%s=%s%c", var->name, var->value, zero ? '\0' : '\n');
            }
            fflush(NULL);
            _exit(0);
        }
        int result = builtin_invoke(shell, argc - index, argv + index, true, true);
        fflush(NULL);
        _exit(result & 255);
    }
    strvec_free(&removed);
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return data_error("env", strerror(errno));
    }
    return shell_wait_status(status);
}

static int printenv_command(Shell *shell, int argc, char **argv)
{
    bool zero = argc > 1 && strcmp(argv[1], "-0") == 0;
    int index = zero ? 2 : 1;
    int result = 0;
    if (index == argc) {
        for (Var *var = shell->variables; var != NULL; var = var->next) {
            if (var->exported) printf("%s=%s%c", var->name, var->value, zero ? '\0' : '\n');
        }
    } else {
        for (; index < argc; ++index) {
            Var *var = shell_find_var(shell, argv[index]);
            if (var != NULL && var->exported) printf("%s%c", var->value, zero ? '\0' : '\n');
            else result = 1;
        }
    }
    return result;
}

static int shift_command(Shell *shell, int argc, char **argv)
{
    int64_t amount = 1;
    if (argc > 2 || (argc == 2 && parse_number(argv[1], &amount) != 0) || amount < 0) {
        return data_error("shift", "expected a nonnegative count");
    }
    if ((uint64_t)amount > shell->positional_count) return data_error("shift", "count exceeds positional arguments");
    StrVec values;
    strvec_init(&values);
    for (size_t i = (size_t)amount; i < shell->positional_count; ++i) strvec_push(&values, shell->positional[i]);
    shell_set_positional(shell, values.len, values.items);
    strvec_free(&values);
    return 0;
}

static const char *short_option(char value)
{
    switch (value) {
    case 'a': return "allexport";
    case 'e': return "errexit";
    case 'f': return "noglob";
    case 'm': return "monitor";
    case 'n': return "noexec";
    case 'u': return "nounset";
    case 'v': return "verbose";
    case 'x': return "xtrace";
    case 'C': return "noclobber";
    case 'h': return "hashall";
    default: return NULL;
    }
}

static int set_command(Shell *shell, int argc, char **argv)
{
    if (argc == 1) {
        for (Var *var = shell->variables; var != NULL; var = var->next) print_variable(var, "", false);
        return 0;
    }
    int index = 1;
    bool positional = false;
    while (index < argc && (argv[index][0] == '-' || argv[index][0] == '+')) {
        if (strcmp(argv[index], "--") == 0) { positional = true; ++index; break; }
        if (strcmp(argv[index], "-") == 0) { shell->xtrace = false; shell->verbose = false; ++index; break; }
        bool enabled = argv[index][0] == '-';
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            if (*p == 'o') {
                if (p[1] != '\0') return data_error("set", "-o must end an option group");
                if (index + 1 == argc) {
                    shell_print_options(shell, stdout, !enabled);
                    return 0;
                }
                if (shell_option(shell, argv[++index], enabled) != 0) return 2;
                break;
            }
            const char *name = short_option(*p);
            if (name == NULL || shell_option(shell, name, enabled) != 0) {
                fprintf(stderr, "set: invalid option: %c\n", *p);
                return 2;
            }
        }
        ++index;
    }
    if (positional || index < argc) shell_set_positional(shell, (size_t)(argc - index), argv + index);
    return 0;
}

static int shopt_command(Shell *shell, int argc, char **argv)
{
    bool setting = false;
    bool enabled = false;
    bool quiet = false;
    bool reusable = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "-s") == 0) { setting = true; enabled = true; }
        else if (strcmp(argv[index], "-u") == 0) { setting = true; enabled = false; }
        else if (strcmp(argv[index], "-q") == 0) quiet = true;
        else if (strcmp(argv[index], "-p") == 0) reusable = true;
        else if (strcmp(argv[index], "-o") != 0) return data_error("shopt", "invalid option");
        ++index;
    }
    if (index == argc) {
        if (!quiet) shell_print_options(shell, stdout, reusable);
        return 0;
    }
    int result = 0;
    for (; index < argc; ++index) {
        if (setting) {
            if (shell_option(shell, argv[index], enabled) != 0) result = 1;
        } else {
            bool value;
            if (builtin_option_state(shell, argv[index], &value) != 0) {
                fprintf(stderr, "shopt: unknown option: %s\n", argv[index]);
                result = 1;
                continue;
            }
            if (!value) result = 1;
            if (!quiet) {
                if (reusable) printf("shopt %s %s\n", value ? "-s" : "-u", argv[index]);
                else printf("%-16s %s\n", argv[index], value ? "on" : "off");
            }
        }
    }
    return result;
}

static int getopts_command(Shell *shell, int argc, char **argv)
{
    if (argc < 3 || !valid_name(argv[2])) return data_error("getopts", "usage: getopts OPTSTRING NAME [ARG ...]");
    const char *spec = argv[1];
    bool silent = spec[0] == ':';
    if (silent) ++spec;
    int64_t optind = 1;
    const char *saved = shell_get(shell, "OPTIND");
    if (saved != NULL && (parse_number(saved, &optind) != 0 || optind < 1)) optind = 1;
    int64_t offset = 1;
    const char *last = shell_get(shell, "_MISH_GETOPTS_INDEX");
    int64_t previous;
    if (last != NULL && parse_number(last, &previous) == 0 && previous == optind) {
        const char *place = shell_get(shell, "_MISH_GETOPTS_OFFSET");
        if (place != NULL && parse_number(place, &offset) != 0) offset = 1;
    }
    char **values = argc > 3 ? argv + 3 : shell->positional;
    size_t count = argc > 3 ? (size_t)(argc - 3) : shell->positional_count;
    if ((uint64_t)optind > count || values[optind - 1][0] != '-' || values[optind - 1][1] == '\0') {
        shell_set(shell, argv[2], "?");
        shell_unset(shell, "OPTARG");
        return 1;
    }
    const char *argument = values[optind - 1];
    if (strcmp(argument, "--") == 0) {
        char *number = str_printf("%lld", (long long)(optind + 1));
        shell_set(shell, "OPTIND", number);
        free(number);
        shell_set(shell, argv[2], "?");
        shell_unset(shell, "OPTARG");
        return 1;
    }
    size_t length = strlen(argument);
    if (offset < 1 || (uint64_t)offset >= length) offset = 1;
    char option = argument[offset++];
    const char *definition = strchr(spec, option);
    char result[2] = { option, '\0' };
    shell_unset(shell, "OPTARG");
    if (definition == NULL || option == ':') {
        result[0] = '?';
        if (silent) { char error[2] = { option, '\0' }; shell_set(shell, "OPTARG", error); }
        else fprintf(stderr, "getopts: invalid option: -%c\n", option);
    } else if (definition[1] == ':') {
        if ((uint64_t)offset < length) {
            shell_set(shell, "OPTARG", argument + offset);
            offset = (int64_t)length;
        } else if ((uint64_t)optind < count) {
            shell_set(shell, "OPTARG", values[optind]);
            ++optind;
        } else {
            result[0] = silent ? ':' : '?';
            if (silent) { char error[2] = { option, '\0' }; shell_set(shell, "OPTARG", error); }
            else fprintf(stderr, "getopts: option requires an argument: -%c\n", option);
        }
    }
    if ((uint64_t)offset >= length || (definition != NULL && definition[1] == ':')) {
        ++optind;
        offset = 1;
    }
    char *number = str_printf("%lld", (long long)optind);
    shell_set(shell, "OPTIND", number);
    shell_set(shell, "_MISH_GETOPTS_INDEX", number);
    free(number);
    number = str_printf("%lld", (long long)offset);
    shell_set(shell, "_MISH_GETOPTS_OFFSET", number);
    free(number);
    return shell_set(shell, argv[2], result) == 0 ? 0 : 1;
}

static double monotonic_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static bool read_separator(const char *ifs, const char *text, const char *cursor,
                           const Buffer *protected)
{
    size_t position = (size_t)(cursor - text);
    return *cursor != '\0' && position < protected->len &&
        protected->data[position] == 0 && strchr(ifs, *cursor) != NULL;
}

static int read_command(Shell *shell, int argc, char **argv)
{
    bool raw = false;
    bool silent = false;
    int fd = STDIN_FILENO;
    int64_t maximum = -1;
    bool exact = false;
    unsigned char delimiter = '\n';
    double timeout = -1;
    const char *prompt = NULL;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        if (strcmp(argv[index], "-r") == 0) raw = true;
        else if (strcmp(argv[index], "-s") == 0) silent = true;
        else if (strcmp(argv[index], "-p") == 0 && index + 1 < argc) prompt = argv[++index];
        else if (strcmp(argv[index], "-d") == 0 && index + 1 < argc) delimiter = (unsigned char)argv[++index][0];
        else if ((strcmp(argv[index], "-n") == 0 || strcmp(argv[index], "-N") == 0) && index + 1 < argc) {
            exact = argv[index][1] == 'N';
            if (parse_number(argv[++index], &maximum) != 0 || maximum < 0 || maximum > MISH_MAX_SOURCE) return data_error("read", "invalid count");
        } else if (strcmp(argv[index], "-u") == 0 && index + 1 < argc) {
            int64_t value;
            if (parse_number(argv[++index], &value) != 0 || value < 0 || value > INT_MAX) return data_error("read", "invalid descriptor");
            fd = (int)value;
        } else if (strcmp(argv[index], "-t") == 0 && index + 1 < argc) {
            char *end;
            timeout = strtod(argv[++index], &end);
            if (*end != '\0' || timeout < 0 || timeout > 2147483) return data_error("read", "invalid timeout");
        } else return data_error("read", "invalid option or missing option argument");
        ++index;
    }
    for (int i = index; i < argc; ++i) {
        if (!valid_name(argv[i])) return data_error("read", "invalid variable name");
        Var *var = shell_find_var(shell, argv[i]);
        if (var != NULL && var->readonly) return data_error("read", "cannot assign a readonly variable");
    }
    if (prompt != NULL && isatty(fd)) { fputs(prompt, stderr); fflush(stderr); }
    struct termios saved;
    bool terminal = isatty(fd) && tcgetattr(fd, &saved) == 0;
    if (terminal && (silent || maximum >= 0)) {
        struct termios changed = saved;
        if (silent) changed.c_lflag &= (tcflag_t)~ECHO;
        if (maximum >= 0) { changed.c_lflag &= (tcflag_t)~ICANON; changed.c_cc[VMIN] = 1; changed.c_cc[VTIME] = 0; }
        tcsetattr(fd, TCSANOW, &changed);
    }
    double deadline = timeout >= 0 ? monotonic_time() + timeout : 0;
    Buffer input;
    Buffer protected;
    buffer_init(&input);
    buffer_init(&protected);
    int status = 0;
    bool quoted = false;
    while (maximum < 0 || input.len < (size_t)maximum) {
        if (timeout >= 0) {
            double remaining = deadline - monotonic_time();
            int milliseconds = remaining <= 0 ? 0 : (int)(remaining * 1000.0) + 1;
            struct pollfd descriptor = { .fd = fd, .events = POLLIN };
            int ready = poll(&descriptor, 1, milliseconds);
            if (ready == 0) { status = 142; break; }
            if (ready < 0) { if (errno == EINTR) continue; status = 1; break; }
        }
        unsigned char character;
        ssize_t received = read(fd, &character, 1);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) { status = 1; break; }
        if (!raw && !quoted && character == '\\') { quoted = true; continue; }
        if (quoted && character == '\n') { quoted = false; continue; }
        if (!quoted && !exact && character == delimiter) break;
        bool escaped = quoted;
        quoted = false;
        if (character == '\0') continue;
        if (input.len >= MISH_MAX_SOURCE) { status = 1; break; }
        buffer_putc(&input, (char)character);
        buffer_putc(&protected, escaped ? 1 : 0);
    }
    if (terminal && (silent || maximum >= 0)) tcsetattr(fd, TCSANOW, &saved);
    char *text = buffer_take(&input);
    if (index == argc) {
        if (shell_set(shell, "REPLY", text) != 0) status = 1;
    } else {
        const char *ifs = shell_get(shell, "IFS");
        if (ifs == NULL) ifs = " \t\n";
        if (exact) ifs = "";
        char *cursor = text;
        while (*cursor != '\0' && read_separator(ifs, text, cursor, &protected) && isspace((unsigned char)*cursor)) ++cursor;
        for (int i = index; i < argc; ++i) {
            char *start = cursor;
            if (i == argc - 1) {
                char *end = cursor + strlen(cursor);
                while (end > cursor && read_separator(ifs, text, end - 1, &protected) && isspace((unsigned char)end[-1])) --end;
                *end = '\0';
                char *boundary = cursor;
                while (*boundary != '\0' && !read_separator(ifs, text, boundary, &protected)) ++boundary;
                char *after = boundary;
                while (read_separator(ifs, text, after, &protected) && isspace((unsigned char)*after)) ++after;
                if (read_separator(ifs, text, after, &protected) && !isspace((unsigned char)*after)) ++after;
                while (read_separator(ifs, text, after, &protected) && isspace((unsigned char)*after)) ++after;
                if (*after == '\0') *boundary = '\0';
            } else {
                while (*cursor != '\0' && !read_separator(ifs, text, cursor, &protected)) ++cursor;
                if (*cursor != '\0') {
                    bool whitespace = isspace((unsigned char)*cursor) != 0;
                    *cursor++ = '\0';
                    while (read_separator(ifs, text, cursor, &protected) && isspace((unsigned char)*cursor)) ++cursor;
                    if (whitespace && read_separator(ifs, text, cursor, &protected) && !isspace((unsigned char)*cursor)) {
                        ++cursor;
                        while (read_separator(ifs, text, cursor, &protected) && isspace((unsigned char)*cursor)) ++cursor;
                    }
                }
            }
            if (shell_set(shell, argv[i], start) != 0) status = 1;
        }
    }
    free(text);
    buffer_free(&protected);
    return status;
}

int builtin_data_run(Shell *shell, int argc, char **argv)
{
    const char *name = argv[0];
    if (strcmp(name, "echo") == 0) return echo_command(argc, argv);
    if (strcmp(name, "printf") == 0) return printf_command(shell, argc, argv);
    if (strcmp(name, "env") == 0) return env_command(shell, argc, argv);
    if (strcmp(name, "printenv") == 0) return printenv_command(shell, argc, argv);
    if (strcmp(name, "export") == 0 || strcmp(name, "readonly") == 0 || strcmp(name, "local") == 0 ||
        strcmp(name, "declare") == 0 || strcmp(name, "typeset") == 0 || strcmp(name, "variables") == 0) return variables_command(shell, argc, argv);
    if (strcmp(name, "unset") == 0) return unset_command(shell, argc, argv);
    if (strcmp(name, "set") == 0) return set_command(shell, argc, argv);
    if (strcmp(name, "shift") == 0) return shift_command(shell, argc, argv);
    if (strcmp(name, "shopt") == 0) return shopt_command(shell, argc, argv);
    if (strcmp(name, "getopts") == 0) return getopts_command(shell, argc, argv);
    if (strcmp(name, "read") == 0) return read_command(shell, argc, argv);
    return -1;
}
