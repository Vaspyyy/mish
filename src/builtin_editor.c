#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct KeyBinding {
    char *sequence;
    char *action;
    struct KeyBinding *next;
};

typedef struct {
    const char *sequence;
    const char *action;
} DefaultBinding;

static const DefaultBinding default_bindings[] = {
    { "\001", "beginning-of-line" },
    { "\002", "backward-char" },
    { "\003", "cancel-line" },
    { "\004", "delete-char-or-eof" },
    { "\005", "end-of-line" },
    { "\006", "forward-char" },
    { "\007", "abort" },
    { "\010", "backward-delete-char" },
    { "\011", "complete" },
    { "\012", "accept-line" },
    { "\013", "kill-line" },
    { "\014", "clear-screen" },
    { "\015", "accept-line" },
    { "\016", "next-history" },
    { "\020", "previous-history" },
    { "\022", "reverse-search-history" },
    { "\023", "forward-search-history" },
    { "\024", "transpose-chars" },
    { "\025", "backward-kill-line" },
    { "\027", "backward-kill-word" },
    { "\031", "yank" },
    { "\037", "undo" },
    { "\177", "backward-delete-char" },
    { "\033[A", "previous-history" },
    { "\033[B", "next-history" },
    { "\033[C", "forward-char" },
    { "\033[D", "backward-char" },
    { "\033[H", "beginning-of-line" },
    { "\033[F", "end-of-line" },
    { "\033OH", "beginning-of-line" },
    { "\033OF", "end-of-line" },
    { "\033[1~", "beginning-of-line" },
    { "\033[4~", "end-of-line" },
    { "\033[7~", "beginning-of-line" },
    { "\033[8~", "end-of-line" },
    { "\033[3~", "delete-char" },
    { "\033[5~", "history-start" },
    { "\033[6~", "history-end" },
    { "\033[1;5D", "backward-word" },
    { "\033[1;5C", "forward-word" },
    { "\033b", "backward-word" },
    { "\033f", "forward-word" },
    { "\033d", "kill-word" },
    { "\033\177", "backward-kill-word" },
    { "\033\010", "backward-kill-word" },
    { "\033t", "transpose-words" },
    { "\033u", "upcase-word" },
    { "\033l", "downcase-word" },
    { "\033c", "capitalize-word" },
    { "\033<", "history-start" },
    { "\033>", "history-end" },
    { "\033?", "possible-completions" },
    { "\033\037", "redo" },
    { "\033r", "revert-line" },
    { "\033#", "insert-comment" },
};

static const char *const editor_actions[] = {
    "beginning-of-line", "end-of-line", "backward-char", "forward-char",
    "backward-word", "forward-word", "backward-delete-char", "delete-char",
    "delete-char-or-eof", "kill-line", "backward-kill-line", "kill-word",
    "backward-kill-word", "yank", "undo", "redo", "transpose-chars",
    "transpose-words", "upcase-word", "downcase-word", "capitalize-word",
    "previous-history", "next-history", "history-start", "history-end",
    "reverse-search-history", "forward-search-history", "complete",
    "possible-completions", "clear-screen", "accept-line", "cancel-line",
    "abort", "revert-line", "insert-comment", "self-insert", "ignore"
};

bool editor_action_supported(const char *action)
{
    for (size_t index = 0; index < sizeof(editor_actions) / sizeof(editor_actions[0]); index++) {
        if (!strcmp(editor_actions[index], action)) {
            return true;
        }
    }
    return false;
}

const char *editor_binding_action(const Shell *shell, const char *sequence)
{
    for (KeyBinding *binding = shell->bindings; binding; binding = binding->next) {
        if (!strcmp(binding->sequence, sequence)) {
            return binding->action;
        }
    }
    for (size_t index = 0; index < sizeof(default_bindings) / sizeof(default_bindings[0]); index++) {
        if (!strcmp(default_bindings[index].sequence, sequence)) {
            return default_bindings[index].action;
        }
    }
    return NULL;
}

bool editor_binding_prefix(const Shell *shell, const char *sequence)
{
    size_t length = strlen(sequence);
    for (KeyBinding *binding = shell->bindings; binding; binding = binding->next) {
        if (strlen(binding->sequence) > length &&
            !strncmp(binding->sequence, sequence, length)) {
            return true;
        }
    }
    for (size_t index = 0; index < sizeof(default_bindings) / sizeof(default_bindings[0]); index++) {
        if (strlen(default_bindings[index].sequence) > length &&
            !strncmp(default_bindings[index].sequence, sequence, length)) {
            return true;
        }
    }
    return false;
}

void editor_state_destroy(Shell *shell)
{
    while (shell->bindings) {
        KeyBinding *next = shell->bindings->next;
        free(shell->bindings->sequence);
        free(shell->bindings->action);
        free(shell->bindings);
        shell->bindings = next;
    }
}

static char *binding_trim(const char *text)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) {
        length--;
    }
    if (length >= 2 && (text[0] == '"' || text[0] == '\'') && text[length - 1] == text[0]) {
        return xstrndup(text + 1, length - 2);
    }
    return xstrndup(text, length);
}

static char *binding_decode(const char *text, char **error)
{
    char *trimmed = binding_trim(text);
    Buffer sequence;
    buffer_init(&sequence);
    for (size_t index = 0; trimmed[index]; index++) {
        unsigned char character = (unsigned char)trimmed[index];
        if (character == '^' && trimmed[index + 1]) {
            character = (unsigned char)trimmed[++index];
            character = character == '?' ? 127 : (unsigned char)(toupper(character) & 31);
        } else if (character == '\\') {
            character = (unsigned char)trimmed[++index];
            if (!character) {
                *error = xstrdup("unfinished key escape");
                break;
            }
            if (character == 'C' && trimmed[index + 1] == '-' && trimmed[index + 2]) {
                index += 2;
                character = (unsigned char)trimmed[index];
                character = character == '?' ? 127 : (unsigned char)(toupper(character) & 31);
            } else if (character == 'M' && trimmed[index + 1] == '-' && trimmed[index + 2]) {
                buffer_putc(&sequence, '\033');
                index += 2;
                character = (unsigned char)trimmed[index];
                if (character == '\\' && trimmed[index + 1] == 'C' &&
                    trimmed[index + 2] == '-' && trimmed[index + 3]) {
                    index += 3;
                    character = (unsigned char)(toupper((unsigned char)trimmed[index]) & 31);
                }
            } else if (character == 'e' || character == 'E') {
                character = 27;
            } else if (character == 'n') {
                character = '\n';
            } else if (character == 'r') {
                character = '\r';
            } else if (character == 't') {
                character = '\t';
            } else if (character == 'b') {
                character = '\b';
            } else if (character >= '0' && character <= '7') {
                unsigned number = character - '0';
                for (unsigned digit = 1; digit < 3; digit++) {
                    char next = trimmed[index + 1];
                    if (next < '0' || next > '7') {
                        break;
                    }
                    number = number * 8 + (unsigned)(next - '0');
                    index++;
                }
                character = (unsigned char)number;
            }
        }
        if (!character) {
            *error = xstrdup("NUL is not a bindable key sequence");
            break;
        }
        buffer_putc(&sequence, (char)character);
        if (sequence.len > 63) {
            *error = xstrdup("key sequence exceeds 63 bytes");
            break;
        }
    }
    free(trimmed);
    if (*error) {
        buffer_free(&sequence);
        return NULL;
    }
    if (!sequence.len) {
        *error = xstrdup("empty key sequence");
        buffer_free(&sequence);
        return NULL;
    }
    return buffer_take(&sequence);
}

static char *binding_encode(const char *sequence)
{
    Buffer text;
    buffer_init(&text);
    for (const unsigned char *cursor = (const unsigned char *)sequence; *cursor; cursor++) {
        if (*cursor == 27) {
            buffer_append(&text, "\\e");
        } else if (*cursor == 127) {
            buffer_append(&text, "\\C-?");
        } else if (*cursor < 32) {
            buffer_append(&text, "\\C-");
            buffer_putc(&text, (char)('a' + *cursor - 1));
        } else {
            if (*cursor == '\\' || *cursor == '"') {
                buffer_putc(&text, '\\');
            }
            buffer_putc(&text, (char)*cursor);
        }
    }
    return buffer_take(&text);
}

static void binding_print(const char *sequence, const char *action)
{
    char *key = binding_encode(sequence);
    char *specification = str_printf("\"%s\": %s", key, action);
    char *quoted = shell_quote(specification);
    printf("bind %s\n", quoted);
    free(key);
    free(specification);
    free(quoted);
}

static void binding_set(Shell *shell, char *sequence, char *action)
{
    for (KeyBinding *binding = shell->bindings; binding; binding = binding->next) {
        if (!strcmp(binding->sequence, sequence)) {
            free(binding->action);
            binding->action = action;
            free(sequence);
            return;
        }
    }
    KeyBinding *binding = xcalloc(1, sizeof(*binding));
    binding->sequence = sequence;
    binding->action = action;
    binding->next = shell->bindings;
    shell->bindings = binding;
}

int bind_builtin(Shell *shell, int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-p"))) {
        for (size_t index = 0; index < sizeof(default_bindings) / sizeof(default_bindings[0]); index++) {
            const char *sequence = default_bindings[index].sequence;
            binding_print(sequence, editor_binding_action(shell, sequence));
        }
        for (KeyBinding *binding = shell->bindings; binding; binding = binding->next) {
            bool standard = false;
            for (size_t index = 0; index < sizeof(default_bindings) / sizeof(default_bindings[0]); index++) {
                if (!strcmp(default_bindings[index].sequence, binding->sequence)) {
                    standard = true;
                    break;
                }
            }
            if (!standard) {
                binding_print(binding->sequence, binding->action);
            }
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "-l")) {
        for (size_t index = 0; index < sizeof(editor_actions) / sizeof(editor_actions[0]); index++) {
            puts(editor_actions[index]);
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("bind [-p|-l] | bind -r KEY | bind 'KEY: ACTION'");
        puts("KEY accepts \\C-a, \\M-f, \\e[A, octal escapes, and ^A notation.");
        puts("Bindings name editing actions, not shell commands. -r disables a key.");
        return 0;
    }
    bool remove = argc == 3 && !strcmp(argv[1], "-r");
    if (!remove && argc != 2) {
        fputs("bind: expected one binding, -p, -l, or -r KEY\n", stderr);
        return 2;
    }
    const char *specification = remove ? argv[2] : argv[1];
    const char *colon = remove ? NULL : strrchr(specification, ':');
    if (!remove && !colon) {
        fputs("bind: binding requires KEY: ACTION\n", stderr);
        return 2;
    }
    char *key = remove ? xstrdup(specification) :
                xstrndup(specification, (size_t)(colon - specification));
    char *error = NULL;
    char *sequence = binding_decode(key, &error);
    free(key);
    if (!sequence) {
        fprintf(stderr, "bind: %s\n", error);
        free(error);
        return 2;
    }
    char *action = remove ? xstrdup("ignore") : binding_trim(colon + 1);
    if (!editor_action_supported(action)) {
        fprintf(stderr, "bind: unknown action: %s\n", action);
        free(sequence);
        free(action);
        return 2;
    }
    binding_set(shell, sequence, action);
    return 0;
}

int prompt_builtin(Shell *shell, int argc, char **argv)
{
    if (argc == 1) {
        const char *prompt = shell_get(shell, "PS1");
        char *quoted = shell_quote(prompt ? prompt : "> ");
        printf("PS1=%s\n", quoted);
        free(quoted);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("prompt [TEXT] | --preview [TEXT] | --reset");
        puts("Escapes: \\u \\h \\H \\w \\W \\$ \\! \\# \\? \\s \\v \\t \\T \\@ \\d \\D{FORMAT}");
        puts("\\e emits ESC; \\[ and \\] mark zero-width color sequences; \\n adds a newline.");
        puts("Variables and command substitutions use normal mish expansion.");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--reset")) {
        return shell_set(shell, "PS1", "\\u@\\h:\\w\\$ ");
    }
    bool preview = !strcmp(argv[1], "--preview");
    if ((preview && argc > 3) || (!preview && argc != 2)) {
        fputs("prompt: expected one prompt string\n", stderr);
        return 2;
    }
    if (preview) {
        const char *source = argc == 3 ? argv[2] : shell_get(shell, "PS1");
        char *expanded = expand_prompt(shell, source ? source : "> ");
        puts(expanded);
        free(expanded);
        return 0;
    }
    return shell_set(shell, "PS1", argv[1]);
}
