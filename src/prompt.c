#include "mish.h"
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Prompt expansion deliberately treats the prompt as one double-quoted scalar.
 * Literal quote marks in PS1 are display characters, not shell quote syntax. */
static void prompt_quoted(Buffer *result, const char *text)
{
    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor == '\\' || *cursor == '"') {
            buffer_putc(result, '\\');
        }
        buffer_putc(result, *cursor);
    }
}

static void prompt_literal(Buffer *result, const char *text)
{
    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor == '\\' || *cursor == '"' || *cursor == '$' || *cursor == '`') {
            buffer_putc(result, '\\');
        }
        buffer_putc(result, *cursor);
    }
}

static char *prompt_username(const Shell *shell)
{
    const char *value = shell_get(shell, "USER");
    if (value && *value) {
        return xstrdup(value);
    }
    struct passwd *entry = getpwuid(geteuid());
    if (entry && entry->pw_name) {
        return xstrdup(entry->pw_name);
    }
    return str_printf("%lu", (unsigned long)geteuid());
}

static char *prompt_hostname(bool short_name)
{
    char host[256];
    memset(host, 0, sizeof(host));
    if (gethostname(host, sizeof(host) - 1) < 0) {
        return xstrdup("localhost");
    }
    if (short_name) {
        char *dot = strchr(host, '.');
        if (dot) {
            *dot = '\0';
        }
    }
    return xstrdup(host);
}

static char *prompt_directory(const Shell *shell, bool basename_only)
{
    const char *pwd = shell_get(shell, "PWD");
    char *owned = NULL;
    if (!pwd || !*pwd) {
        owned = getcwd(NULL, 0);
        pwd = owned ? owned : "?";
    }
    const char *home = shell_get(shell, "HOME");
    char *result;
    if (home && *home && !strcmp(pwd, home)) {
        result = xstrdup("~");
    } else if (basename_only) {
        const char *last = strrchr(pwd, '/');
        result = xstrdup(last && last[1] ? last + 1 : pwd);
    } else if (home && *home && !strncmp(pwd, home, strlen(home)) &&
               pwd[strlen(home)] == '/') {
        result = str_printf("~%s", pwd + strlen(home));
    } else {
        result = xstrdup(pwd);
    }
    free(owned);
    return result;
}

static void prompt_time(Buffer *result, const char *format)
{
    time_t now = time(NULL);
    struct tm local;
    char text[512];
    if (!localtime_r(&now, &local)) {
        return;
    }
    size_t length = strftime(text, sizeof(text), format, &local);
    if (length) {
        prompt_literal(result, text);
    }
}

static void prompt_number(Buffer *result, size_t number)
{
    char *text = str_printf("%zu", number);
    prompt_literal(result, text);
    free(text);
}

static void prompt_escape(Shell *shell, Buffer *result, char escape)
{
    char *text = NULL;
    switch (escape) {
    case 'u':
        text = prompt_username(shell);
        break;
    case 'h':
        text = prompt_hostname(true);
        break;
    case 'H':
        text = prompt_hostname(false);
        break;
    case 'w':
        text = prompt_directory(shell, false);
        break;
    case 'W':
        text = prompt_directory(shell, true);
        break;
    case '$':
        prompt_literal(result, geteuid() == 0 ? "#" : "$");
        break;
    case '!':
        prompt_number(result, shell->history.len + 1);
        break;
    case '#': {
        const char *number = shell_get(shell, "MISH_COMMAND_NUMBER");
        prompt_literal(result, number ? number : "1");
        break;
    }
    case '?':
        text = str_printf("%d", shell->last_status);
        break;
    case 's':
        prompt_literal(result, "mish");
        break;
    case 'v':
        prompt_literal(result, MISH_VERSION);
        break;
    case 'V':
        prompt_literal(result, MISH_VERSION);
        break;
    case 't':
        prompt_time(result, "%H:%M:%S");
        break;
    case 'T':
        prompt_time(result, "%I:%M:%S");
        break;
    case '@':
        prompt_time(result, "%I:%M %p");
        break;
    case 'A':
        prompt_time(result, "%H:%M");
        break;
    case 'd':
        prompt_time(result, "%a %b %d");
        break;
    case 'n':
        buffer_putc(result, '\n');
        break;
    case 'r':
        buffer_putc(result, '\r');
        break;
    case 'a':
        buffer_putc(result, '\a');
        break;
    case 'e':
        buffer_putc(result, '\033');
        break;
    case '\\':
        prompt_literal(result, "\\");
        break;
    case '[':
    case ']':
        /* ANSI sequences themselves are measured as zero width by the editor. */
        break;
    default:
        buffer_putc(result, '\\');
        buffer_putc(result, '\\');
        buffer_putc(result, escape);
        break;
    }
    if (text) {
        prompt_literal(result, text);
        free(text);
    }
}

char *expand_prompt(Shell *shell, const char *prompt)
{
    Buffer encoded;
    buffer_init(&encoded);
    buffer_putc(&encoded, '"');
    for (size_t index = 0; prompt[index]; index++) {
        if (prompt[index] != '\\' || !prompt[index + 1]) {
            char literal[2] = { prompt[index], '\0' };
            prompt_quoted(&encoded, literal);
            continue;
        }
        char escape = prompt[++index];
        if (escape == 'D' && prompt[index + 1] == '{') {
            size_t start = index + 2;
            const char *close = strchr(prompt + start, '}');
            if (close) {
                char *format = xstrndup(prompt + start, (size_t)(close - prompt - start));
                prompt_time(&encoded, *format ? format : "%c");
                free(format);
                index = (size_t)(close - prompt);
                continue;
            }
        }
        if (escape >= '0' && escape <= '7') {
            unsigned value = (unsigned)(escape - '0');
            for (unsigned digit = 1; digit < 3; digit++) {
                char next = prompt[index + 1];
                if (next < '0' || next > '7') {
                    break;
                }
                value = value * 8 + (unsigned)(next - '0');
                index++;
            }
            if ((value & 255u) != 0) {
                char literal[2] = { (char)(value & 255u), '\0' };
                prompt_literal(&encoded, literal);
            }
            continue;
        }
        prompt_escape(shell, &encoded, escape);
    }
    buffer_putc(&encoded, '"');
    int status = 0;
    int saved_status = shell->last_status;
    Flow saved_flow = shell->flow;
    int saved_flow_status = shell->flow_status;
    unsigned saved_flow_level = shell->flow_level;
    char *result = expand_scalar(shell, encoded.data, &status);
    shell->last_status = saved_status;
    shell->flow = saved_flow;
    shell->flow_status = saved_flow_status;
    shell->flow_level = saved_flow_level;
    buffer_free(&encoded);
    if (!result || status) {
        free(result);
        return xstrdup("> ");
    }
    return result;
}
