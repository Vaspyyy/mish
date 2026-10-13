#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define BRACE_LIMIT 4096
#define BRACE_DEPTH 64

typedef struct {
    size_t open;
    size_t close;
    StrVec alternatives;
} Brace;

static bool range_number(const char *text, int64_t *value)
{
    if (!*text)
        return false;
    errno = 0;
    char *end;
    long long number = strtoll(text, &end, 10);
    if (errno || *end)
        return false;
    *value = (int64_t)number;
    return true;
}

static bool zero_padded(const char *text)
{
    if (*text == '+' || *text == '-')
        ++text;
    return text[0] == '0' && isdigit((unsigned char)text[1]);
}

static int brace_sequence(const char *content, StrVec *alternatives, char **error)
{
    const char *separator = strstr(content, "..");
    if (!separator)
        return 0;
    char *first = xstrndup(content, (size_t)(separator - content));
    const char *rest = separator + 2;
    const char *second_separator = strstr(rest, "..");
    char *last = second_separator ? xstrndup(rest, (size_t)(second_separator - rest)) : xstrdup(rest);
    char *increment = second_separator ? xstrdup(second_separator + 2) : NULL;
    int64_t begin;
    int64_t finish;
    bool alphabetic = strlen(first) == 1 && strlen(last) == 1 &&
                      isalpha((unsigned char)*first) && isalpha((unsigned char)*last);
    bool numeric = range_number(first, &begin) && range_number(last, &finish);
    if (!alphabetic && !numeric) {
        free(first);
        free(last);
        free(increment);
        return 0;
    }
    if (alphabetic) {
        begin = (unsigned char)*first;
        finish = (unsigned char)*last;
    }
    int64_t step = 1;
    if (increment && (!range_number(increment, &step) || step == INT64_MIN)) {
        free(first);
        free(last);
        free(increment);
        return 0;
    }
    if (!step)
        step = 1;
    if (step < 0)
        step = -step;
    if (begin > finish)
        step = -step;
    size_t width = 0;
    if (numeric && (zero_padded(first) || zero_padded(last))) {
        width = strlen(first) > strlen(last) ? strlen(first) : strlen(last);
        if (width > 64)
            width = 64;
    }
    int result = 1;
    for (int64_t value = begin;;) {
        if (alternatives->len >= BRACE_LIMIT) {
            *error = xstrdup("brace expansion exceeds 4096 results");
            result = -1;
            break;
        }
        if (alphabetic) {
            char letter[2] = {(char)value, '\0'};
            strvec_push(alternatives, letter);
        } else if (width) {
            strvec_push_owned(alternatives, str_printf("%0*lld", (int)width, (long long)value));
        } else {
            strvec_push_owned(alternatives, str_printf("%lld", (long long)value));
        }
        if (value == finish)
            break;
        if ((step > 0 && value > INT64_MAX - step) ||
            (step < 0 && value < INT64_MIN - step))
            break;
        int64_t next = value + step;
        if ((step > 0 && next > finish) || (step < 0 && next < finish))
            break;
        value = next;
    }
    free(first);
    free(last);
    free(increment);
    return result;
}

static bool brace_alternatives(const char *word, size_t open, size_t close, StrVec *alternatives)
{
    size_t start = open + 1;
    unsigned depth = 0;
    char quote = 0;
    bool found_comma = false;
    for (size_t i = start; i < close; ++i) {
        char ch = word[i];
        if (ch == '\\' && quote != '\'' && i + 1 < close) {
            ++i;
            continue;
        }
        if ((ch == '\'' || ch == '"') && (!quote || quote == ch)) {
            quote = quote ? 0 : ch;
            continue;
        }
        if (quote)
            continue;
        if (ch == '{')
            ++depth;
        else if (ch == '}' && depth)
            --depth;
        else if (ch == ',' && !depth) {
            strvec_push_owned(alternatives, xstrndup(word + start, i - start));
            start = i + 1;
            found_comma = true;
        }
    }
    if (found_comma)
        strvec_push_owned(alternatives, xstrndup(word + start, close - start));
    return found_comma;
}

static size_t skip_substitution(const char *word, size_t dollar)
{
    ParseError error = {0};
    size_t close = strlen(word) - 1;
    (void)lexer_expansion_end(word, dollar, &close, &error);
    parse_error_free(&error);
    return close;
}

static int find_brace(const char *word, Brace *brace, char **error)
{
    char quote = 0;
    bool ansi = false;
    for (size_t i = 0; word[i]; ++i) {
        char ch = word[i];
        if (ch == '$' && !quote && word[i + 1] == '\'') {
            quote = '\'';
            ansi = true;
            ++i;
            continue;
        }
        if (ch == '\\' && (quote != '\'' || ansi) && word[i + 1]) {
            ++i;
            continue;
        }
        if ((ch == '\'' || ch == '"') && (!quote || quote == ch)) {
            quote = quote ? 0 : ch;
            if (!quote) ansi = false;
            continue;
        }
        if (quote)
            continue;
        if (ch == '$' && word[i + 1] && strchr("([{", word[i + 1])) {
            i = skip_substitution(word, i);
            continue;
        }
        if (ch != '{')
            continue;
        size_t close = i + 1;
        unsigned depth = 1;
        char inner_quote = 0;
        for (; word[close]; ++close) {
            char current = word[close];
            if (current == '\\' && inner_quote != '\'' && word[close + 1]) {
                ++close;
                continue;
            }
            if ((current == '\'' || current == '"') && (!inner_quote || inner_quote == current)) {
                inner_quote = inner_quote ? 0 : current;
                continue;
            }
            if (inner_quote)
                continue;
            if (current == '{')
                ++depth;
            else if (current == '}' && !--depth)
                break;
        }
        if (!word[close])
            return 0;
        StrVec alternatives;
        strvec_init(&alternatives);
        bool list = brace_alternatives(word, i, close, &alternatives);
        int sequence = 0;
        if (!list) {
            char *content = xstrndup(word + i + 1, close - i - 1);
            sequence = brace_sequence(content, &alternatives, error);
            free(content);
        }
        if (list || sequence > 0) {
            brace->open = i;
            brace->close = close;
            brace->alternatives = alternatives;
            return 1;
        }
        strvec_free(&alternatives);
        if (sequence < 0)
            return -1;
    }
    return 0;
}

static int brace_recursive(const char *word, StrVec *results, unsigned depth, char **error)
{
    if (depth >= BRACE_DEPTH) {
        *error = xstrdup("brace nesting limit exceeded");
        return 1;
    }
    if (results->len >= BRACE_LIMIT) {
        *error = xstrdup("brace expansion exceeds 4096 results");
        return 1;
    }
    Brace brace;
    memset(&brace, 0, sizeof(brace));
    int found = find_brace(word, &brace, error);
    if (found < 0)
        return 1;
    if (!found) {
        strvec_push(results, word);
        return 0;
    }
    int result = 0;
    for (size_t i = 0; i < brace.alternatives.len; ++i) {
        Buffer expanded;
        buffer_init(&expanded);
        buffer_append_n(&expanded, word, brace.open);
        buffer_append(&expanded, brace.alternatives.items[i]);
        buffer_append(&expanded, word + brace.close + 1);
        result = brace_recursive(expanded.data, results, depth + 1, error);
        buffer_free(&expanded);
        if (result)
            break;
    }
    strvec_free(&brace.alternatives);
    return result;
}

int expand_braces(const char *word, StrVec *results, char **error)
{
    *error = NULL;
    return brace_recursive(word, results, 0, error);
}
