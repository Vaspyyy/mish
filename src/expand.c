#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    Buffer text;
    Buffer quoted;
    bool preserve_empty;
    bool failed;
} Expansion;

static void expansion_init(Expansion *expansion)
{
    memset(expansion, 0, sizeof(*expansion));
    buffer_init(&expansion->text);
    buffer_init(&expansion->quoted);
}

static void expansion_free(Expansion *expansion)
{
    buffer_free(&expansion->text);
    buffer_free(&expansion->quoted);
}

static void expansion_append(Expansion *expansion, const char *text, bool quoted)
{
    for (; *text; ++text) {
        buffer_putc(&expansion->text, *text);
        buffer_putc(&expansion->quoted, quoted ? 'q' : 'u');
    }
}

static void expansion_char(Expansion *expansion, char character, bool quoted)
{
    buffer_putc(&expansion->text, character);
    buffer_putc(&expansion->quoted, quoted ? 'q' : 'l');
}

static char *special_parameter(Shell *shell, const char *name, bool *defined)
{
    *defined = true;
    if (!strcmp(name, "?") || !strcmp(name, "$status") || !strcmp(name, "$")) {
        if (!strcmp(name, "$"))
            return str_printf("%ld", (long)shell->shell_pid);
        return str_printf("%d", shell->last_status);
    }
    if (!strcmp(name, "#"))
        return str_printf("%zu", shell->positional_count);
    if (!strcmp(name, "!"))
        return shell->last_background ? str_printf("%ld", (long)shell->last_background) : xstrdup("");
    if (!strcmp(name, "0"))
        return xstrdup(shell->argv0);
    if (!strcmp(name, "-")) {
        Buffer flags;
        buffer_init(&flags);
        if (shell->errexit) buffer_putc(&flags, 'e');
        if (shell->nounset) buffer_putc(&flags, 'u');
        if (shell->noglob) buffer_putc(&flags, 'f');
        if (shell->xtrace) buffer_putc(&flags, 'x');
        if (shell->interactive) buffer_putc(&flags, 'i');
        return buffer_take(&flags);
    }
    if (!strcmp(name, "@") || !strcmp(name, "*")) {
        Buffer joined;
        buffer_init(&joined);
        const char *ifs = shell_get(shell, "IFS");
        char separator = ifs ? *ifs : ' ';
        for (size_t i = 0; i < shell->positional_count; ++i) {
            if (i && separator)
                buffer_putc(&joined, separator);
            buffer_append(&joined, shell->positional[i]);
        }
        return buffer_take(&joined);
    }
    if (*name && strspn(name, "0123456789") == strlen(name)) {
        errno = 0;
        char *end;
        unsigned long position = strtoul(name, &end, 10);
        if (!errno && !*end && position > 0 && position <= shell->positional_count)
            return xstrdup(shell->positional[position - 1]);
        *defined = false;
        return xstrdup("");
    }
    const char *value = shell_get(shell, name);
    *defined = value != NULL;
    return xstrdup(value ? value : "");
}

static size_t matching_close(const char *text, size_t start, char opener, char closer)
{
    (void)opener;
    (void)closer;
    ParseError error = {0};
    size_t end = SIZE_MAX;
    if (lexer_expansion_end(text, start - 2, &end, &error))
        end = SIZE_MAX;
    parse_error_free(&error);
    return end;
}

static bool variable_identifier(const char *name)
{
    if (valid_name(name))
        return true;
    return strlen(name) == 1 && strchr("?$!#@*0123456789-", *name);
}

static char *remove_pattern(const char *value, const char *pattern, bool prefix, bool longest)
{
    size_t length = strlen(value);
    size_t begin = longest ? length : 0;
    size_t end = longest ? 0 : length;
    for (size_t size = begin;; size += longest ? (size_t)-1 : 1) {
        char *candidate = prefix ? xstrndup(value, size) : xstrdup(value + length - size);
        bool match = fnmatch(pattern, candidate, 0) == 0;
        free(candidate);
        if (match)
            return prefix ? xstrdup(value + size) : xstrndup(value, length - size);
        if (size == end)
            break;
    }
    return xstrdup(value);
}

static char *replace_pattern(const char *value, const char *pattern, const char *replacement, bool all)
{
    Buffer result;
    buffer_init(&result);
    size_t length = strlen(value);
    size_t position = 0;
    bool replaced = false;
    while (position < length) {
        size_t matched = 0;
        if (!replaced || all) {
            for (size_t end = length; end > position; --end) {
                char *candidate = xstrndup(value + position, end - position);
                bool match = fnmatch(pattern, candidate, 0) == 0;
                free(candidate);
                if (match) {
                    matched = end - position;
                    break;
                }
            }
        }
        if (matched) {
            buffer_append(&result, replacement);
            position += matched;
            replaced = true;
        } else {
            buffer_putc(&result, value[position++]);
        }
    }
    return buffer_take(&result);
}

static int expand_text(Shell *shell, const char *word, Expansion *expansion, bool heredoc);

static const char *substring_separator(const char *text)
{
    unsigned parentheses = 0;
    unsigned ternaries = 0;
    for (const char *position = text; *position; ++position) {
        if (*position == '(') ++parentheses;
        else if (*position == ')' && parentheses) --parentheses;
        else if (!parentheses && *position == '?') ++ternaries;
        else if (!parentheses && *position == ':') {
            if (ternaries) --ternaries;
            else return position;
        }
    }
    return NULL;
}

static char *parameter_braced(Shell *shell, const char *content, int *status, Buffer *selected_mask, bool *preserve_empty)
{
    bool length_operator = content[0] == '#' && content[1];
    const char *start = content + (length_operator ? 1 : 0);
    size_t name_length = 0;
    if (strchr("?$!#@*0123456789-", *start) && *start)
        name_length = strspn(start, "0123456789") ? strspn(start, "0123456789") : 1;
    else {
        while (isalnum((unsigned char)start[name_length]) || start[name_length] == '_')
            ++name_length;
    }
    if (!name_length) {
        fprintf(stderr, "mish: bad parameter expansion: ${%s}\n", content);
        *status = 1;
        return NULL;
    }
    char *name = xstrndup(start, name_length);
    bool defined;
    char *value = special_parameter(shell, name, &defined);
    const char *operation = start + name_length;
    char default_operator = operation[0] == ':' ? operation[1] : operation[0];
    bool default_form = default_operator && strchr("-+=?", default_operator);
    if (!defined && shell->nounset && (length_operator || !default_form)) {
        fprintf(stderr, "mish: %s: unbound variable\n", name);
        *status = 1;
        free(name);
        free(value);
        return NULL;
    }
    if (length_operator) {
        if (*operation) {
            fprintf(stderr, "mish: invalid length expansion: ${%s}\n", content);
            *status = 1;
            free(value);
            free(name);
            return NULL;
        }
        char *result = str_printf("%zu", strlen(value));
        free(value);
        free(name);
        return result;
    }
    if (!*operation) {
        if (!defined && shell->nounset) {
            fprintf(stderr, "mish: %s: unbound variable\n", name);
            *status = 1;
            free(value);
            value = NULL;
        }
        free(name);
        return value;
    }
    bool colon = operation[0] == ':';
    char operator = operation[colon ? 1 : 0];
    if (strchr("-+=?", operator)) {
        bool missing = !defined || (colon && !*value);
        const char *word = operation + (colon ? 2 : 1);
        bool use_word = operator == '+' ? !missing : missing;
        if (use_word) {
            Expansion selected;
            expansion_init(&selected);
            int expansion_status = expand_text(shell, word, &selected, false);
            char *expanded = expansion_status ? NULL : buffer_take(&selected.text);
            if (!expansion_status) {
                for (size_t i = 0; i < selected.quoted.len; ++i)
                    buffer_putc(selected_mask, selected.quoted.data[i] == 'l' ? 'u' : selected.quoted.data[i]);
                *preserve_empty = selected.preserve_empty;
            }
            expansion_free(&selected);
            if (expansion_status) {
                *status = expansion_status;
                free(value);
                free(name);
                return NULL;
            }
            if (operator == '?') {
                fprintf(stderr, "mish: %s: %s\n", name,
                        *expanded ? expanded : "parameter null or not set");
                *status = 1;
                free(expanded);
                free(value);
                free(name);
                return NULL;
            }
            if (operator == '=' && shell_set(shell, name, expanded)) {
                *status = 1;
                free(expanded);
                free(value);
                free(name);
                return NULL;
            }
            free(value);
            value = expanded;
        } else if (operator == '+') {
            free(value);
            value = xstrdup("");
        }
    } else if (operator == '#' || operator == '%') {
        bool longest = operation[1] == operator;
        int pattern_status = 0;
        char *pattern = expand_pattern(shell, operation + (longest ? 2 : 1), &pattern_status);
        if (pattern_status) {
            *status = pattern_status;
            free(value);
            free(name);
            return NULL;
        }
        char *result = remove_pattern(value, pattern, operator == '#', longest);
        free(value);
        free(pattern);
        value = result;
    } else if (*operation == ':' && operator != ':') {
        const char *offset_start = operation + 1;
        const char *separator = substring_separator(offset_start);
        char *offset_text = separator ? xstrndup(offset_start, (size_t)(separator - offset_start)) : xstrdup(offset_start);
        int64_t offset = 0;
        char *error = NULL;
        if (arithmetic_eval(shell, offset_text, &offset, &error)) {
            fprintf(stderr, "mish: substring: %s\n", error ? error : "invalid offset");
            free(error);
            *status = 1;
        }
        free(offset_text);
        size_t length = strlen(value);
        if (offset < 0)
            offset += (int64_t)length;
        if (offset < 0)
            offset = 0;
        if ((uint64_t)offset > length)
            offset = (int64_t)length;
        size_t take = length - (size_t)offset;
        if (separator && !*status) {
            int64_t requested;
            if (arithmetic_eval(shell, separator + 1, &requested, &error) || requested < 0) {
                fprintf(stderr, "mish: substring: invalid length\n");
                free(error);
                *status = 1;
            } else if ((uint64_t)requested < take) {
                take = (size_t)requested;
            }
        }
        char *result = xstrndup(value + offset, take);
        free(value);
        value = result;
    } else if (*operation == '/') {
        bool all = operation[1] == '/';
        const char *pattern_start = operation + (all ? 2 : 1);
        const char *separator = strchr(pattern_start, '/');
        char *pattern_raw = separator ? xstrndup(pattern_start, (size_t)(separator - pattern_start)) : xstrdup(pattern_start);
        char *pattern = expand_pattern(shell, pattern_raw, status);
        char *replacement = expand_scalar(shell, separator ? separator + 1 : "", status);
        free(pattern_raw);
        if (pattern && replacement) {
            char *result = replace_pattern(value, pattern, replacement, all);
            free(value);
            value = result;
        }
        free(pattern);
        free(replacement);
    } else if (*operation == '^' || *operation == ',') {
        bool all = operation[1] == operation[0];
        for (size_t i = 0; value[i]; ++i) {
            value[i] = (char)(*operation == '^' ? toupper((unsigned char)value[i]) : tolower((unsigned char)value[i]));
            if (!all)
                break;
        }
    } else {
        fprintf(stderr, "mish: unsupported parameter operator: ${%s}\n", content);
        *status = 1;
    }
    free(name);
    if (*status) {
        free(value);
        return NULL;
    }
    return value;
}

static int ansi_hex(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void ansi_codepoint(Expansion *expansion, unsigned long value)
{
    if (!value || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return;
    char bytes[5] = {0};
    if (value < 0x80) {
        bytes[0] = (char)value;
    } else if (value < 0x800) {
        bytes[0] = (char)(0xc0 | (value >> 6));
        bytes[1] = (char)(0x80 | (value & 63));
    } else if (value < 0x10000) {
        bytes[0] = (char)(0xe0 | (value >> 12));
        bytes[1] = (char)(0x80 | ((value >> 6) & 63));
        bytes[2] = (char)(0x80 | (value & 63));
    } else {
        bytes[0] = (char)(0xf0 | (value >> 18));
        bytes[1] = (char)(0x80 | ((value >> 12) & 63));
        bytes[2] = (char)(0x80 | ((value >> 6) & 63));
        bytes[3] = (char)(0x80 | (value & 63));
    }
    expansion_append(expansion, bytes, true);
}

static int ansi_quote(const char *word, size_t *position, Expansion *expansion)
{
    size_t i = *position + 2;
    expansion->preserve_empty = true;
    while (word[i] && word[i] != '\'') {
        char ch = word[i++];
        if (ch != '\\') {
            expansion_char(expansion, ch, true);
            continue;
        }
        ch = word[i++];
        if (!ch) {
            fprintf(stderr, "mish: unfinished ANSI quote escape\n");
            return 1;
        }
        switch (ch) {
        case 'a': ch = '\a'; break;
        case 'b': ch = '\b'; break;
        case 'e': case 'E': ch = '\x1b'; break;
        case 'f': ch = '\f'; break;
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        case 'v': ch = '\v'; break;
        case '\\': case '\'': case '"': break;
        case 'c':
            if (!word[i]) return 1;
            ch = (char)(toupper((unsigned char)word[i++]) & 31);
            break;
        case 'x': case 'u': case 'U': {
            unsigned digits = ch == 'x' ? 2 : ch == 'u' ? 4 : 8;
            unsigned long value = 0;
            unsigned count = 0;
            while (count < digits && word[i] && ansi_hex(word[i]) >= 0) {
                value = value * 16 + (unsigned)ansi_hex(word[i++]);
                ++count;
            }
            if (!count) {
                expansion_char(expansion, '\\', true);
                expansion_char(expansion, ch, true);
            } else if (ch == 'x') {
                if (value) expansion_char(expansion, (char)value, true);
            } else {
                ansi_codepoint(expansion, value);
            }
            continue;
        }
        default:
            if (ch >= '0' && ch <= '7') {
                unsigned value = (unsigned)(ch - '0');
                unsigned count = 1;
                while (count < 3 && word[i] >= '0' && word[i] <= '7') {
                    value = value * 8 + (unsigned)(word[i++] - '0');
                    ++count;
                }
                if (value & 255) expansion_char(expansion, (char)value, true);
                continue;
            }
            expansion_char(expansion, '\\', true);
            break;
        }
        if (ch) expansion_char(expansion, ch, true);
    }
    if (word[i] != '\'') {
        fprintf(stderr, "mish: unterminated ANSI quote\n");
        return 1;
    }
    *position = i + 1;
    return 0;
}

static int expand_text(Shell *shell, const char *word, Expansion *expansion, bool heredoc)
{
    char quote = 0;
    size_t i = 0;
    while (word[i]) {
        char ch = word[i];
        if (!heredoc && !quote && ch == '$' && word[i + 1] == '\'') {
            if (ansi_quote(word, &i, expansion))
                return 1;
            continue;
        }
        if (!heredoc && (ch == '\'' || ch == '"') && (!quote || quote == ch)) {
            quote = quote ? 0 : ch;
            expansion->preserve_empty = true;
            ++i;
            continue;
        }
        bool quoted = heredoc || quote != 0;
        if (ch == '\\' && quote != '\'') {
            char next = word[i + 1];
            if (!next) {
                fprintf(stderr, "mish: unfinished escape\n");
                return 1;
            }
            if ((quote == '"' || heredoc) && !strchr(heredoc ? "$`\\\n" : "$`\\\"\n", next)) {
                expansion_char(expansion, ch, quoted);
                ++i;
                continue;
            }
            if (next != '\n')
                expansion_char(expansion, next, true);
            i += 2;
            continue;
        }
        if (quote == '\'') {
            expansion_char(expansion, ch, true);
            ++i;
            continue;
        }
        if (!heredoc && ch == '~' && i == 0 && !quote) {
            size_t end = 1;
            while (word[end] && word[end] != '/' && word[end] != ':')
                ++end;
            char *user = xstrndup(word + 1, end - 1);
            const char *home = NULL;
            if (!*user)
                home = shell_get(shell, "HOME");
            else if (!strcmp(user, "+"))
                home = shell_get(shell, "PWD");
            else if (!strcmp(user, "-"))
                home = shell_get(shell, "OLDPWD");
            else {
                struct passwd *entry = getpwnam(user);
                if (entry)
                    home = entry->pw_dir;
            }
            free(user);
            if (home) {
                expansion_append(expansion, home, true);
                i = end;
                continue;
            }
        }
        if (ch == '`') {
            size_t end = i + 1;
            while (word[end] && word[end] != '`') {
                if (word[end] == '\\' && word[end + 1])
                    ++end;
                ++end;
            }
            if (!word[end]) {
                fprintf(stderr, "mish: unterminated backtick\n");
                return 1;
            }
            char *source = xstrndup(word + i + 1, end - i - 1);
            char *output = NULL;
            int status = shell_capture(shell, source, &output);
            free(source);
            if (!output)
                return status ? status : 1;
            expansion_append(expansion, output, quoted);
            free(output);
            i = end + 1;
            continue;
        }
        if (ch != '$') {
            expansion_char(expansion, ch, quoted);
            ++i;
            continue;
        }
        char next = word[i + 1];
        if (next == '(' || next == '[' || next == '{') {
            char close = next == '(' ? ')' : next == '[' ? ']' : '}';
            size_t end = matching_close(word, i + 2, next, close);
            if (end == SIZE_MAX) {
                fprintf(stderr, "mish: unterminated expansion\n");
                return 1;
            }
            char *content = xstrndup(word + i + 2, end - i - 2);
            char *value = NULL;
            int status = 0;
            Buffer selected_mask;
            buffer_init(&selected_mask);
            bool selected_empty = false;
            if (next == '{') {
                value = parameter_braced(shell, content, &status, &selected_mask, &selected_empty);
            } else if (next == '(' && content[0] == '(' && end > i + 2 && content[strlen(content) - 1] == ')') {
                content[strlen(content) - 1] = '\0';
                int64_t number;
                char *error = NULL;
                status = arithmetic_eval(shell, content + 1, &number, &error);
                if (status) {
                    status = 1;
                    fprintf(stderr, "mish: arithmetic: %s\n", error ? error : "invalid expression");
                    free(error);
                } else {
                    value = str_printf("%" PRId64, number);
                }
            } else if (next == '(' && (!strcmp(content, "$") || variable_identifier(content))) {
                bool defined = true;
                if (!strcmp(content, "$"))
                    value = str_printf("%d", shell->last_status);
                else
                    value = special_parameter(shell, content, &defined);
                if (shell->nounset && !defined) {
                    fprintf(stderr, "mish: %s: unbound variable\n", content);
                    free(value);
                    value = NULL;
                    status = 1;
                }
            } else if (next == '(' && content[0] == '$' && content[1] == '(') {
                ParseError error = {0};
                size_t nested_end;
                bool nested_name = !lexer_expansion_end(content, 0, &nested_end, &error) && nested_end + 1 == strlen(content);
                parse_error_free(&error);
                if (nested_name) {
                    char *name = expand_scalar(shell, content, &status);
                    if (name) {
                        bool defined;
                        value = special_parameter(shell, name, &defined);
                        if (!defined && shell->nounset) {
                            fprintf(stderr, "mish: %s: unbound variable\n", name);
                            free(value);
                            value = NULL;
                            status = 1;
                        }
                        free(name);
                    }
                } else {
                    status = shell_capture(shell, content, &value);
                }
                if (value) status = 0;
            } else {
                status = shell_capture(shell, content, &value);
                if (value)
                    status = 0;
            }
            free(content);
            if (!value) {
                buffer_free(&selected_mask);
                return status ? status : 1;
            }
            if (selected_mask.len == strlen(value) && selected_mask.len) {
                for (size_t position = 0; value[position]; ++position) {
                    buffer_putc(&expansion->text, value[position]);
                    buffer_putc(&expansion->quoted, quoted ? 'q' : selected_mask.data[position]);
                }
            } else {
                expansion_append(expansion, value, quoted);
            }
            if (selected_empty)
                expansion->preserve_empty = true;
            buffer_free(&selected_mask);
            free(value);
            i = end + 1;
            continue;
        }
        size_t length = 0;
        if (next && strchr("?$!#@*0123456789-", next))
            length = 1;
        else if (isalpha((unsigned char)next) || next == '_') {
            length = 1;
            while (isalnum((unsigned char)word[i + 1 + length]) || word[i + 1 + length] == '_')
                ++length;
        }
        if (!length) {
            expansion_char(expansion, '$', quoted);
            ++i;
            continue;
        }
        char *name = xstrndup(word + i + 1, length);
        if (!strcmp(name, "@") && quote == '"') {
            for (size_t position = 0; position < shell->positional_count; ++position) {
                if (position) {
                    buffer_putc(&expansion->text, '\x1f');
                    buffer_putc(&expansion->quoted, 'a');
                }
                expansion_append(expansion, shell->positional[position], true);
            }
        } else {
            bool defined;
            char *value = special_parameter(shell, name, &defined);
            if (!defined && shell->nounset) {
                fprintf(stderr, "mish: %s: unbound variable\n", name);
                free(name);
                free(value);
                return 1;
            }
            expansion_append(expansion, value, quoted);
            free(value);
        }
        free(name);
        i += length + 1;
    }
    if (quote) {
        fprintf(stderr, "mish: unterminated quote\n");
        return 1;
    }
    return 0;
}

static void emit_field(Shell *shell, Expansion *expansion, size_t start, size_t end, StrVec *fields, bool globbing)
{
    char *field = xstrndup(expansion->text.data + start, end - start);
    bool magic = false;
    Buffer pattern;
    buffer_init(&pattern);
    for (size_t i = start; i < end; ++i) {
        char ch = expansion->text.data[i];
        bool quoted = expansion->quoted.data[i] == 'q';
        if (!quoted && strchr("*?[", ch))
            magic = true;
        if (quoted && strchr("*?[]\\", ch))
            buffer_putc(&pattern, '\\');
        buffer_putc(&pattern, ch);
    }
    if (globbing && magic) {
        size_t before = fields->len;
        char *error = NULL;
        if (expand_glob(shell, pattern.data, fields, &error)) {
            fprintf(stderr, "mish: %s\n", error ? error : "pathname expansion failed");
            free(error);
            expansion->failed = true;
        } else if (fields->len == before && !shell->nullglob) {
            strvec_push(fields, field);
        }
        free(field);
    } else {
        strvec_push_owned(fields, field);
    }
    buffer_free(&pattern);
}

static int expand_word_single(Shell *shell, const char *word, StrVec *fields, bool split, bool globbing)
{
    if (!strcmp(word, "\"$@\"")) {
        for (size_t i = 0; i < shell->positional_count; ++i)
            strvec_push(fields, shell->positional[i]);
        return 0;
    }
    Expansion expansion;
    expansion_init(&expansion);
    int status = expand_text(shell, word, &expansion, false);
    if (status) {
        expansion_free(&expansion);
        return status;
    }
    const char *ifs = shell_get(shell, "IFS");
    if (!ifs)
        ifs = " \t\n";
    size_t start = 0;
    bool in_field = false;
    for (size_t i = 0; i < expansion.text.len; ++i) {
        char ch = expansion.text.data[i];
        char mask = expansion.quoted.data[i];
        bool delimiter = mask == 'a' || (split && mask == 'u' && strchr(ifs, ch));
        if (!delimiter) {
            if (!in_field)
                start = i;
            in_field = true;
            continue;
        }
        bool whitespace = mask != 'a' && strchr(" \t\n", ch);
        if (in_field || !whitespace)
            emit_field(shell, &expansion, in_field ? start : i, i, fields, globbing && !shell->noglob);
        in_field = false;
        start = i + 1;
    }
    if (in_field)
        emit_field(shell, &expansion, start, expansion.text.len, fields, globbing && !shell->noglob);
    else if (expansion.text.len && expansion.quoted.data[expansion.text.len - 1] == 'a')
        strvec_push(fields, "");
    else if (!expansion.text.len && expansion.preserve_empty)
        strvec_push(fields, "");
    bool failed = expansion.failed;
    expansion_free(&expansion);
    return failed ? 1 : 0;
}

int expand_word(Shell *shell, const char *word, StrVec *fields, bool split, bool globbing)
{
    if (!shell->braceexpand)
        return expand_word_single(shell, word, fields, split, globbing);
    StrVec alternatives;
    strvec_init(&alternatives);
    char *error = NULL;
    int status = expand_braces(word, &alternatives, &error);
    if (status) {
        fprintf(stderr, "mish: %s\n", error ? error : "brace expansion failed");
        free(error);
    } else {
        for (size_t i = 0; i < alternatives.len; ++i) {
            status = expand_word_single(shell, alternatives.items[i], fields, split, globbing);
            if (status)
                break;
        }
    }
    strvec_free(&alternatives);
    return status;
}

char *expand_scalar(Shell *shell, const char *word, int *status)
{
    Expansion expansion;
    expansion_init(&expansion);
    *status = expand_text(shell, word, &expansion, false);
    char *result = *status ? NULL : buffer_take(&expansion.text);
    expansion_free(&expansion);
    return result;
}

char *expand_pattern(Shell *shell, const char *word, int *status)
{
    Expansion expansion;
    expansion_init(&expansion);
    *status = expand_text(shell, word, &expansion, false);
    if (*status) {
        expansion_free(&expansion);
        return NULL;
    }
    Buffer pattern;
    buffer_init(&pattern);
    for (size_t i = 0; i < expansion.text.len; ++i) {
        char ch = expansion.text.data[i];
        if (expansion.quoted.data[i] == 'q' && strchr("*?[]\\", ch))
            buffer_putc(&pattern, '\\');
        buffer_putc(&pattern, ch);
    }
    expansion_free(&expansion);
    return buffer_take(&pattern);
}

int expand_heredoc(Shell *shell, const char *text, char **result)
{
    Expansion expansion;
    expansion_init(&expansion);
    int status = expand_text(shell, text, &expansion, true);
    *result = status ? NULL : buffer_take(&expansion.text);
    expansion_free(&expansion);
    return status;
}
