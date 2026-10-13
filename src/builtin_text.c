#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static FILE *text_input(const char *name)
{
    if (name == NULL || strcmp(name, "-") == 0) return stdin;
    FILE *stream = fopen(name, "r");
    if (stream == NULL) fprintf(stderr, "%s: %s\n", name, strerror(errno));
    return stream;
}

static int text_finish(FILE *stream, const char *name)
{
    int status = 0;
    if (ferror(stream)) {
        fprintf(stderr, "%s: read error: %s\n", name == NULL ? "stdin" : name, strerror(errno));
        status = 1;
    }
    if (stream != stdin && fclose(stream) != 0) status = 1;
    return status;
}

static int text_usage(const char *name, const char *usage)
{
    fprintf(stderr, "%s: usage: %s\n", name, usage);
    return 2;
}

static int cat_command(int argc, char **argv)
{
    bool number = false;
    bool nonblank = false;
    bool squeeze = false;
    bool ends = false;
    bool tabs = false;
    bool visible = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            switch (*p) {
            case 'n': number = true; break;
            case 'b': number = true; nonblank = true; break;
            case 's': squeeze = true; break;
            case 'E': ends = true; break;
            case 'T': tabs = true; break;
            case 'v': visible = true; break;
            case 'A': ends = tabs = visible = true; break;
            case 'e': ends = visible = true; break;
            case 't': tabs = visible = true; break;
            case 'u': break;
            default: return text_usage("cat", "cat [-nbsETv] [FILE ...]");
            }
        }
        ++index;
    }
    unsigned long long line = 1;
    bool line_start = true;
    bool blank = false;
    int status = 0;
    int count = index == argc ? 1 : argc - index;
    for (int file = 0; file < count; ++file) {
        const char *name = index == argc ? NULL : argv[index + file];
        FILE *stream = text_input(name);
        if (stream == NULL) { status = 1; continue; }
        int ch;
        while ((ch = fgetc(stream)) != EOF) {
            bool empty_line = line_start && ch == '\n';
            if (squeeze && empty_line && blank) continue;
            if (number && line_start && (!nonblank || !empty_line)) printf("%6llu\t", line++);
            if (ch == '\n') {
                if (ends) putchar('$');
                putchar('\n');
                line_start = true;
                blank = empty_line;
                continue;
            }
            line_start = false;
            blank = false;
            if (ch == '\t' && tabs) fputs("^I", stdout);
            else if (visible && ch < 32 && ch != '\t') { putchar('^'); putchar(ch + 64); }
            else if (visible && ch == 127) fputs("^?", stdout);
            else if (visible && ch >= 128) {
                fputs("M-", stdout);
                ch -= 128;
                if (ch < 32) { putchar('^'); putchar(ch + 64); }
                else if (ch == 127) fputs("^?", stdout);
                else putchar(ch);
            } else putchar(ch);
        }
        if (text_finish(stream, name) != 0) status = 1;
    }
    return ferror(stdout) ? 1 : status;
}

static int head_tail_command(int argc, char **argv, bool tail)
{
    int64_t count = 10;
    bool bytes = false;
    bool from_start = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        if ((strcmp(argv[index], "-n") == 0 || strcmp(argv[index], "-c") == 0) && index + 1 < argc) {
            bytes = argv[index][1] == 'c';
            const char *number = argv[++index];
            from_start = number[0] == '+';
            if (parse_number(number, &count) != 0 || count < 0 || count > MISH_MAX_SOURCE) return text_usage(argv[0], "head|tail [-n COUNT|-c COUNT] [FILE ...]");
        } else if (argv[index][1] >= '0' && argv[index][1] <= '9') {
            if (parse_number(argv[index] + 1, &count) != 0 || count < 0 || count > MISH_MAX_SOURCE) return text_usage(argv[0], "head|tail [-n COUNT|-c COUNT] [FILE ...]");
        } else return text_usage(argv[0], "head|tail [-n COUNT|-c COUNT] [FILE ...]");
        ++index;
    }
    int status = 0;
    int files = index == argc ? 1 : argc - index;
    for (int file = 0; file < files; ++file) {
        const char *name = index == argc ? NULL : argv[index + file];
        FILE *stream = text_input(name);
        if (stream == NULL) { status = 1; continue; }
        if (files > 1) printf("%s==> %s <==\n", file == 0 ? "" : "\n", name == NULL ? "standard input" : name);
        if (!tail || from_start) {
            int64_t position = 0;
            int ch;
            while ((tail || position < count) && (ch = fgetc(stream)) != EOF) {
                if (tail) {
                    if (position >= (count > 0 ? count - 1 : 0)) putchar(ch);
                } else if (position < count) {
                    putchar(ch);
                } else {
                    break;
                }
                if (bytes || ch == '\n') ++position;
            }
        } else if (bytes) {
            size_t capacity = (size_t)count;
            unsigned char *ring = xmalloc(capacity == 0 ? 1 : capacity);
            size_t used = 0;
            size_t next = 0;
            int ch;
            while ((ch = fgetc(stream)) != EOF) {
                if (capacity == 0) continue;
                ring[next] = (unsigned char)ch;
                next = (next + 1) % capacity;
                if (used < capacity) ++used;
            }
            size_t begin = used == capacity ? next : 0;
            for (size_t i = 0; i < used; ++i) putchar(ring[(begin + i) % capacity]);
            free(ring);
        } else {
            size_t capacity = (size_t)count;
            char **ring = xcalloc(capacity == 0 ? 1 : capacity, sizeof(char *));
            size_t *lengths = xcalloc(capacity == 0 ? 1 : capacity, sizeof(size_t));
            size_t used = 0;
            size_t next = 0;
            size_t retained = 0;
            char *line = NULL;
            size_t size = 0;
            ssize_t length;
            while ((length = getline(&line, &size, stream)) >= 0) {
                if ((size_t)length > MISH_MAX_SOURCE) { fputs("tail: input line too large\n", stderr); status = 1; break; }
                if (capacity == 0) continue;
                retained -= lengths[next];
                if (retained + (size_t)length > MISH_MAX_SOURCE) { fputs("tail: retained input exceeds limit\n", stderr); status = 1; break; }
                free(ring[next]);
                ring[next] = xstrndup(line, (size_t)length);
                lengths[next] = (size_t)length;
                retained += (size_t)length;
                next = (next + 1) % capacity;
                if (used < capacity) ++used;
            }
            free(line);
            size_t begin = used == capacity ? next : 0;
            for (size_t i = 0; i < used; ++i) fwrite(ring[(begin + i) % capacity], 1, lengths[(begin + i) % capacity], stdout);
            for (size_t i = 0; i < capacity; ++i) free(ring[i]);
            free(ring);
            free(lengths);
        }
        if (text_finish(stream, name) != 0) status = 1;
    }
    return ferror(stdout) ? 1 : status;
}

typedef struct {
    unsigned long long lines;
    unsigned long long words;
    unsigned long long bytes;
    unsigned long long characters;
    unsigned long long longest;
} Counts;

static void print_counts(const Counts *count, const char *name, bool lines, bool words,
                         bool bytes, bool characters, bool longest)
{
    bool separator = false;
    unsigned long long values[] = { count->lines, count->words, count->bytes, count->characters, count->longest };
    bool selected[] = { lines, words, bytes, characters, longest };
    for (size_t i = 0; i < 5; ++i) {
        if (!selected[i]) continue;
        printf("%s%llu", separator ? " " : "", values[i]);
        separator = true;
    }
    if (name != NULL) printf(" %s", name);
    putchar('\n');
}

static int wc_command(int argc, char **argv)
{
    bool lines = false;
    bool words = false;
    bool bytes = false;
    bool characters = false;
    bool longest = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            if (*p == 'l') lines = true;
            else if (*p == 'w') words = true;
            else if (*p == 'c') bytes = true;
            else if (*p == 'm') characters = true;
            else if (*p == 'L') longest = true;
            else return text_usage("wc", "wc [-lwcmL] [FILE ...]");
        }
        ++index;
    }
    if (!lines && !words && !bytes && !characters && !longest) lines = words = bytes = true;
    int files = index == argc ? 1 : argc - index;
    int status = 0;
    Counts total = {0};
    for (int file = 0; file < files; ++file) {
        const char *name = index == argc ? NULL : argv[index + file];
        FILE *stream = text_input(name);
        if (stream == NULL) { status = 1; continue; }
        Counts current = {0};
        bool in_word = false;
        unsigned long long column = 0;
        int ch;
        while ((ch = fgetc(stream)) != EOF) {
            ++current.bytes;
            if ((ch & 0xc0) != 0x80) ++current.characters;
            if (ch == '\n') {
                ++current.lines;
                if (column > current.longest) current.longest = column;
                column = 0;
            } else if (ch == '\t') column += 8 - column % 8;
            else if (ch == '\r' || ch == '\f') {
                if (column > current.longest) current.longest = column;
                column = 0;
            } else if ((ch & 0xc0) != 0x80) ++column;
            if (isspace((unsigned char)ch)) in_word = false;
            else if (!in_word) { ++current.words; in_word = true; }
        }
        if (column > current.longest) current.longest = column;
        total.lines += current.lines;
        total.words += current.words;
        total.bytes += current.bytes;
        total.characters += current.characters;
        if (current.longest > total.longest) total.longest = current.longest;
        print_counts(&current, name, lines, words, bytes, characters, longest);
        if (text_finish(stream, name) != 0) status = 1;
    }
    if (files > 1) print_counts(&total, "total", lines, words, bytes, characters, longest);
    return ferror(stdout) ? 1 : status;
}

typedef struct {
    bool numeric;
    bool reverse;
    bool fold;
} SortOptions;

static int compare_lines(const char *left, const char *right, const SortOptions *options)
{
    int comparison = 0;
    if (options->numeric) {
        double a = strtod(left, NULL);
        double b = strtod(right, NULL);
        comparison = a < b ? -1 : a > b ? 1 : 0;
    } else if (options->fold) {
        while (*left != '\0' && *right != '\0') {
            comparison = tolower((unsigned char)*left) - tolower((unsigned char)*right);
            if (comparison != 0) break;
            ++left;
            ++right;
        }
        if (comparison == 0) comparison = (unsigned char)*left - (unsigned char)*right;
    } else {
        comparison = strcmp(left, right);
    }
    return options->reverse ? -comparison : comparison;
}

static void merge_lines(char **items, char **temporary, size_t begin, size_t end,
                        const SortOptions *options)
{
    if (end - begin < 2) return;
    size_t middle = begin + (end - begin) / 2;
    merge_lines(items, temporary, begin, middle, options);
    merge_lines(items, temporary, middle, end, options);
    size_t left = begin;
    size_t right = middle;
    size_t next = begin;
    while (left < middle && right < end) {
        if (compare_lines(items[left], items[right], options) <= 0) temporary[next++] = items[left++];
        else temporary[next++] = items[right++];
    }
    while (left < middle) temporary[next++] = items[left++];
    while (right < end) temporary[next++] = items[right++];
    for (size_t i = begin; i < end; ++i) items[i] = temporary[i];
}

static int sort_command(int argc, char **argv)
{
    SortOptions options = {0};
    bool unique = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            if (*p == 'n') options.numeric = true;
            else if (*p == 'r') options.reverse = true;
            else if (*p == 'f') options.fold = true;
            else if (*p == 'u') unique = true;
            else if (*p != 's') return text_usage("sort", "sort [-nrufs] [FILE ...]");
        }
        ++index;
    }
    StrVec records;
    strvec_init(&records);
    size_t total = 0;
    int status = 0;
    int files = index == argc ? 1 : argc - index;
    for (int file = 0; file < files; ++file) {
        const char *name = index == argc ? NULL : argv[index + file];
        FILE *stream = text_input(name);
        if (stream == NULL) { status = 1; continue; }
        char *line = NULL;
        size_t capacity = 0;
        ssize_t length;
        while ((length = getline(&line, &capacity, stream)) >= 0) {
            if ((size_t)length > MISH_MAX_SOURCE - total) { fputs("sort: input exceeds memory limit\n", stderr); status = 1; break; }
            total += (size_t)length;
            if (length > 0 && line[length - 1] == '\n') line[--length] = '\0';
            if (memchr(line, '\0', (size_t)length) != NULL) { fputs("sort: binary input is not supported\n", stderr); status = 1; break; }
            strvec_push(&records, line);
        }
        free(line);
        if (text_finish(stream, name) != 0) status = 1;
    }
    char **temporary = xmalloc((records.len == 0 ? 1 : records.len) * sizeof(char *));
    merge_lines(records.items, temporary, 0, records.len, &options);
    free(temporary);
    for (size_t i = 0; i < records.len; ++i) {
        if (unique && i > 0 && compare_lines(records.items[i - 1], records.items[i], &options) == 0) continue;
        puts(records.items[i]);
    }
    strvec_free(&records);
    return ferror(stdout) ? 1 : status;
}

static void uniq_record(FILE *output, const char *line, size_t count, bool counts,
                        bool duplicates, bool unique)
{
    if (duplicates && count < 2) return;
    if (unique && count > 1) return;
    if (counts) fprintf(output, "%7zu ", count);
    fputs(line, output);
}

static int uniq_command(int argc, char **argv)
{
    bool counts = false;
    bool duplicates = false;
    bool unique = false;
    bool fold = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            if (*p == 'c') counts = true;
            else if (*p == 'd') duplicates = true;
            else if (*p == 'u') unique = true;
            else if (*p == 'i') fold = true;
            else return text_usage("uniq", "uniq [-cdui] [INPUT [OUTPUT]]");
        }
        ++index;
    }
    if (argc - index > 2) return text_usage("uniq", "uniq [-cdui] [INPUT [OUTPUT]]");
    FILE *input = text_input(index == argc ? NULL : argv[index]);
    if (input == NULL) return 1;
    FILE *output = index + 1 < argc ? fopen(argv[index + 1], "w") : stdout;
    if (output == NULL) { text_finish(input, index == argc ? NULL : argv[index]); fprintf(stderr, "uniq: %s\n", strerror(errno)); return 1; }
    char *previous = NULL;
    char *line = NULL;
    size_t capacity = 0;
    size_t count = 0;
    ssize_t length;
    int status = 0;
    SortOptions comparison = { .fold = fold };
    while ((length = getline(&line, &capacity, input)) >= 0) {
        if ((size_t)length > MISH_MAX_SOURCE || memchr(line, '\0', (size_t)length) != NULL) { fputs("uniq: invalid or oversized input line\n", stderr); status = 1; break; }
        if (previous != NULL && compare_lines(previous, line, &comparison) == 0) ++count;
        else {
            if (previous != NULL) uniq_record(output, previous, count, counts, duplicates, unique);
            free(previous);
            previous = xstrdup(line);
            count = 1;
        }
    }
    if (previous != NULL) uniq_record(output, previous, count, counts, duplicates, unique);
    free(previous);
    free(line);
    if (text_finish(input, index == argc ? NULL : argv[index]) != 0) status = 1;
    if (ferror(output)) status = 1;
    if (output != stdout && fclose(output) != 0) status = 1;
    return status;
}

typedef struct {
    size_t first;
    size_t last;
} Range;

static int parse_ranges(const char *list, Range **ranges, size_t *count)
{
    char *copy = xstrdup(list);
    char *cursor = copy;
    size_t used = 0;
    Range *result = NULL;
    bool valid = true;
    do {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) *comma = '\0';
        char *dash = strchr(cursor, '-');
        Range range = { .first = 1, .last = SIZE_MAX };
        if (dash != NULL) *dash++ = '\0';
        int64_t number;
        if (cursor[0] != '\0') {
            if (parse_number(cursor, &number) != 0 || number < 1) { valid = false; break; }
            range.first = (size_t)number;
        } else if (dash == NULL) { valid = false; break; }
        if (dash == NULL) range.last = range.first;
        else if (dash[0] != '\0') {
            if (parse_number(dash, &number) != 0 || number < 1) { valid = false; break; }
            range.last = (size_t)number;
        }
        if (range.last < range.first) { valid = false; break; }
        result = xrealloc(result, (used + 1) * sizeof(Range));
        result[used++] = range;
        cursor = comma == NULL ? NULL : comma + 1;
    } while (cursor != NULL);
    free(copy);
    if (!valid) { free(result); return 2; }
    *ranges = result;
    *count = used;
    return 0;
}

static bool range_contains(const Range *ranges, size_t count, size_t index)
{
    for (size_t i = 0; i < count; ++i) {
        if (index >= ranges[i].first && index <= ranges[i].last) return true;
    }
    return false;
}

static int cut_command(int argc, char **argv)
{
    bool fields = false;
    bool suppress = false;
    bool mode = false;
    unsigned char delimiter = '\t';
    const char *list = NULL;
    int index = 1;
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        if ((strcmp(argv[index], "-f") == 0 || strcmp(argv[index], "-c") == 0 || strcmp(argv[index], "-b") == 0) && index + 1 < argc) {
            if (mode) return text_usage("cut", "cut [-s] [-d DELIMITER] -f|-c|-b LIST [FILE ...]");
            fields = argv[index][1] == 'f';
            mode = true;
            list = argv[++index];
        } else if (strcmp(argv[index], "-d") == 0 && index + 1 < argc) {
            const char *value = argv[++index];
            if (value[0] == '\0' || value[1] != '\0') return text_usage("cut", "cut delimiter must be one byte");
            delimiter = (unsigned char)value[0];
        } else if (strcmp(argv[index], "-s") == 0) suppress = true;
        else return text_usage("cut", "cut [-s] [-d DELIMITER] -f|-c|-b LIST [FILE ...]");
        ++index;
    }
    if (!mode) return text_usage("cut", "cut requires a field or byte list");
    Range *ranges = NULL;
    size_t range_count = 0;
    if (parse_ranges(list, &ranges, &range_count) != 0) return text_usage("cut", "invalid list");
    int status = 0;
    int files = index == argc ? 1 : argc - index;
    for (int file = 0; file < files; ++file) {
        const char *name = index == argc ? NULL : argv[index + file];
        FILE *input = text_input(name);
        if (input == NULL) { status = 1; continue; }
        char *line = NULL;
        size_t capacity = 0;
        ssize_t length;
        while ((length = getline(&line, &capacity, input)) >= 0) {
            if ((size_t)length > MISH_MAX_SOURCE) { fputs("cut: input line too large\n", stderr); status = 1; break; }
            bool newline = length > 0 && line[length - 1] == '\n';
            size_t size = (size_t)length - (newline ? 1 : 0);
            if (!fields) {
                for (size_t i = 0; i < size; ++i) if (range_contains(ranges, range_count, i + 1)) putchar((unsigned char)line[i]);
            } else if (memchr(line, delimiter, size) == NULL) {
                if (suppress) continue;
                fwrite(line, 1, size, stdout);
            } else {
                size_t field = 1;
                size_t begin = 0;
                bool printed = false;
                for (size_t i = 0; i <= size; ++i) {
                    if (i != size && (unsigned char)line[i] != delimiter) continue;
                    if (range_contains(ranges, range_count, field)) {
                        if (printed) putchar(delimiter);
                        fwrite(line + begin, 1, i - begin, stdout);
                        printed = true;
                    }
                    begin = i + 1;
                    ++field;
                }
            }
            if (newline) putchar('\n');
        }
        free(line);
        if (text_finish(input, name) != 0) status = 1;
    }
    free(ranges);
    return ferror(stdout) ? 1 : status;
}

static int set_byte(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    unsigned value = *p++;
    if (value == '\\' && *p != '\0') {
        unsigned escaped = *p++;
        if (escaped >= '0' && escaped <= '7') {
            value = escaped - '0';
            for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; ++i) value = value * 8 + *p++ - '0';
        } else {
            switch (escaped) {
            case 'a': value = '\a'; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            case 'v': value = '\v'; break;
            default: value = escaped; break;
            }
        }
    }
    *cursor = (const char *)p;
    return (int)(value & 255);
}

static bool class_member(const char *name, unsigned char byte)
{
    if (strcmp(name, "alnum") == 0) return isalnum(byte) != 0;
    if (strcmp(name, "alpha") == 0) return isalpha(byte) != 0;
    if (strcmp(name, "blank") == 0) return byte == ' ' || byte == '\t';
    if (strcmp(name, "cntrl") == 0) return iscntrl(byte) != 0;
    if (strcmp(name, "digit") == 0) return isdigit(byte) != 0;
    if (strcmp(name, "graph") == 0) return isgraph(byte) != 0;
    if (strcmp(name, "lower") == 0) return islower(byte) != 0;
    if (strcmp(name, "print") == 0) return isprint(byte) != 0;
    if (strcmp(name, "punct") == 0) return ispunct(byte) != 0;
    if (strcmp(name, "space") == 0) return isspace(byte) != 0;
    if (strcmp(name, "upper") == 0) return isupper(byte) != 0;
    if (strcmp(name, "xdigit") == 0) return isxdigit(byte) != 0;
    return false;
}

static int parse_set(const char *text, Buffer *bytes)
{
    buffer_init(bytes);
    const char *cursor = text;
    while (*cursor != '\0') {
        if (strncmp(cursor, "[:", 2) == 0) {
            const char *end = strstr(cursor + 2, ":]");
            if (end == NULL) { buffer_free(bytes); return 2; }
            char *name = xstrndup(cursor + 2, (size_t)(end - cursor - 2));
            bool any = false;
            for (unsigned i = 0; i < 256; ++i) {
                if (class_member(name, (unsigned char)i)) { buffer_putc(bytes, (char)i); any = true; }
            }
            free(name);
            if (!any) { buffer_free(bytes); return 2; }
            cursor = end + 2;
            continue;
        }
        int first = set_byte(&cursor);
        if (*cursor == '-' && cursor[1] != '\0') {
            ++cursor;
            int last = set_byte(&cursor);
            if (last < first) { buffer_free(bytes); return 2; }
            for (int i = first; i <= last; ++i) buffer_putc(bytes, (char)i);
        } else buffer_putc(bytes, (char)first);
        if (bytes->len > 65536) { buffer_free(bytes); return 2; }
    }
    return 0;
}

static int tr_command(int argc, char **argv)
{
    bool deletion = false;
    bool squeeze = false;
    bool complement = false;
    int index = 1;
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        for (const char *p = argv[index] + 1; *p != '\0'; ++p) {
            if (*p == 'd') deletion = true;
            else if (*p == 's') squeeze = true;
            else if (*p == 'c' || *p == 'C') complement = true;
            else return text_usage("tr", "tr [-dsc] SET1 [SET2]");
        }
        ++index;
    }
    if (argc - index < 1 || argc - index > 2 || (!deletion && !squeeze && argc - index != 2)) return text_usage("tr", "tr [-dsc] SET1 [SET2]");
    Buffer first;
    Buffer second;
    if (parse_set(argv[index], &first) != 0) return text_usage("tr", "invalid first set");
    buffer_init(&second);
    if (index + 1 < argc && parse_set(argv[index + 1], &second) != 0) { buffer_free(&first); return text_usage("tr", "invalid second set"); }
    bool membership[256] = {false};
    bool repeated[256] = {false};
    unsigned char mapping[256];
    for (unsigned i = 0; i < 256; ++i) mapping[i] = (unsigned char)i;
    for (size_t i = 0; i < first.len; ++i) membership[(unsigned char)first.data[i]] = true;
    if (complement) {
        buffer_clear(&first);
        for (unsigned i = 0; i < 256; ++i) {
            membership[i] = !membership[i];
            if (membership[i]) buffer_putc(&first, (char)i);
        }
    }
    bool translation = !deletion && index + 1 < argc;
    if (translation && second.len == 0) { buffer_free(&first); buffer_free(&second); return text_usage("tr", "second set is empty"); }
    if (translation) {
        for (size_t i = 0; i < first.len; ++i) mapping[(unsigned char)first.data[i]] = (unsigned char)second.data[i < second.len ? i : second.len - 1];
    }
    const Buffer *squeezed = second.len > 0 ? &second : &first;
    for (size_t i = 0; i < squeezed->len; ++i) repeated[(unsigned char)squeezed->data[i]] = true;
    int previous = -1;
    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
        if (deletion && membership[ch]) continue;
        int output = mapping[ch];
        if (squeeze && output == previous && repeated[output]) continue;
        putchar(output);
        previous = output;
    }
    buffer_free(&first);
    buffer_free(&second);
    return ferror(stdin) || ferror(stdout) ? 1 : 0;
}

static int seq_command(int argc, char **argv)
{
    bool equal = false;
    const char *separator = "\n";
    int index = 1;
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0' && !isdigit((unsigned char)argv[index][1])) {
        if (strcmp(argv[index], "--") == 0) { ++index; break; }
        if (strcmp(argv[index], "-w") == 0) equal = true;
        else if (strcmp(argv[index], "-s") == 0 && index + 1 < argc) separator = argv[++index];
        else return text_usage("seq", "seq [-w] [-s SEPARATOR] [FIRST [INCREMENT]] LAST");
        ++index;
    }
    if (argc - index < 1 || argc - index > 3) return text_usage("seq", "seq [FIRST [INCREMENT]] LAST");
    double values[3] = { 1, 1, 0 };
    for (int i = index; i < argc; ++i) {
        char *end;
        errno = 0;
        double value = strtod(argv[i], &end);
        if (end == argv[i] || *end != '\0' || errno != 0 || !isfinite(value)) return text_usage("seq", "invalid numeric operand");
        int position = argc - index == 1 ? 2 : argc - index == 2 ? (i == index ? 0 : 2) : i - index;
        values[position] = value;
    }
    if (values[1] == 0) return text_usage("seq", "increment must be nonzero");
    int width = 0;
    if (equal) {
        char first[128];
        char last[128];
        snprintf(first, sizeof(first), "%.12g", values[0]);
        snprintf(last, sizeof(last), "%.12g", values[2]);
        width = (int)(strlen(first) > strlen(last) ? strlen(first) : strlen(last));
    }
    bool printed = false;
    for (unsigned long long i = 0;; ++i) {
        double value = values[0] + (double)i * values[1];
        double tolerance = fabs(values[1]) * 1e-12;
        if ((values[1] > 0 && value > values[2] + tolerance) || (values[1] < 0 && value < values[2] - tolerance)) break;
        if (i >= 10000000) { fputs("seq: sequence exceeds ten million items\n", stderr); return 1; }
        if (printed) fputs(separator, stdout);
        if (equal) printf("%0*.12g", width, value);
        else printf("%.12g", value);
        printed = true;
        if (ferror(stdout)) return 1;
    }
    if (printed) putchar('\n');
    return ferror(stdout) ? 1 : 0;
}

static int expr_command(Shell *shell, int argc, char **argv)
{
    if (argc < 2) return text_usage("expr", "expr INTEGER_EXPRESSION ...");
    StrVec words;
    strvec_init(&words);
    for (int i = 1; i < argc; ++i) strvec_push(&words, argv[i]);
    char *expression = strvec_join(&words, " ");
    strvec_free(&words);
    char *error = NULL;
    int64_t result;
    int status = arithmetic_eval(shell, expression, &result, &error);
    free(expression);
    if (status != 0) {
        fprintf(stderr, "expr: %s\n", error == NULL ? "invalid integer expression" : error);
        free(error);
        return 2;
    }
    printf("%lld\n", (long long)result);
    return result == 0 ? 1 : 0;
}

int builtin_text_run(Shell *shell, int argc, char **argv)
{
    const char *name = argv[0];
    if (strcmp(name, "cat") == 0) return cat_command(argc, argv);
    if (strcmp(name, "head") == 0) return head_tail_command(argc, argv, false);
    if (strcmp(name, "tail") == 0) return head_tail_command(argc, argv, true);
    if (strcmp(name, "wc") == 0) return wc_command(argc, argv);
    if (strcmp(name, "sort") == 0) return sort_command(argc, argv);
    if (strcmp(name, "uniq") == 0) return uniq_command(argc, argv);
    if (strcmp(name, "cut") == 0) return cut_command(argc, argv);
    if (strcmp(name, "tr") == 0) return tr_command(argc, argv);
    if (strcmp(name, "seq") == 0) return seq_command(argc, argv);
    if (strcmp(name, "expr") == 0) return expr_command(shell, argc, argv);
    return -1;
}
