#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY_SIGNATURE "# mish history v1\n"
#define HISTORY_DEFAULT_LIMIT 1000u
#define HISTORY_MAX_LIMIT 100000u

static size_t history_limit(Shell *shell, const char *name, size_t fallback)
{
    const char *text = shell_get(shell, name);
    int64_t value;
    if (!text || parse_number(text, &value) || value < 0) {
        return fallback;
    }
    if ((uint64_t)value > HISTORY_MAX_LIMIT) {
        return HISTORY_MAX_LIMIT;
    }
    return (size_t)value;
}

static void history_trim(History *history, size_t limit)
{
    if (history->len <= limit) {
        return;
    }
    size_t remove = history->len - limit;
    for (size_t index = 0; index < remove; index++) {
        free(history->lines[index]);
    }
    memmove(history->lines, history->lines + remove,
            (history->len - remove) * sizeof(*history->lines));
    history->len -= remove;
    history->saved = history->saved > remove ? history->saved - remove : 0;
}

static void history_push(History *history, const char *line)
{
    if (history->len == history->cap) {
        size_t capacity = history->cap ? history->cap * 2 : 32;
        history->lines = xrealloc(history->lines, capacity * sizeof(*history->lines));
        history->cap = capacity;
    }
    history->lines[history->len++] = xstrdup(line);
}

static bool history_control(const Shell *shell, const char *option)
{
    const char *control = shell_get(shell, "HISTCONTROL");
    if (!control) {
        return false;
    }
    size_t length = strlen(option);
    while (*control) {
        const char *end = strchr(control, ':');
        size_t part = end ? (size_t)(end - control) : strlen(control);
        if ((part == length && !strncmp(control, option, length)) ||
            (part == 10 && !strncmp(control, "ignoreboth", 10) &&
             (!strcmp(option, "ignorespace") || !strcmp(option, "ignoredups")))) {
            return true;
        }
        if (!end) {
            break;
        }
        control = end + 1;
    }
    return false;
}

static bool history_ignored(Shell *shell, const char *line)
{
    if (!*line) {
        return true;
    }
    bool nonspace = false;
    for (const char *cursor = line; *cursor; cursor++) {
        if (!isspace((unsigned char)*cursor)) {
            nonspace = true;
            break;
        }
    }
    if (!nonspace) {
        return true;
    }
    if (isspace((unsigned char)line[0]) && history_control(shell, "ignorespace")) {
        return true;
    }
    const char *patterns = shell_get(shell, "HISTIGNORE");
    if (!patterns) {
        return false;
    }
    while (*patterns) {
        const char *end = strchr(patterns, ':');
        size_t length = end ? (size_t)(end - patterns) : strlen(patterns);
        char *pattern = xstrndup(patterns, length);
        bool ignore;
        if (!strcmp(pattern, "&")) {
            ignore = shell->history.len &&
                     !strcmp(line, shell->history.lines[shell->history.len - 1]);
        } else {
            ignore = !fnmatch(pattern, line, 0);
        }
        free(pattern);
        if (ignore) {
            return true;
        }
        if (!end) {
            break;
        }
        patterns = end + 1;
    }
    return false;
}

void history_clear(Shell *shell)
{
    History *history = &shell->history;
    for (size_t index = 0; index < history->len; index++) {
        free(history->lines[index]);
    }
    history->len = 0;
    history->saved = 0;
}

void history_destroy(Shell *shell)
{
    history_clear(shell);
    free(shell->history.lines);
    free(shell->history.path);
    memset(&shell->history, 0, sizeof(shell->history));
}

static void history_add_normalized(Shell *shell, const char *line)
{
    History *history = &shell->history;
    history->limit = history_limit(shell, "HISTSIZE", HISTORY_DEFAULT_LIMIT);
    if (!shell->history_enabled || history_ignored(shell, line) || !history->limit) {
        return;
    }
    if (history_control(shell, "ignoredups") && history->len &&
        !strcmp(history->lines[history->len - 1], line)) {
        return;
    }
    if (history_control(shell, "erasedups")) {
        size_t index = 0;
        while (index < history->len) {
            if (strcmp(history->lines[index], line)) {
                index++;
                continue;
            }
            free(history->lines[index]);
            memmove(history->lines + index, history->lines + index + 1,
                    (history->len - index - 1) * sizeof(*history->lines));
            history->len--;
            if (history->saved > index) {
                history->saved--;
            }
        }
    }
    history_push(history, line);
    history_trim(history, history->limit);
}

void history_add(Shell *shell, const char *line)
{
    size_t length = strlen(line);
    if (length && line[length - 1] == '\n') {
        char *normalized = xstrndup(line, length - 1);
        history_add_normalized(shell, normalized);
        free(normalized);
    } else {
        history_add_normalized(shell, line);
    }
}

static int history_secure_fd(int fd)
{
    struct stat metadata;
    if (fstat(fd, &metadata) < 0) {
        return -1;
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
        (metadata.st_mode & 077u)) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int history_read_records(int fd, StrVec *records)
{
    FILE *stream = fdopen(fd, "r");
    if (!stream) {
        close(fd);
        return -1;
    }
    char *error = NULL;
    char *content = read_stream(stream, MISH_MAX_SOURCE, &error);
    int close_status = fclose(stream);
    if (!content || close_status) {
        free(content);
        free(error);
        errno = EIO;
        return -1;
    }
    free(error);
    size_t signature_length = strlen(HISTORY_SIGNATURE);
    const char *cursor = content;
    if (strncmp(cursor, HISTORY_SIGNATURE, signature_length)) {
        /* Import an ordinary newline-delimited history file. */
        while (*cursor) {
            const char *end = strchr(cursor, '\n');
            size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
            if (length && cursor[length - 1] == '\r') {
                length--;
            }
            if (length) {
                strvec_push_owned(records, xstrndup(cursor, length));
            }
            if (!end) {
                break;
            }
            cursor = end + 1;
        }
        free(content);
        return 0;
    }
    cursor += signature_length;
    const char *finish = content + strlen(content);
    while (cursor < finish) {
        if (!isdigit((unsigned char)*cursor)) {
            free(content);
            errno = EINVAL;
            return -1;
        }
        errno = 0;
        char *number_end;
        unsigned long length = strtoul(cursor, &number_end, 10);
        if (errno || *number_end != ':' || length > MISH_MAX_SOURCE) {
            free(content);
            errno = EINVAL;
            return -1;
        }
        cursor = number_end + 1;
        if ((size_t)(finish - cursor) <= length || cursor[length] != '\n') {
            free(content);
            errno = EINVAL;
            return -1;
        }
        strvec_push_owned(records, xstrndup(cursor, (size_t)length));
        cursor += length + 1;
    }
    free(content);
    return 0;
}

static int history_read_path(const char *path, StrVec *records)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (history_secure_fd(fd)) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return history_read_records(fd, records);
}

int history_load(Shell *shell, const char *path)
{
    if (!path || !*path) {
        errno = ENOENT;
        return -1;
    }
    StrVec records;
    strvec_init(&records);
    if (history_read_path(path, &records)) {
        strvec_free(&records);
        return -1;
    }
    for (size_t index = 0; index < records.len; index++) {
        history_push(&shell->history, records.items[index]);
    }
    strvec_free(&records);
    history_trim(&shell->history,
                 history_limit(shell, "HISTSIZE", HISTORY_DEFAULT_LIMIT));
    if (!shell->history.path) {
        shell->history.path = xstrdup(path);
    }
    if (!strcmp(path, shell->history.path)) {
        shell->history.saved = shell->history.len;
    }
    return 0;
}

int history_init(Shell *shell)
{
    memset(&shell->history, 0, sizeof(shell->history));
    shell->history.limit = history_limit(shell, "HISTSIZE", HISTORY_DEFAULT_LIMIT);
    const char *path = shell_get(shell, "HISTFILE");
    if (path) {
        shell->history.path = xstrdup(path);
    } else {
        const char *home = shell_get(shell, "HOME");
        shell->history.path = home && *home ? path_join(home, ".mish_history") : NULL;
    }
    if (!shell->interactive || !shell->history.path || !*shell->history.path) {
        return 0;
    }
    if (history_load(shell, shell->history.path) && errno != ENOENT) {
        fprintf(stderr, "mish: history: %s: %s\n", shell->history.path, strerror(errno));
        return -1;
    }
    return 0;
}

static int history_lock(const char *path, char **lock_path)
{
    *lock_path = str_printf("%s.lock", path);
    int fd = open(*lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (history_secure_fd(fd) || flock(fd, LOCK_EX)) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int history_write_records(int fd, const StrVec *records, size_t limit)
{
    if (write_all(fd, HISTORY_SIGNATURE, strlen(HISTORY_SIGNATURE))) {
        return -1;
    }
    size_t start = records->len > limit ? records->len - limit : 0;
    for (size_t index = start; index < records->len; index++) {
        const char *record = records->items[index];
        char *length = str_printf("%zu:", strlen(record));
        int status = write_all(fd, length, strlen(length));
        free(length);
        if (status || write_all(fd, record, strlen(record)) || write_all(fd, "\n", 1)) {
            return -1;
        }
    }
    return fsync(fd);
}

static int history_replace(const char *path, const StrVec *records, size_t limit)
{
    struct stat metadata;
    if (!lstat(path, &metadata)) {
        if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
            (metadata.st_mode & 077u)) {
            errno = EPERM;
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }
    static unsigned counter;
    char *temporary = NULL;
    int fd = -1;
    for (unsigned retry = 0; retry < 32; retry++) {
        free(temporary);
        temporary = str_printf("%s.%lu.%u.tmp", path, (unsigned long)getpid(), ++counter);
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (fd < 0) {
        free(temporary);
        return -1;
    }
    int result = history_write_records(fd, records, limit);
    int saved_errno = errno;
    if (close(fd) && !result) {
        result = -1;
        saved_errno = errno;
    }
    if (!result && rename(temporary, path)) {
        result = -1;
        saved_errno = errno;
    }
    if (result) {
        unlink(temporary);
    }
    free(temporary);
    errno = saved_errno;
    return result;
}

int history_save(Shell *shell, const char *path, bool append)
{
    if (!path || !*path) {
        errno = ENOENT;
        return -1;
    }
    char *lock_path = NULL;
    int lock = history_lock(path, &lock_path);
    free(lock_path);
    if (lock < 0) {
        return -1;
    }
    StrVec records;
    strvec_init(&records);
    if (append && history_read_path(path, &records) && errno != ENOENT) {
        int saved_errno = errno;
        strvec_free(&records);
        flock(lock, LOCK_UN);
        close(lock);
        errno = saved_errno;
        return -1;
    }
    if (!shell->history.path) {
        shell->history.path = xstrdup(path);
    }
    bool primary = !strcmp(path, shell->history.path);
    size_t start = append && primary ? shell->history.saved : 0;
    if (start > shell->history.len) {
        start = shell->history.len;
    }
    for (size_t index = start; index < shell->history.len; index++) {
        strvec_push(&records, shell->history.lines[index]);
    }
    size_t limit = history_limit(shell, "HISTFILESIZE", HISTORY_DEFAULT_LIMIT);
    int result = history_replace(path, &records, limit);
    int saved_errno = errno;
    strvec_free(&records);
    flock(lock, LOCK_UN);
    close(lock);
    if (!result && primary) {
        shell->history.saved = shell->history.len;
    }
    errno = saved_errno;
    return result;
}

static int history_delete(Shell *shell, const char *text)
{
    int64_t value;
    if (parse_number(text, &value) || !value) {
        fprintf(stderr, "history: invalid offset: %s\n", text);
        return 2;
    }
    int64_t index = value > 0 ? value - 1 : (int64_t)shell->history.len + value;
    if (index < 0 || (uint64_t)index >= shell->history.len) {
        fprintf(stderr, "history: offset out of range: %s\n", text);
        return 1;
    }
    free(shell->history.lines[index]);
    memmove(shell->history.lines + index, shell->history.lines + index + 1,
            (shell->history.len - (size_t)index - 1) * sizeof(*shell->history.lines));
    shell->history.len--;
    if (shell->history.saved > (size_t)index) {
        shell->history.saved--;
    }
    return 0;
}

int history_builtin(Shell *shell, int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--help")) {
        puts("history [N] | -c | -d OFFSET | -r|-w|-a [FILE] | -s COMMAND...");
        puts("List, clear, delete, read, replace, append, or add history entries.");
        puts("History files must be regular files owned by you with mode 0600.");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "-c")) {
        if (argc != 2) {
            fputs("history: -c takes no arguments\n", stderr);
            return 2;
        }
        history_clear(shell);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "-d")) {
        if (argc != 3) {
            fputs("history: -d requires one offset\n", stderr);
            return 2;
        }
        return history_delete(shell, argv[2]);
    }
    if (argc > 1 && !strcmp(argv[1], "-s")) {
        StrVec words;
        strvec_init(&words);
        for (int index = 2; index < argc; index++) {
            strvec_push(&words, argv[index]);
        }
        char *line = strvec_join(&words, " ");
        history_add(shell, line);
        free(line);
        strvec_free(&words);
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "-w") ||
                     !strcmp(argv[1], "-a"))) {
        if (argc > 3) {
            fputs("history: too many filenames\n", stderr);
            return 2;
        }
        const char *path = argc == 3 ? argv[2] : shell_get(shell, "HISTFILE");
        if (!path) {
            path = shell->history.path;
        }
        int result = !strcmp(argv[1], "-r") ? history_load(shell, path) :
                     history_save(shell, path, !strcmp(argv[1], "-a"));
        if (result) {
            fprintf(stderr, "history: %s: %s\n", path ? path : "HISTFILE", strerror(errno));
            return 1;
        }
        return 0;
    }
    size_t count = shell->history.len;
    if (argc > 2) {
        fputs("history: too many arguments\n", stderr);
        return 2;
    }
    if (argc == 2) {
        int64_t number;
        if (parse_number(argv[1], &number) || number < 0) {
            fprintf(stderr, "history: invalid count: %s\n", argv[1]);
            return 2;
        }
        if ((uint64_t)number < count) {
            count = (size_t)number;
        }
    }
    size_t first = shell->history.len - count;
    for (size_t index = first; index < shell->history.len; index++) {
        printf("%5zu  %s\n", index + 1, shell->history.lines[index]);
    }
    return 0;
}

static char *history_event(Shell *shell, const char *line, size_t *position,
                           const char *current, char **error)
{
    size_t index = *position;
    size_t count = shell->history.len;
    char selector = line[index];
    if (selector == '!') {
        *position = index + 1;
        if (count) {
            return xstrdup(shell->history.lines[count - 1]);
        }
    } else if (selector == '#') {
        *position = index + 1;
        return xstrdup(current);
    } else if (selector == '-' || isdigit((unsigned char)selector)) {
        bool relative = selector == '-';
        if (relative) {
            index++;
        }
        size_t start = index;
        uint64_t number = 0;
        while (isdigit((unsigned char)line[index])) {
            unsigned digit = (unsigned)(line[index] - '0');
            if (number > (UINT64_MAX - digit) / 10) {
                *error = xstrdup("history event number overflow");
                return NULL;
            }
            number = number * 10 + digit;
            index++;
        }
        *position = index;
        if (index > start && number && number <= count) {
            size_t event = relative ? count - (size_t)number : (size_t)number - 1;
            return xstrdup(shell->history.lines[event]);
        }
    } else if (selector == '?') {
        size_t start = ++index;
        while (line[index] && line[index] != '?' && line[index] != '\n') {
            index++;
        }
        char *search = xstrndup(line + start, index - start);
        if (line[index] == '?') {
            index++;
        }
        *position = index;
        if (*search) {
            for (size_t event = count; event > 0; event--) {
                if (strstr(shell->history.lines[event - 1], search)) {
                    free(search);
                    return xstrdup(shell->history.lines[event - 1]);
                }
            }
        }
        free(search);
    } else {
        size_t start = index;
        while (line[index] && !isspace((unsigned char)line[index]) &&
               !strchr(":'\"\\;|&()<>$", line[index])) {
            index++;
        }
        char *prefix = xstrndup(line + start, index - start);
        *position = index;
        if (*prefix) {
            for (size_t event = count; event > 0; event--) {
                if (!strncmp(shell->history.lines[event - 1], prefix, strlen(prefix))) {
                    free(prefix);
                    return xstrdup(shell->history.lines[event - 1]);
                }
            }
        }
        free(prefix);
    }
    if (!*error) {
        *error = xstrdup("history event not found");
    }
    return NULL;
}

static int history_word_number(const char *line, size_t *position, size_t *number)
{
    size_t index = *position;
    if (!isdigit((unsigned char)line[index])) {
        return -1;
    }
    size_t value = 0;
    while (isdigit((unsigned char)line[index])) {
        unsigned digit = (unsigned)(line[index] - '0');
        if (value > (SIZE_MAX - digit) / 10) {
            return -1;
        }
        value = value * 10 + digit;
        index++;
    }
    *number = value;
    *position = index;
    return 0;
}

static char *history_words(const char *event, const char *line, size_t *position,
                           char **error)
{
    TokenVec tokens = {0};
    ParseError parse_error = {0};
    if (lex_source(event, &tokens, &parse_error)) {
        *error = xstrdup("cannot split history event into words");
        parse_error_free(&parse_error);
        tokens_free(&tokens);
        return NULL;
    }
    StrVec words;
    strvec_init(&words);
    for (size_t index = 0; index < tokens.len; index++) {
        if (tokens.items[index].type == TOK_WORD) {
            strvec_push(&words, tokens.items[index].text);
        }
    }
    tokens_free(&tokens);
    size_t first = 0;
    size_t last = words.len ? words.len - 1 : 0;
    size_t index = *position;
    if (line[index] == '^') {
        first = last = 1;
        index++;
    } else if (line[index] == '$') {
        first = last;
        index++;
    } else if (line[index] == '*') {
        first = 1;
        index++;
    } else if (line[index] == '-') {
        first = 0;
        index++;
        if (history_word_number(line, &index, &last)) {
            *error = xstrdup("invalid history word range");
            strvec_free(&words);
            return NULL;
        }
    } else {
        if (history_word_number(line, &index, &first)) {
            *error = xstrdup("invalid history word designator");
            strvec_free(&words);
            return NULL;
        }
        last = first;
        if (line[index] == '*') {
            last = words.len ? words.len - 1 : 0;
            index++;
        } else if (line[index] == '-') {
            index++;
            if (isdigit((unsigned char)line[index])) {
                if (history_word_number(line, &index, &last)) {
                    *error = xstrdup("invalid history word range");
                    strvec_free(&words);
                    return NULL;
                }
            } else {
                last = words.len > 1 ? words.len - 2 : 0;
            }
        }
    }
    *position = index;
    if (first > last && first == 1 && !last) {
        strvec_free(&words);
        return xstrdup("");
    }
    if (first > last || last >= words.len) {
        *error = xstrdup("history word designator out of range");
        strvec_free(&words);
        return NULL;
    }
    Buffer result;
    buffer_init(&result);
    for (size_t word = first; word <= last; word++) {
        if (word != first) {
            buffer_putc(&result, ' ');
        }
        buffer_append(&result, words.items[word]);
    }
    strvec_free(&words);
    return buffer_take(&result);
}

static char *history_substitute(const char *event, const char *old, const char *replacement,
                                bool global, char **error)
{
    if (!*old) {
        *error = xstrdup("empty history substitution pattern");
        return NULL;
    }
    const char *match = strstr(event, old);
    if (!match) {
        *error = xstrdup("history substitution failed");
        return NULL;
    }
    Buffer result;
    buffer_init(&result);
    const char *cursor = event;
    do {
        buffer_append_n(&result, cursor, (size_t)(match - cursor));
        for (const char *value = replacement; *value; value++) {
            if (*value == '\\' && value[1]) {
                buffer_putc(&result, *++value);
            } else if (*value == '&') {
                buffer_append(&result, old);
            } else {
                buffer_putc(&result, *value);
            }
        }
        cursor = match + strlen(old);
        match = global ? strstr(cursor, old) : NULL;
    } while (match);
    buffer_append(&result, cursor);
    return buffer_take(&result);
}

static char *history_delimited(const char *line, size_t *position, char delimiter)
{
    Buffer result;
    buffer_init(&result);
    size_t index = *position;
    while (line[index] && line[index] != delimiter) {
        if (line[index] == '\\' && line[index + 1] == delimiter) {
            index++;
        }
        buffer_putc(&result, line[index++]);
    }
    if (line[index] == delimiter) {
        index++;
    }
    *position = index;
    return buffer_take(&result);
}

static char *history_path_modifier(const char *event, char modifier)
{
    char *result = xstrdup(event);
    char *slash = strrchr(result, '/');
    char *base = slash ? slash + 1 : result;
    char *dot = strrchr(base, '.');
    if (modifier == 'h') {
        if (!slash) {
            free(result);
            return xstrdup(".");
        }
        if (slash == result) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
    } else if (modifier == 't') {
        char *tail = xstrdup(base);
        free(result);
        return tail;
    } else if (modifier == 'r' && dot) {
        *dot = '\0';
    } else if (modifier == 'e') {
        char *extension = xstrdup(dot ? dot + 1 : "");
        free(result);
        return extension;
    }
    return result;
}

static char *history_modifiers(Shell *shell, char *event, const char *line,
                               size_t *position, char **error)
{
    while (line[*position] == ':') {
        size_t index = *position + 1;
        char modifier = line[index++];
        char *replacement = NULL;
        if (modifier == 'p') {
            shell->history_print_only = true;
        } else if (modifier == 'q') {
            replacement = shell_quote(event);
        } else if (modifier && strchr("htre", modifier)) {
            replacement = history_path_modifier(event, modifier);
        } else if (modifier == 's' || (modifier == 'g' && line[index] == 's')) {
            bool global = modifier == 'g';
            if (global) {
                index++;
            }
            char delimiter = line[index++];
            if (!delimiter || isspace((unsigned char)delimiter)) {
                *error = xstrdup("invalid history substitution delimiter");
                free(event);
                return NULL;
            }
            char *old = history_delimited(line, &index, delimiter);
            char *value = history_delimited(line, &index, delimiter);
            replacement = history_substitute(event, old, value, global, error);
            free(old);
            free(value);
            if (!replacement) {
                free(event);
                return NULL;
            }
        } else if (modifier == '^' || modifier == '$' || modifier == '*' ||
                   modifier == '-' || isdigit((unsigned char)modifier)) {
            index--;
            replacement = history_words(event, line, &index, error);
            if (!replacement) {
                free(event);
                return NULL;
            }
        } else {
            *error = str_printf("unknown history modifier: %c", modifier ? modifier : '?');
            free(event);
            return NULL;
        }
        if (replacement) {
            free(event);
            event = replacement;
        }
        *position = index;
    }
    return event;
}

char *history_expand(Shell *shell, const char *line, char **error)
{
    *error = NULL;
    shell->history_print_only = false;
    if (!shell->history_enabled) {
        return xstrdup(line);
    }
    if (line[0] == '^') {
        if (!shell->history.len) {
            *error = xstrdup("history is empty");
            return NULL;
        }
        size_t position = 1;
        char *old = history_delimited(line, &position, '^');
        char *value = history_delimited(line, &position, '^');
        char *result = history_substitute(shell->history.lines[shell->history.len - 1],
                                          old, value, false, error);
        free(old);
        free(value);
        return result;
    }
    Buffer result;
    buffer_init(&result);
    bool single = false;
    bool double_quote = false;
    for (size_t position = 0; line[position];) {
        char current = line[position];
        if (current == '\\' && line[position + 1]) {
            buffer_putc(&result, current);
            buffer_putc(&result, line[position + 1]);
            position += 2;
            continue;
        }
        if (current == '\'' && !double_quote) {
            single = !single;
            buffer_putc(&result, current);
            position++;
            continue;
        }
        if (current == '"' && !single) {
            double_quote = !double_quote;
        }
        if (current != '!' || single || !line[position + 1] ||
            isspace((unsigned char)line[position + 1]) ||
            strchr("=\"'", line[position + 1])) {
            buffer_putc(&result, current);
            position++;
            continue;
        }
        position++;
        char *event;
        if (strchr("^$*", line[position])) {
            if (!shell->history.len) {
                *error = xstrdup("history is empty");
                buffer_free(&result);
                return NULL;
            }
            event = history_words(shell->history.lines[shell->history.len - 1],
                                  line, &position, error);
        } else {
            event = history_event(shell, line, &position,
                                  result.data ? result.data : "", error);
        }
        if (!event) {
            buffer_free(&result);
            return NULL;
        }
        event = history_modifiers(shell, event, line, &position, error);
        if (!event) {
            buffer_free(&result);
            return NULL;
        }
        buffer_append(&result, event);
        free(event);
        if (result.len > MISH_MAX_SOURCE) {
            *error = xstrdup("history expansion exceeds source limit");
            buffer_free(&result);
            return NULL;
        }
    }
    return buffer_take(&result);
}
