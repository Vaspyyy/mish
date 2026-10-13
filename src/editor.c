#define _XOPEN_SOURCE 700
#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>

#define EDITOR_UNDO_LIMIT 128u
#define EDITOR_KEY_LIMIT 64u
#define EDITOR_PASTE_LIMIT MISH_MAX_SOURCE

typedef struct EditSnapshot {
    char *text;
    size_t cursor;
    struct EditSnapshot *next;
} EditSnapshot;

typedef struct {
    Shell *shell;
    Buffer line;
    size_t cursor;
    char *kill;
    char *original;
    char *history_draft;
    size_t history_position;
    EditSnapshot *undo;
    EditSnapshot *redo;
    size_t undo_count;
    size_t redo_count;
    int input_fd;
    int output_fd;
    unsigned columns;
    unsigned rows;
    unsigned rendered_cursor_row;
    unsigned rendered_rows;
    const char *prompt;
    bool vi_insert;
    unsigned vi_count;
    char vi_operator;
    bool eof;
    bool canceled;
    bool accept;
    bool completion_listed;
} Editor;

typedef struct {
    Buffer bytes;
    unsigned row;
    unsigned column;
    unsigned columns;
    size_t *row_offsets;
    size_t row_count;
    size_t row_capacity;
} Display;

static struct termios editor_saved_modes;
static int editor_active_fd = -1;
static bool editor_cleanup_registered;

void editor_reset_terminal(void)
{
    if (editor_active_fd >= 0) {
        int saved_errno = errno;
        write_all(editor_active_fd, "\033[?2004l", 8);
        tcsetattr(editor_active_fd, TCSANOW, &editor_saved_modes);
        editor_active_fd = -1;
        errno = saved_errno;
    }
}

static bool editor_raw_mode(int fd)
{
    if (tcgetattr(fd, &editor_saved_modes)) {
        return false;
    }
    struct termios raw = editor_saved_modes;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | INLCR | IGNCR);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw)) {
        return false;
    }
    editor_active_fd = fd;
    if (!editor_cleanup_registered) {
        atexit(editor_reset_terminal);
        editor_cleanup_registered = true;
    }
    write_all(fd, "\033[?2004h", 8);
    return true;
}

static void editor_size(Editor *editor)
{
    struct winsize size;
    memset(&size, 0, sizeof(size));
    if (!ioctl(editor->output_fd, TIOCGWINSZ, &size) && size.ws_col) {
        editor->columns = size.ws_col;
        editor->rows = size.ws_row ? size.ws_row : 24;
    } else {
        editor->columns = 80;
        editor->rows = 24;
    }
    if (editor->columns < 4) {
        editor->columns = 4;
    }
}

static bool editor_continuation(unsigned char byte)
{
    return (byte & 0xc0u) == 0x80u;
}

static size_t editor_previous(const char *text, size_t cursor)
{
    if (!cursor) {
        return 0;
    }
    cursor--;
    while (cursor && editor_continuation((unsigned char)text[cursor])) {
        cursor--;
    }
    return cursor;
}

static size_t editor_next(const char *text, size_t length, size_t cursor)
{
    if (cursor >= length) {
        return length;
    }
    cursor++;
    while (cursor < length && editor_continuation((unsigned char)text[cursor])) {
        cursor++;
    }
    return cursor;
}

static bool editor_word_byte(unsigned char character)
{
    return character >= 128 || isalnum(character) || character == '_';
}

static size_t editor_backward_word(const Editor *editor, size_t cursor)
{
    while (cursor && !editor_word_byte((unsigned char)editor->line.data[editor_previous(editor->line.data, cursor)])) {
        cursor = editor_previous(editor->line.data, cursor);
    }
    while (cursor && editor_word_byte((unsigned char)editor->line.data[editor_previous(editor->line.data, cursor)])) {
        cursor = editor_previous(editor->line.data, cursor);
    }
    return cursor;
}

static size_t editor_forward_word(const Editor *editor, size_t cursor)
{
    while (cursor < editor->line.len && !editor_word_byte((unsigned char)editor->line.data[cursor])) {
        cursor = editor_next(editor->line.data, editor->line.len, cursor);
    }
    while (cursor < editor->line.len && editor_word_byte((unsigned char)editor->line.data[cursor])) {
        cursor = editor_next(editor->line.data, editor->line.len, cursor);
    }
    return cursor;
}

static void editor_snapshot_free(EditSnapshot *snapshot)
{
    while (snapshot) {
        EditSnapshot *next = snapshot->next;
        free(snapshot->text);
        free(snapshot);
        snapshot = next;
    }
}

static void editor_snapshot_push(EditSnapshot **stack, size_t *count,
                                 const char *text, size_t cursor)
{
    EditSnapshot *snapshot = xcalloc(1, sizeof(*snapshot));
    snapshot->text = xstrdup(text);
    snapshot->cursor = cursor;
    snapshot->next = *stack;
    *stack = snapshot;
    (*count)++;
    if (*count > EDITOR_UNDO_LIMIT) {
        EditSnapshot *last = *stack;
        for (size_t index = 1; index < EDITOR_UNDO_LIMIT; index++) {
            last = last->next;
        }
        editor_snapshot_free(last->next);
        last->next = NULL;
        *count = EDITOR_UNDO_LIMIT;
    }
}

static void editor_checkpoint(Editor *editor)
{
    editor_snapshot_push(&editor->undo, &editor->undo_count,
                         editor->line.data ? editor->line.data : "", editor->cursor);
    editor_snapshot_free(editor->redo);
    editor->redo = NULL;
    editor->redo_count = 0;
    editor->completion_listed = false;
}

static void editor_replace(Editor *editor, const char *text, size_t cursor)
{
    buffer_clear(&editor->line);
    buffer_append(&editor->line, text);
    editor->cursor = cursor > editor->line.len ? editor->line.len : cursor;
    while (editor->cursor && editor->cursor < editor->line.len &&
           editor_continuation((unsigned char)editor->line.data[editor->cursor])) {
        editor->cursor--;
    }
}

static void editor_undo(Editor *editor, bool redo)
{
    EditSnapshot **source = redo ? &editor->redo : &editor->undo;
    EditSnapshot **destination = redo ? &editor->undo : &editor->redo;
    size_t *source_count = redo ? &editor->redo_count : &editor->undo_count;
    size_t *destination_count = redo ? &editor->undo_count : &editor->redo_count;
    if (!*source) {
        write_all(editor->output_fd, "\a", 1);
        return;
    }
    editor_snapshot_push(destination, destination_count,
                         editor->line.data ? editor->line.data : "", editor->cursor);
    EditSnapshot *snapshot = *source;
    *source = snapshot->next;
    (*source_count)--;
    editor_replace(editor, snapshot->text, snapshot->cursor);
    free(snapshot->text);
    free(snapshot);
}

static bool editor_insert(Editor *editor, const char *text, size_t length)
{
    if (!length) {
        return true;
    }
    if (length > MISH_MAX_SOURCE - editor->line.len) {
        write_all(editor->output_fd, "\a", 1);
        return false;
    }
    size_t old_length = editor->line.len;
    buffer_append_n(&editor->line, text, length);
    memmove(editor->line.data + editor->cursor + length,
            editor->line.data + editor->cursor, old_length - editor->cursor);
    memcpy(editor->line.data + editor->cursor, text, length);
    editor->cursor += length;
    editor->line.data[editor->line.len] = '\0';
    return true;
}

static void editor_erase(Editor *editor, size_t first, size_t last, bool kill)
{
    if (last > editor->line.len) {
        last = editor->line.len;
    }
    if (first >= last) {
        return;
    }
    if (kill) {
        free(editor->kill);
        editor->kill = xstrndup(editor->line.data + first, last - first);
    }
    memmove(editor->line.data + first, editor->line.data + last,
            editor->line.len - last + 1);
    editor->line.len -= last - first;
    if (editor->cursor > last) {
        editor->cursor -= last - first;
    } else if (editor->cursor > first) {
        editor->cursor = first;
    }
}

static void display_record_row(Display *display)
{
    if (display->row_count == display->row_capacity) {
        display->row_capacity = display->row_capacity ? display->row_capacity * 2 : 16;
        display->row_offsets = xrealloc(display->row_offsets,
                                       display->row_capacity * sizeof(*display->row_offsets));
    }
    display->row_offsets[display->row_count++] = display->bytes.len;
}

static void display_newline(Display *display)
{
    buffer_append(&display->bytes, "\r\n");
    display->row++;
    display->column = 0;
    display_record_row(display);
}

static void display_advance(Display *display, unsigned cells)
{
    display->column += cells;
    while (display->column >= display->columns) {
        display->column -= display->columns;
        display->row++;
        buffer_append(&display->bytes, "\r\n");
        display_record_row(display);
    }
}

static size_t display_ansi_length(const char *text, size_t remaining)
{
    if (remaining < 2 || text[0] != '\033') {
        return 0;
    }
    if (text[1] == '[') {
        for (size_t index = 2; index < remaining; index++) {
            unsigned char byte = (unsigned char)text[index];
            if (byte >= 0x40 && byte <= 0x7e) {
                return index + 1;
            }
        }
        return remaining;
    }
    if (text[1] == ']') {
        for (size_t index = 2; index < remaining; index++) {
            if (text[index] == '\a') {
                return index + 1;
            }
            if (text[index] == '\033' && index + 1 < remaining && text[index + 1] == '\\') {
                return index + 2;
            }
        }
        return remaining;
    }
    return 2;
}

static void display_text(Display *display, const char *text, size_t length,
                         bool prompt, size_t cursor, unsigned *cursor_row,
                         unsigned *cursor_column)
{
    size_t index = 0;
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    while (index < length) {
        if (index == cursor) {
            *cursor_row = display->row;
            *cursor_column = display->column;
        }
        if (prompt && text[index] == '\033') {
            size_t escape_length = display_ansi_length(text + index, length - index);
            buffer_append_n(&display->bytes, text + index, escape_length);
            index += escape_length;
            continue;
        }
        unsigned char byte = (unsigned char)text[index];
        if (byte == '\n') {
            display_newline(display);
            index++;
            continue;
        }
        if (prompt && byte == '\r') {
            buffer_putc(&display->bytes, '\r');
            display->column = 0;
            index++;
            continue;
        }
        if (byte == '\t') {
            unsigned spaces = 4 - display->column % 4;
            for (unsigned space = 0; space < spaces; space++) {
                buffer_putc(&display->bytes, ' ');
                display_advance(display, 1);
            }
            index++;
            continue;
        }
        if (byte < 32 || byte == 127) {
            if (prompt && byte == '\a') {
                buffer_putc(&display->bytes, '\a');
            } else {
                buffer_putc(&display->bytes, '^');
                display_advance(display, 1);
                buffer_putc(&display->bytes, byte == 127 ? '?' : (char)(byte + 64));
                display_advance(display, 1);
            }
            index++;
            continue;
        }
        wchar_t wide;
        size_t count = mbrtowc(&wide, text + index, length - index, &state);
        if (count == (size_t)-1 || count == (size_t)-2 || !count) {
            memset(&state, 0, sizeof(state));
            buffer_putc(&display->bytes, '?');
            display_advance(display, 1);
            index++;
            continue;
        }
        int width = wcwidth(wide);
        if (width < 0) {
            width = 1;
        }
        if (width > 1 && display->column + (unsigned)width > display->columns) {
            display_newline(display);
        }
        buffer_append_n(&display->bytes, text + index, count);
        display_advance(display, (unsigned)width);
        index += count;
    }
    if (cursor >= length) {
        *cursor_row = display->row;
        *cursor_column = display->column;
    }
}

static void editor_redraw(Editor *editor)
{
    Display display;
    memset(&display, 0, sizeof(display));
    buffer_init(&display.bytes);
    buffer_append(&display.bytes, "");
    display.columns = editor->columns;
    display_record_row(&display);
    unsigned ignored_row = 0;
    unsigned ignored_column = 0;
    display_text(&display, editor->prompt, strlen(editor->prompt), true, SIZE_MAX,
                 &ignored_row, &ignored_column);
    unsigned cursor_row = display.row;
    unsigned cursor_column = display.column;
    display_text(&display, editor->line.data ? editor->line.data : "", editor->line.len,
                 false, editor->cursor, &cursor_row, &cursor_column);
    unsigned visible_rows = editor->rows > 2 ? editor->rows - 1 : 1;
    unsigned first_row = cursor_row >= visible_rows ? cursor_row - visible_rows + 1 : 0;
    unsigned last_row = first_row + visible_rows - 1;
    if (last_row > display.row) {
        last_row = display.row;
    }
    size_t first_byte = display.row_offsets[first_row];
    size_t final_byte = last_row + 1 < display.row_count ?
                        display.row_offsets[last_row + 1] - 2 : display.bytes.len;
    Buffer output;
    buffer_init(&output);
    buffer_putc(&output, '\r');
    if (editor->rendered_cursor_row) {
        char *up = str_printf("\033[%uA", editor->rendered_cursor_row);
        buffer_append(&output, up);
        free(up);
    }
    buffer_append(&output, "\033[J");
    if (first_row) {
        buffer_append(&output, "\033[0m");
    }
    buffer_append_n(&output, display.bytes.data + first_byte, final_byte - first_byte);
    unsigned end_row = last_row - first_row;
    cursor_row -= first_row;
    if (end_row > cursor_row) {
        char *up = str_printf("\033[%uA", end_row - cursor_row);
        buffer_append(&output, up);
        free(up);
    }
    buffer_putc(&output, '\r');
    if (cursor_column) {
        char *right = str_printf("\033[%uC", cursor_column);
        buffer_append(&output, right);
        free(right);
    }
    write_all(editor->output_fd, output.data, output.len);
    editor->rendered_rows = end_row + 1;
    editor->rendered_cursor_row = cursor_row;
    buffer_free(&output);
    free(display.row_offsets);
    buffer_free(&display.bytes);
}

static void editor_process_signals(Editor *editor)
{
    size_t cursor = editor->cursor;
    editor->cursor = editor->line.len;
    editor_redraw(editor);
    write_all(editor->output_fd, "\r\n", 2);
    editor_reset_terminal();
    shell_handle_traps(editor->shell);
    editor->cursor = cursor;
    editor->rendered_cursor_row = 0;
    editor->rendered_rows = 0;
    if (editor->shell->flow == FLOW_EXIT) {
        editor->eof = true;
        return;
    }
    if (!editor_raw_mode(editor->input_fd)) {
        editor->eof = true;
        return;
    }
    editor_size(editor);
    editor_redraw(editor);
}

static int editor_byte(Editor *editor, unsigned char *byte, int timeout)
{
    struct pollfd descriptor;
    descriptor.fd = editor->input_fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    for (;;) {
        int status = poll(&descriptor, 1, timeout);
        if (status < 0 && errno == EINTR) {
            editor_process_signals(editor);
            if (editor->eof) {
                return -1;
            }
            return 2;
        }
        if (status <= 0) {
            return status;
        }
        ssize_t length = read(editor->input_fd, byte, 1);
        if (length == 1) {
            return 1;
        }
        if (!length) {
            return -1;
        }
        if (errno == EINTR) {
            editor_process_signals(editor);
            if (editor->eof) {
                return -1;
            }
            return 2;
        }
        return -1;
    }
}

static unsigned editor_utf8_length(unsigned char byte)
{
    if (byte < 0x80) {
        return 1;
    }
    if ((byte & 0xe0u) == 0xc0u) {
        return 2;
    }
    if ((byte & 0xf0u) == 0xe0u) {
        return 3;
    }
    if ((byte & 0xf8u) == 0xf0u) {
        return 4;
    }
    return 1;
}

static int editor_key(Editor *editor, char key[EDITOR_KEY_LIMIT])
{
    unsigned char byte;
    int result = editor_byte(editor, &byte, 100);
    if (result != 1) {
        return result;
    }
    key[0] = (char)byte;
    key[1] = '\0';
    size_t length = 1;
    if (byte >= 0x80) {
        unsigned needed = editor_utf8_length(byte);
        while (length < needed) {
            result = editor_byte(editor, &byte, 100);
            if (result != 1) {
                break;
            }
            key[length++] = (char)byte;
        }
        key[length] = '\0';
        return 1;
    }
    if (byte == 27 || editor_binding_prefix(editor->shell, key)) {
        while (length + 1 < EDITOR_KEY_LIMIT) {
            bool ansi = length >= 2 && key[0] == '\033' &&
                        (key[1] == '[' || key[1] == 'O');
            bool finished = ansi && length >= 3 &&
                            (unsigned char)key[length - 1] >= 0x40 &&
                            (unsigned char)key[length - 1] <= 0x7e;
            if (finished || (length > 1 && !ansi &&
                             !editor_binding_prefix(editor->shell, key))) {
                break;
            }
            result = editor_byte(editor, &byte, 70);
            if (result != 1) {
                break;
            }
            key[length++] = (char)byte;
            key[length] = '\0';
        }
    }
    return 1;
}

static void editor_history(Editor *editor, int direction, bool endpoint)
{
    History *history = &editor->shell->history;
    if (!history->len) {
        write_all(editor->output_fd, "\a", 1);
        return;
    }
    if (editor->history_position == history->len) {
        free(editor->history_draft);
        editor->history_draft = xstrdup(editor->line.data ? editor->line.data : "");
    }
    size_t selected = editor->history_position;
    if (endpoint) {
        selected = direction < 0 ? 0 : history->len;
    } else if (direction < 0 && selected) {
        selected--;
    } else if (direction > 0 && selected < history->len) {
        selected++;
    } else {
        write_all(editor->output_fd, "\a", 1);
        return;
    }
    editor_checkpoint(editor);
    editor->history_position = selected;
    const char *line = selected == history->len ?
                       (editor->history_draft ? editor->history_draft : "") : history->lines[selected];
    editor_replace(editor, line, strlen(line));
}

static ssize_t editor_history_find(Editor *editor, const char *query,
                                   ssize_t from, int direction)
{
    History *history = &editor->shell->history;
    if (direction < 0) {
        if (from >= (ssize_t)history->len) {
            from = (ssize_t)history->len - 1;
        }
        for (ssize_t index = from; index >= 0; index--) {
            if (strstr(history->lines[index], query)) {
                return index;
            }
        }
    } else {
        if (from < 0) {
            from = 0;
        }
        for (size_t index = (size_t)from; index < history->len; index++) {
            if (strstr(history->lines[index], query)) {
                return (ssize_t)index;
            }
        }
    }
    return -1;
}

static void editor_search(Editor *editor, int direction)
{
    char *saved = xstrdup(editor->line.data ? editor->line.data : "");
    size_t saved_cursor = editor->cursor;
    const char *saved_prompt = editor->prompt;
    Buffer query;
    buffer_init(&query);
    ssize_t match = direction < 0 ? (ssize_t)editor->shell->history.len : -1;
    bool repeat = false;
    editor_checkpoint(editor);
    for (;;) {
        ssize_t from = repeat ? match + direction :
                       (direction < 0 ? (ssize_t)editor->shell->history.len - 1 : 0);
        ssize_t found = editor_history_find(editor, query.data ? query.data : "", from, direction);
        bool failed = found < 0;
        if (!failed) {
            match = found;
            const char *text = editor->shell->history.lines[match];
            editor_replace(editor, text, strlen(text));
        }
        char *prompt = str_printf("(%s%s-i-search)`%s': ", failed ? "failed " : "",
                                  direction < 0 ? "reverse" : "forward",
                                  query.data ? query.data : "");
        editor->prompt = prompt;
        editor_redraw(editor);
        char key[EDITOR_KEY_LIMIT];
        int status;
        do {
            status = editor_key(editor, key);
        } while (status == 0 || status == 2);
        free(prompt);
        editor->prompt = saved_prompt;
        if (status < 0) {
            editor->eof = true;
            break;
        }
        repeat = false;
        if (!strcmp(key, "\022") || !strcmp(key, "\023")) {
            direction = key[0] == '\022' ? -1 : 1;
            repeat = true;
        } else if (!strcmp(key, "\007")) {
            editor_replace(editor, saved, saved_cursor);
            break;
        } else if (!strcmp(key, "\003")) {
            editor->canceled = true;
            break;
        } else if (!strcmp(key, "\r") || !strcmp(key, "\n")) {
            editor->accept = true;
            break;
        } else if (!strcmp(key, "\033")) {
            break;
        } else if (!strcmp(key, "\177") || !strcmp(key, "\010")) {
            if (query.len) {
                query.len = editor_previous(query.data, query.len);
                query.data[query.len] = '\0';
            }
        } else if ((unsigned char)key[0] >= 32) {
            buffer_append(&query, key);
        } else {
            break;
        }
    }
    editor->prompt = saved_prompt;
    buffer_free(&query);
    free(saved);
}

static size_t editor_completion_start(const char *line, size_t cursor)
{
    size_t start = 0;
    char quote = '\0';
    bool escaped = false;
    for (size_t index = 0; index < cursor; index++) {
        char character = line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && quote != '\'') {
            escaped = true;
        } else if ((character == '\'' || character == '"') &&
                   (!quote || quote == character)) {
            quote = quote ? '\0' : character;
        } else if (!quote && (isspace((unsigned char)character) ||
                              strchr(";|&()<>\n", character))) {
            start = index + 1;
        }
    }
    return start;
}

static void editor_completion_list(Editor *editor, const StrVec *candidates)
{
    write_all(editor->output_fd, "\r\n", 2);
    unsigned column = 0;
    for (size_t index = 0; index < candidates->len; index++) {
        const char *candidate = candidates->items[index];
        size_t length = strlen(candidate);
        if (column && column + length + 2 >= editor->columns) {
            write_all(editor->output_fd, "\r\n", 2);
            column = 0;
        }
        if (column) {
            write_all(editor->output_fd, "  ", 2);
            column += 2;
        }
        for (const unsigned char *cursor = (const unsigned char *)candidate; *cursor; cursor++) {
            if (*cursor < 32 || *cursor == 127) {
                char safe[2] = { '^', *cursor == 127 ? '?' : (char)(*cursor + 64) };
                write_all(editor->output_fd, safe, sizeof(safe));
                column += 2;
            } else {
                write_all(editor->output_fd, cursor, 1);
                column++;
            }
        }
    }
    write_all(editor->output_fd, "\r\n", 2);
    editor->rendered_cursor_row = 0;
    editor->rendered_rows = 0;
}

static void editor_complete(Editor *editor, bool list_only)
{
    StrVec candidates;
    strvec_init(&candidates);
    completion_candidates(editor->shell, editor->line.data ? editor->line.data : "",
                          editor->cursor, &candidates);
    if (!candidates.len) {
        write_all(editor->output_fd, "\a", 1);
        strvec_free(&candidates);
        return;
    }
    size_t common = strlen(candidates.items[0]);
    for (size_t index = 1; index < candidates.len; index++) {
        size_t length = 0;
        while (length < common && candidates.items[index][length] &&
               candidates.items[index][length] == candidates.items[0][length]) {
            length++;
        }
        common = length;
    }
    while (common && editor_continuation((unsigned char)candidates.items[0][common])) {
        common--;
    }
    size_t start = editor_completion_start(editor->line.data ? editor->line.data : "", editor->cursor);
    size_t current_length = editor->cursor - start;
    bool useful = candidates.len == 1 || common > current_length;
    if (!list_only && useful) {
        editor_checkpoint(editor);
        editor_erase(editor, start, editor->cursor, false);
        editor->cursor = start;
        editor_insert(editor, candidates.items[0], common);
        if (candidates.len == 1 && common && candidates.items[0][common - 1] != '/') {
            editor_insert(editor, " ", 1);
        }
    } else if (list_only || editor->completion_listed) {
        editor_completion_list(editor, &candidates);
    } else {
        write_all(editor->output_fd, "\a", 1);
    }
    editor->completion_listed = true;
    strvec_free(&candidates);
}

static void editor_paste(Editor *editor)
{
    Buffer paste;
    buffer_init(&paste);
    static const char end[] = "\033[201~";
    size_t matched = 0;
    bool truncated = false;
    bool carriage_return = false;
    for (;;) {
        unsigned char byte;
        int status = editor_byte(editor, &byte, 1000);
        if (status < 0) {
            editor->eof = true;
            break;
        }
        if (status != 1) {
            continue;
        }
        if (byte == (unsigned char)end[matched]) {
            matched++;
            if (matched == sizeof(end) - 1) {
                break;
            }
            continue;
        }
        if (matched) {
            if (paste.len + matched <= EDITOR_PASTE_LIMIT) {
                buffer_append_n(&paste, end, matched);
            } else {
                truncated = true;
            }
            matched = 0;
            if (byte == (unsigned char)end[0]) {
                matched = 1;
                continue;
            }
        }
        if (!byte) {
            continue;
        }
        if (byte == '\n' && carriage_return) {
            carriage_return = false;
            continue;
        }
        carriage_return = byte == '\r';
        if (byte == '\r') {
            byte = '\n';
        }
        if (paste.len < EDITOR_PASTE_LIMIT) {
            buffer_putc(&paste, (char)byte);
        } else {
            truncated = true;
        }
    }
    if (paste.len) {
        editor_checkpoint(editor);
        editor_insert(editor, paste.data, paste.len);
    }
    if (truncated) {
        write_all(editor->output_fd, "\a", 1);
    }
    buffer_free(&paste);
}

static void editor_transpose_chars(Editor *editor)
{
    if (!editor->cursor || editor->line.len < 2) {
        return;
    }
    size_t right = editor->cursor == editor->line.len ?
                   editor_previous(editor->line.data, editor->cursor) : editor->cursor;
    if (!right) {
        return;
    }
    size_t left = editor_previous(editor->line.data, right);
    size_t finish = editor_next(editor->line.data, editor->line.len, right);
    char *second = xstrndup(editor->line.data + right, finish - right);
    char *first = xstrndup(editor->line.data + left, right - left);
    editor_checkpoint(editor);
    editor_erase(editor, left, finish, false);
    editor->cursor = left;
    editor_insert(editor, second, strlen(second));
    editor_insert(editor, first, strlen(first));
    free(second);
    free(first);
}

static void editor_transpose_words(Editor *editor)
{
    size_t right_start = editor_backward_word(editor, editor->cursor);
    size_t right_end = editor_forward_word(editor, right_start);
    if (!right_start) {
        return;
    }
    size_t left_start = editor_backward_word(editor, right_start);
    size_t left_end = editor_forward_word(editor, left_start);
    if (left_end > right_start || left_start == left_end || right_start == right_end) {
        return;
    }
    char *left = xstrndup(editor->line.data + left_start, left_end - left_start);
    char *middle = xstrndup(editor->line.data + left_end, right_start - left_end);
    char *right = xstrndup(editor->line.data + right_start, right_end - right_start);
    editor_checkpoint(editor);
    editor_erase(editor, left_start, right_end, false);
    editor->cursor = left_start;
    editor_insert(editor, right, strlen(right));
    editor_insert(editor, middle, strlen(middle));
    editor_insert(editor, left, strlen(left));
    free(left);
    free(middle);
    free(right);
}

static void editor_case_word(Editor *editor, const char *action)
{
    size_t start = editor->cursor;
    while (start < editor->line.len && !editor_word_byte((unsigned char)editor->line.data[start])) {
        start = editor_next(editor->line.data, editor->line.len, start);
    }
    size_t end = editor_forward_word(editor, start);
    if (start == end) {
        return;
    }
    editor_checkpoint(editor);
    bool capitalize = !strcmp(action, "capitalize-word");
    bool upper = !strcmp(action, "upcase-word");
    bool first = true;
    for (size_t index = start; index < end; index++) {
        unsigned char character = (unsigned char)editor->line.data[index];
        if (character >= 128) {
            continue;
        }
        editor->line.data[index] = (char)((upper || (capitalize && first)) ?
                                   toupper(character) : tolower(character));
        if (isalpha(character)) {
            first = false;
        }
    }
    editor->cursor = end;
}

static void editor_action(Editor *editor, const char *action, const char *key)
{
    if (!strcmp(action, "beginning-of-line")) {
        editor->cursor = 0;
    } else if (!strcmp(action, "end-of-line")) {
        editor->cursor = editor->line.len;
    } else if (!strcmp(action, "backward-char")) {
        editor->cursor = editor_previous(editor->line.data, editor->cursor);
    } else if (!strcmp(action, "forward-char")) {
        editor->cursor = editor_next(editor->line.data, editor->line.len, editor->cursor);
    } else if (!strcmp(action, "backward-word")) {
        editor->cursor = editor_backward_word(editor, editor->cursor);
    } else if (!strcmp(action, "forward-word")) {
        editor->cursor = editor_forward_word(editor, editor->cursor);
    } else if (!strcmp(action, "backward-delete-char")) {
        if (editor->cursor) {
            editor_checkpoint(editor);
            editor_erase(editor, editor_previous(editor->line.data, editor->cursor), editor->cursor, false);
        }
    } else if (!strcmp(action, "delete-char") || !strcmp(action, "delete-char-or-eof")) {
        if (!editor->line.len && !strcmp(action, "delete-char-or-eof")) {
            if (editor->shell->ignoreeof) {
                write_all(editor->output_fd, "\a", 1);
            } else {
                editor->eof = true;
            }
        } else if (editor->cursor < editor->line.len) {
            editor_checkpoint(editor);
            editor_erase(editor, editor->cursor,
                         editor_next(editor->line.data, editor->line.len, editor->cursor), false);
        }
    } else if (!strcmp(action, "kill-line")) {
        editor_checkpoint(editor);
        editor_erase(editor, editor->cursor, editor->line.len, true);
    } else if (!strcmp(action, "backward-kill-line")) {
        editor_checkpoint(editor);
        editor_erase(editor, 0, editor->cursor, true);
    } else if (!strcmp(action, "kill-word")) {
        editor_checkpoint(editor);
        editor_erase(editor, editor->cursor, editor_forward_word(editor, editor->cursor), true);
    } else if (!strcmp(action, "backward-kill-word")) {
        editor_checkpoint(editor);
        editor_erase(editor, editor_backward_word(editor, editor->cursor), editor->cursor, true);
    } else if (!strcmp(action, "yank")) {
        if (editor->kill) {
            editor_checkpoint(editor);
            editor_insert(editor, editor->kill, strlen(editor->kill));
        }
    } else if (!strcmp(action, "undo") || !strcmp(action, "redo")) {
        editor_undo(editor, !strcmp(action, "redo"));
    } else if (!strcmp(action, "transpose-chars")) {
        editor_transpose_chars(editor);
    } else if (!strcmp(action, "transpose-words")) {
        editor_transpose_words(editor);
    } else if (!strcmp(action, "upcase-word") || !strcmp(action, "downcase-word") ||
               !strcmp(action, "capitalize-word")) {
        editor_case_word(editor, action);
    } else if (!strcmp(action, "previous-history") || !strcmp(action, "next-history")) {
        editor_history(editor, !strcmp(action, "previous-history") ? -1 : 1, false);
    } else if (!strcmp(action, "history-start") || !strcmp(action, "history-end")) {
        editor_history(editor, !strcmp(action, "history-start") ? -1 : 1, true);
    } else if (!strcmp(action, "reverse-search-history") || !strcmp(action, "forward-search-history")) {
        editor_search(editor, !strcmp(action, "reverse-search-history") ? -1 : 1);
    } else if (!strcmp(action, "complete") || !strcmp(action, "possible-completions")) {
        editor_complete(editor, !strcmp(action, "possible-completions"));
    } else if (!strcmp(action, "clear-screen")) {
        write_all(editor->output_fd, "\033[H\033[2J", 7);
        editor->rendered_cursor_row = 0;
        editor->rendered_rows = 0;
    } else if (!strcmp(action, "accept-line")) {
        editor->accept = true;
    } else if (!strcmp(action, "cancel-line")) {
        editor->canceled = true;
    } else if (!strcmp(action, "abort")) {
        write_all(editor->output_fd, "\a", 1);
    } else if (!strcmp(action, "revert-line")) {
        editor_checkpoint(editor);
        editor_replace(editor, editor->original ? editor->original : "", 0);
    } else if (!strcmp(action, "insert-comment")) {
        editor_checkpoint(editor);
        editor->cursor = 0;
        editor_insert(editor, "# ", 2);
        editor->accept = true;
    } else if (!strcmp(action, "self-insert")) {
        editor_checkpoint(editor);
        editor_insert(editor, key, strlen(key));
    }
}

static const char *editor_vi_action(Editor *editor, const char *key)
{
    if (!strcmp(key, "\033")) {
        editor->vi_insert = false;
        editor->vi_count = 0;
        editor->vi_operator = '\0';
        if (editor->cursor) {
            editor->cursor = editor_previous(editor->line.data, editor->cursor);
        }
        return "ignore";
    }
    if (editor->vi_insert) {
        return NULL;
    }
    if (key[1]) {
        return NULL;
    }
    char character = key[0];
    if (isdigit((unsigned char)character) && (character != '0' || editor->vi_count)) {
        if (editor->vi_count < 10000) {
            editor->vi_count = editor->vi_count * 10 + (unsigned)(character - '0');
        }
        return "ignore";
    }
    if (editor->vi_operator) {
        char operation = editor->vi_operator;
        editor->vi_operator = '\0';
        if (character == operation) {
            editor_checkpoint(editor);
            editor_erase(editor, 0, editor->line.len, true);
        } else if (character == 'w') {
            editor_action(editor, "kill-word", key);
        } else if (character == '$') {
            editor_action(editor, "kill-line", key);
        } else if (character == 'b') {
            editor_action(editor, "backward-kill-word", key);
        } else if (character == '0') {
            editor_action(editor, "backward-kill-line", key);
        } else {
            write_all(editor->output_fd, "\a", 1);
        }
        if (operation == 'c') {
            editor->vi_insert = true;
        }
        return "ignore";
    }
    switch (character) {
    case 'h':
        return "backward-char";
    case 'l':
        return "forward-char";
    case 'j':
        return "next-history";
    case 'k':
        return "previous-history";
    case '0':
    case '^':
        return "beginning-of-line";
    case '$':
        return "end-of-line";
    case 'b':
    case 'B':
        return "backward-word";
    case 'w':
    case 'W':
    case 'e':
        return "forward-word";
    case 'x':
        return "delete-char";
    case 'X':
        return "backward-delete-char";
    case 'D':
        return "kill-line";
    case 'C':
        editor->vi_insert = true;
        return "kill-line";
    case 'u':
        return "undo";
    case 'p':
        editor->cursor = editor_next(editor->line.data, editor->line.len, editor->cursor);
        return "yank";
    case 'P':
        return "yank";
    case 'i':
        editor->vi_insert = true;
        return "ignore";
    case 'a':
        editor->vi_insert = true;
        return "forward-char";
    case 'I':
        editor->vi_insert = true;
        return "beginning-of-line";
    case 'A':
        editor->vi_insert = true;
        return "end-of-line";
    case 's':
        editor->vi_insert = true;
        return "delete-char";
    case 'd':
    case 'c':
        editor->vi_operator = character;
        return "ignore";
    case '/':
        return "reverse-search-history";
    case '?':
        return "forward-search-history";
    default:
        if (character == '\022') {
            return "redo";
        }
        if ((unsigned char)character >= 32) {
            return "ignore";
        }
        return NULL;
    }
}

static void editor_release(Editor *editor)
{
    buffer_free(&editor->line);
    free(editor->kill);
    free(editor->original);
    free(editor->history_draft);
    editor_snapshot_free(editor->undo);
    editor_snapshot_free(editor->redo);
}

static char *editor_plain_line(Shell *shell, const char *prompt)
{
    if (shell->interactive) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, stdin);
    if (length < 0) {
        free(line);
        return NULL;
    }
    if (length && line[length - 1] == '\n') {
        line[--length] = '\0';
    }
    return line;
}

char *editor_readline(Shell *shell, const char *prompt)
{
    int fd = shell->terminal_fd >= 0 ? shell->terminal_fd : STDIN_FILENO;
    if (!isatty(fd) || !editor_raw_mode(fd)) {
        return editor_plain_line(shell, prompt);
    }
    Editor editor;
    memset(&editor, 0, sizeof(editor));
    editor.shell = shell;
    editor.input_fd = fd;
    editor.output_fd = fd;
    editor.prompt = prompt ? prompt : "> ";
    editor.vi_insert = true;
    editor.history_position = shell->history.len;
    editor.original = xstrdup("");
    buffer_init(&editor.line);
    buffer_append(&editor.line, "");
    editor_size(&editor);
    editor_redraw(&editor);
    while (!editor.eof && !editor.canceled && !editor.accept) {
        char key[EDITOR_KEY_LIMIT];
        int status = editor_key(&editor, key);
        if (status < 0) {
            editor.eof = true;
            break;
        }
        if (status == 0 || status == 2) {
            unsigned columns = editor.columns;
            unsigned rows = editor.rows;
            editor_size(&editor);
            if (editor.columns != columns || editor.rows != rows) {
                editor_redraw(&editor);
            }
            continue;
        }
        if (!strcmp(key, "\033[200~")) {
            editor_paste(&editor);
            editor_redraw(&editor);
            continue;
        }
        const char *action = shell->vi_mode ? editor_vi_action(&editor, key) : NULL;
        if (!action) {
            action = editor_binding_action(shell, key);
        }
        if (action) {
            unsigned repeat = editor.vi_count ? editor.vi_count : 1;
            if (strcmp(action, "ignore")) {
                editor.vi_count = 0;
            }
            for (unsigned count = 0; count < repeat && !editor.accept && !editor.canceled; count++) {
                editor_action(&editor, action, key);
            }
            if (strcmp(action, "complete") && strcmp(action, "possible-completions")) {
                editor.completion_listed = false;
            }
        } else if ((unsigned char)key[0] >= 32 && key[0] != '\177') {
            editor_checkpoint(&editor);
            editor_insert(&editor, key, strlen(key));
        } else if (key[0] == '\033') {
            write_all(editor.output_fd, "\a", 1);
        }
        editor_size(&editor);
        editor_redraw(&editor);
    }
    editor.cursor = editor.line.len;
    editor_redraw(&editor);
    if (editor.canceled) {
        write_all(editor.output_fd, "^C", 2);
        shell->last_status = 130;
    }
    write_all(editor.output_fd, "\r\n", 2);
    editor_reset_terminal();
    char *result;
    if (editor.eof) {
        result = NULL;
    } else if (editor.canceled) {
        result = xstrdup("");
    } else {
        result = xstrdup(editor.line.data ? editor.line.data : "");
    }
    editor_release(&editor);
    return result;
}
