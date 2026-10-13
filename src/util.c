#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void allocation_failure(void)
{
    static const char message[] = "mish: memory allocation failed\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(125);
}

void *xmalloc(size_t size)
{
    void *pointer = malloc(size ? size : 1);
    if (!pointer)
        allocation_failure();
    return pointer;
}

void *xcalloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size)
        allocation_failure();
    void *pointer = calloc(count ? count : 1, size ? size : 1);
    if (!pointer)
        allocation_failure();
    return pointer;
}

void *xrealloc(void *pointer, size_t size)
{
    void *result = realloc(pointer, size ? size : 1);
    if (!result)
        allocation_failure();
    return result;
}

char *xstrdup(const char *text)
{
    if (!text)
        text = "";
    return xstrndup(text, strlen(text));
}

char *xstrndup(const char *text, size_t length)
{
    if (length == SIZE_MAX)
        allocation_failure();
    char *result = xmalloc(length + 1);
    memcpy(result, text, length);
    result[length] = '\0';
    return result;
}

char *str_printf(const char *format, ...)
{
    va_list arguments;
    va_list copy;
    va_start(arguments, format);
    va_copy(copy, arguments);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(arguments);
        return xstrdup("");
    }
    char *text = xmalloc((size_t)length + 1);
    vsnprintf(text, (size_t)length + 1, format, arguments);
    va_end(arguments);
    return text;
}

void buffer_init(Buffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
}

static void buffer_reserve(Buffer *buffer, size_t extra)
{
    if (extra >= SIZE_MAX - buffer->len)
        allocation_failure();
    size_t needed = buffer->len + extra + 1;
    if (needed <= buffer->cap)
        return;
    size_t capacity = buffer->cap ? buffer->cap : 64;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    buffer->data = xrealloc(buffer->data, capacity);
    buffer->cap = capacity;
}

void buffer_append_n(Buffer *buffer, const char *text, size_t length)
{
    buffer_reserve(buffer, length);
    memcpy(buffer->data + buffer->len, text, length);
    buffer->len += length;
    buffer->data[buffer->len] = '\0';
}

void buffer_append(Buffer *buffer, const char *text)
{
    buffer_append_n(buffer, text, strlen(text));
}

void buffer_putc(Buffer *buffer, char character)
{
    buffer_append_n(buffer, &character, 1);
}

void buffer_clear(Buffer *buffer)
{
    buffer->len = 0;
    if (buffer->data)
        buffer->data[0] = '\0';
}

void buffer_free(Buffer *buffer)
{
    free(buffer->data);
    buffer_init(buffer);
}

char *buffer_take(Buffer *buffer)
{
    char *result = buffer->data ? buffer->data : xstrdup("");
    buffer_init(buffer);
    return result;
}

void strvec_init(StrVec *vector)
{
    memset(vector, 0, sizeof(*vector));
}

void strvec_push_owned(StrVec *vector, char *text)
{
    if (vector->len + 1 >= vector->cap) {
        size_t capacity = vector->cap ? vector->cap * 2 : 8;
        if (capacity <= vector->len || capacity > SIZE_MAX / sizeof(char *))
            allocation_failure();
        vector->items = xrealloc(vector->items, capacity * sizeof(char *));
        vector->cap = capacity;
    }
    vector->items[vector->len++] = text;
    vector->items[vector->len] = NULL;
}

void strvec_push(StrVec *vector, const char *text)
{
    strvec_push_owned(vector, xstrdup(text));
}

void strvec_free(StrVec *vector)
{
    for (size_t i = 0; i < vector->len; ++i)
        free(vector->items[i]);
    free(vector->items);
    strvec_init(vector);
}

char **strvec_take(StrVec *vector)
{
    if (!vector->items) {
        vector->items = xcalloc(1, sizeof(char *));
    }
    char **items = vector->items;
    strvec_init(vector);
    return items;
}

char *strvec_join(const StrVec *vector, const char *separator)
{
    Buffer buffer;
    buffer_init(&buffer);
    for (size_t i = 0; i < vector->len; ++i) {
        if (i)
            buffer_append(&buffer, separator);
        buffer_append(&buffer, vector->items[i]);
    }
    return buffer_take(&buffer);
}

bool valid_name(const char *name)
{
    if (!name || !(isalpha((unsigned char)*name) || *name == '_'))
        return false;
    for (++name; *name; ++name) {
        if (!(isalnum((unsigned char)*name) || *name == '_'))
            return false;
    }
    return true;
}

char *path_join(const char *directory, const char *name)
{
    if (*name == '/' || !directory || !*directory)
        return xstrdup(name);
    return str_printf("%s%s%s", directory,
                      directory[strlen(directory) - 1] == '/' ? "" : "/", name);
}

char *read_stream(FILE *stream, size_t limit, char **error)
{
    Buffer buffer;
    buffer_init(&buffer);
    char chunk[8192];
    for (;;) {
        size_t count = fread(chunk, 1, sizeof(chunk), stream);
        if (count) {
            if (buffer.len > limit || count > limit - buffer.len) {
                if (error)
                    *error = xstrdup("input exceeds size limit");
                buffer_free(&buffer);
                return NULL;
            }
            if (memchr(chunk, '\0', count)) {
                if (error)
                    *error = xstrdup("NUL byte in shell source");
                buffer_free(&buffer);
                return NULL;
            }
            buffer_append_n(&buffer, chunk, count);
        }
        if (count < sizeof(chunk)) {
            if (ferror(stream)) {
                if (errno == EINTR) {
                    clearerr(stream);
                    continue;
                }
                if (error)
                    *error = str_printf("read: %s", strerror(errno));
                buffer_free(&buffer);
                return NULL;
            }
            break;
        }
    }
    return buffer_take(&buffer);
}

int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    while (length) {
        ssize_t count = write(fd, bytes, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        bytes += count;
        length -= (size_t)count;
    }
    return 0;
}

int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int parse_number(const char *text, int64_t *number)
{
    if (!text || !*text)
        return -1;
    errno = 0;
    char *end;
    intmax_t result = strtoimax(text, &end, 0);
    if (errno || *end || end == text || result < INT64_MIN || result > INT64_MAX)
        return -1;
    *number = (int64_t)result;
    return 0;
}

char *shell_quote(const char *text)
{
    Buffer buffer;
    buffer_init(&buffer);
    buffer_putc(&buffer, '\'');
    for (; *text; ++text) {
        if (*text == '\'')
            buffer_append(&buffer, "'\\''");
        else
            buffer_putc(&buffer, *text);
    }
    buffer_putc(&buffer, '\'');
    return buffer_take(&buffer);
}
