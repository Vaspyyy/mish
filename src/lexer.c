#include "mish.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Lexing intentionally preserves source spelling. In particular, an expanded
 * string must never be lexed again as an operator or assignment. */
typedef struct {
    size_t token;
    char *delimiter;
    bool strip_tabs;
    bool expand;
} PendingDocument;

typedef struct {
    const char *source;
    size_t length;
    size_t offset;
    size_t line;
    size_t column;
    TokenVec *tokens;
    ParseError *error;
    PendingDocument *documents;
    size_t document_count;
    size_t document_capacity;
    size_t waiting_document;
    bool waiting;
    bool stop_expansion;
    bool expansion_done;
    size_t expansion_closer;
} Lexer;

typedef struct WordDocument {
    char *delimiter;
    bool strip_tabs;
    struct WordDocument *next;
} WordDocument;

typedef struct {
    char closer;
    char quote;
    bool backtick;
    bool command;
    bool word_start;
    WordDocument *documents;
} WordFrame;

static char *document_delimiter(const char *raw, bool *expand);

void parse_error_free(ParseError *error)
{
    if (error == NULL) {
        return;
    }
    free(error->message);
    memset(error, 0, sizeof(*error));
}

static int lexer_error(Lexer *lexer, const char *message, bool incomplete)
{
    if (lexer->error != NULL && lexer->error->message == NULL) {
        lexer->error->message = xstrdup(message);
        lexer->error->line = lexer->line;
        lexer->error->column = lexer->column;
        lexer->error->incomplete = incomplete;
    }
    return -1;
}

static void advance(Lexer *lexer)
{
    if (lexer->offset >= lexer->length) {
        return;
    }
    if (lexer->source[lexer->offset] == '\n') {
        ++lexer->line;
        lexer->column = 1;
    } else {
        ++lexer->column;
    }
    ++lexer->offset;
}

static char current(const Lexer *lexer)
{
    return lexer->offset < lexer->length ? lexer->source[lexer->offset] : '\0';
}

static char following(const Lexer *lexer)
{
    return lexer->offset + 1 < lexer->length ? lexer->source[lexer->offset + 1] : '\0';
}

static bool blank_character(char character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\v' || character == '\f';
}

static bool word_boundary(char character)
{
    return character == '\0' || blank_character(character) ||
        strchr("\n;&|()<>", character) != NULL;
}

static Token *emit_token(Lexer *lexer, TokenType type, size_t start,
                         size_t line, size_t column)
{
    if (lexer->tokens->len == lexer->tokens->cap) {
        size_t capacity = lexer->tokens->cap == 0 ? 32 : lexer->tokens->cap * 2;
        lexer->tokens->items = xrealloc(lexer->tokens->items, capacity * sizeof(Token));
        lexer->tokens->cap = capacity;
    }
    Token *token = &lexer->tokens->items[lexer->tokens->len++];
    memset(token, 0, sizeof(*token));
    token->type = type;
    token->text = xstrndup(lexer->source + start, lexer->offset - start);
    token->offset = start;
    token->line = line;
    token->column = column;
    return token;
}

static bool redirection_start(const Lexer *lexer)
{
    char character = current(lexer);
    if (character == '<' || character == '>') {
        return true;
    }
    if (character == '&' && following(lexer) == '>') {
        return true;
    }
    if (!isdigit((unsigned char)character)) {
        return false;
    }
    size_t position = lexer->offset;
    while (position < lexer->length && isdigit((unsigned char)lexer->source[position])) {
        ++position;
    }
    return position < lexer->length &&
        (lexer->source[position] == '<' || lexer->source[position] == '>');
}

static void scan_redirection(Lexer *lexer)
{
    while (isdigit((unsigned char)current(lexer))) {
        advance(lexer);
    }
    if (current(lexer) == '&') {
        advance(lexer);
    }
    char opener = current(lexer);
    advance(lexer);
    if (current(lexer) == opener) {
        advance(lexer);
        if (opener == '<' && current(lexer) == '<') {
            advance(lexer);
        } else if (opener == '<' && current(lexer) == '-') {
            advance(lexer);
        }
    } else if (current(lexer) == '&' ||
               (opener == '<' && current(lexer) == '>') ||
               (opener == '>' && current(lexer) == '|')) {
        advance(lexer);
    }
}

static int push_frame(Lexer *lexer, WordFrame *frames, size_t *depth,
                      char closer, bool backtick)
{
    if (*depth >= MISH_MAX_DEPTH) {
        return lexer_error(lexer, "word nesting limit exceeded", false);
    }
    frames[*depth].closer = closer;
    frames[*depth].quote = 0;
    frames[*depth].backtick = backtick;
    frames[*depth].command = closer == ')' || closer == ']' || backtick;
    frames[*depth].word_start = true;
    frames[*depth].documents = NULL;
    ++*depth;
    return 0;
}

static void word_documents_free(WordDocument *document)
{
    while (document != NULL) {
        WordDocument *next = document->next;
        free(document->delimiter);
        free(document);
        document = next;
    }
}

static void word_frames_cleanup(WordFrame *frames)
{
    for (size_t index = 0; index < MISH_MAX_DEPTH; ++index) {
        word_documents_free(frames[index].documents);
        frames[index].documents = NULL;
    }
}

static int word_document_header(Lexer *lexer, WordFrame *frame)
{
    advance(lexer);
    advance(lexer);
    bool strip_tabs = current(lexer) == '-';
    if (strip_tabs) {
        advance(lexer);
    }
    while (blank_character(current(lexer))) {
        advance(lexer);
    }
    size_t start = lexer->offset;
    char quote = 0;
    bool ansi = false;
    while (lexer->offset < lexer->length) {
        char character = current(lexer);
        if (character == '\\' && (quote != '\'' || ansi)) {
            advance(lexer);
            if (lexer->offset == lexer->length) {
                return lexer_error(lexer, "unfinished here document delimiter escape", true);
            }
            advance(lexer);
            continue;
        }
        if (quote == 0 && character == '$' && following(lexer) == '\'') {
            quote = '\'';
            ansi = true;
            advance(lexer);
            advance(lexer);
            continue;
        }
        if ((character == '\'' || character == '"') && (quote == 0 || quote == character)) {
            quote = quote == 0 ? character : 0;
            if (quote == 0) {
                ansi = false;
            }
            advance(lexer);
            continue;
        }
        if (quote == 0 && word_boundary(character)) {
            break;
        }
        advance(lexer);
    }
    if (quote != 0) {
        return lexer_error(lexer, "unterminated here document delimiter quote", true);
    }
    if (lexer->offset == start) {
        return lexer_error(lexer, "missing here document delimiter", current(lexer) == '\0');
    }
    char *raw = xstrndup(lexer->source + start, lexer->offset - start);
    bool expand;
    WordDocument *document = xcalloc(1, sizeof(*document));
    document->delimiter = document_delimiter(raw, &expand);
    document->strip_tabs = strip_tabs;
    free(raw);
    WordDocument **tail = &frame->documents;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = document;
    return 0;
}

static int word_documents_read(Lexer *lexer, WordFrame *frame)
{
    while (frame->documents != NULL) {
        WordDocument *document = frame->documents;
        bool found = false;
        while (lexer->offset < lexer->length) {
            size_t start = lexer->offset;
            while (current(lexer) != '\0' && current(lexer) != '\n') {
                advance(lexer);
            }
            size_t end = lexer->offset;
            if (document->strip_tabs) {
                while (start < end && lexer->source[start] == '\t') {
                    ++start;
                }
            }
            size_t length = strlen(document->delimiter);
            bool matches = end - start == length &&
                memcmp(lexer->source + start, document->delimiter, length) == 0;
            if (current(lexer) == '\n') {
                advance(lexer);
            }
            if (matches) {
                found = true;
                break;
            }
        }
        if (!found) {
            return lexer_error(lexer, "unterminated substitution here document", true);
        }
        frame->documents = document->next;
        free(document->delimiter);
        free(document);
    }
    return 0;
}

static int scan_word(Lexer *lexer)
{
    WordFrame frames[MISH_MAX_DEPTH];
    memset(frames, 0, sizeof(frames));
    size_t depth = 1;
    frames[0].word_start = true;
    while (lexer->offset < lexer->length) {
        WordFrame *frame = &frames[depth - 1];
        char character = current(lexer);
        if (frame->closer == '\'') {
            if (character == '\\') {
                advance(lexer);
                if (lexer->offset == lexer->length) {
                    word_frames_cleanup(frames);
                    return lexer_error(lexer, "unfinished ANSI quote escape", true);
                }
                advance(lexer);
            } else {
                if (character == '\'') {
                    --depth;
                }
                advance(lexer);
            }
            continue;
        }
        if (frame->quote == '\'') {
            if (character == '\'') {
                frame->quote = 0;
            }
            advance(lexer);
            continue;
        }
        if (character == '\\') {
            advance(lexer);
            if (lexer->offset == lexer->length) {
                word_frames_cleanup(frames);
                return lexer_error(lexer, "unfinished backslash escape", true);
            }
            advance(lexer);
            frame->word_start = false;
            continue;
        }
        if (character == '\'' && frame->quote == 0) {
            frame->quote = '\'';
            frame->word_start = false;
            advance(lexer);
            continue;
        }
        if (character == '"') {
            frame->quote = frame->quote == '"' ? 0 : '"';
            frame->word_start = false;
            advance(lexer);
            continue;
        }
        if (character == '`') {
            if (frame->backtick && frame->quote == 0) {
                --depth;
                advance(lexer);
                continue;
            }
            if (push_frame(lexer, frames, &depth, '`', true) != 0) {
                word_frames_cleanup(frames);
                return -1;
            }
            advance(lexer);
            continue;
        }
        if (character == '$') {
            char next = following(lexer);
            if (next == '(' || next == '[' || next == '{') {
                char closer = next == '(' ? ')' : next == '[' ? ']' : '}';
                advance(lexer);
                advance(lexer);
                if (push_frame(lexer, frames, &depth, closer, false) != 0) {
                    word_frames_cleanup(frames);
                    return -1;
                }
                if (next == '(' && current(lexer) == '(') {
                    frames[depth - 1].command = false;
                }
                continue;
            }
            if (next == '\'') {
                /* ANSI C quotes permit escaped apostrophes. A separate frame
                 * distinguishes those escapes from ordinary single quotes. */
                advance(lexer);
                advance(lexer);
                if (push_frame(lexer, frames, &depth, '\'', false) != 0) {
                    word_frames_cleanup(frames);
                    return -1;
                }
                continue;
            }
        }
        if (frame->quote == 0 && depth > 1) {
            if (frame->command && frame->word_start && character == '#') {
                while (current(lexer) != '\0' && current(lexer) != '\n') {
                    advance(lexer);
                }
                continue;
            }
            if (frame->command && character == '<' && following(lexer) == '<' &&
                !(lexer->offset + 2 < lexer->length && lexer->source[lexer->offset + 2] == '<')) {
                if (word_document_header(lexer, frame) != 0) {
                    word_frames_cleanup(frames);
                    return -1;
                }
                frame->word_start = false;
                continue;
            }
            if (frame->command && character == '\n' && frame->documents != NULL) {
                advance(lexer);
                if (word_documents_read(lexer, frame) != 0) {
                    word_frames_cleanup(frames);
                    return -1;
                }
                frame->word_start = true;
                continue;
            }
            if (character == frame->closer) {
                if (frame->documents != NULL) {
                    word_frames_cleanup(frames);
                    return lexer_error(lexer, "unfinished substitution here document", true);
                }
                --depth;
                if (depth == 1 && lexer->stop_expansion) {
                    lexer->expansion_done = true;
                    lexer->expansion_closer = lexer->offset;
                    advance(lexer);
                    break;
                }
                advance(lexer);
                continue;
            }
            char opener = frame->closer == ')' ? '(' : frame->closer == ']' ? '[' : '{';
            if (!frame->backtick && character == opener) {
                bool command_context = frame->command;
                if (push_frame(lexer, frames, &depth, frame->closer, false) != 0) {
                    word_frames_cleanup(frames);
                    return -1;
                }
                frames[depth - 1].command = command_context;
                advance(lexer);
                continue;
            }
        }
        if (depth == 1 && frame->quote == 0 && word_boundary(character)) {
            break;
        }
        frame->word_start = frame->quote == 0 && word_boundary(character);
        advance(lexer);
    }
    if (depth != 1) {
        word_frames_cleanup(frames);
        return lexer_error(lexer, "unterminated substitution or quoted word", true);
    }
    if (frames[0].quote != 0) {
        word_frames_cleanup(frames);
        return lexer_error(lexer, "unterminated quoted word", true);
    }
    word_frames_cleanup(frames);
    return 0;
}

int lexer_expansion_end(const char *source, size_t dollar, size_t *closer, ParseError *error)
{
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    Lexer lexer;
    memset(&lexer, 0, sizeof(lexer));
    lexer.source = source == NULL ? "" : source;
    lexer.length = strlen(lexer.source);
    lexer.error = error;
    lexer.line = 1;
    lexer.column = 1;
    lexer.stop_expansion = true;
    if (closer == NULL || dollar >= lexer.length || lexer.source[dollar] != '$' ||
        dollar + 1 >= lexer.length || strchr("([{", lexer.source[dollar + 1]) == NULL) {
        return lexer_error(&lexer, "expected dollar substitution opener", false);
    }
    if (lexer.length > MISH_MAX_SOURCE) {
        return lexer_error(&lexer, "source limit exceeded", false);
    }
    while (lexer.offset < dollar) {
        advance(&lexer);
    }
    if (scan_word(&lexer) != 0) {
        return -1;
    }
    if (!lexer.expansion_done) {
        return lexer_error(&lexer, "unterminated expansion", true);
    }
    *closer = lexer.expansion_closer;
    return 0;
}

static int scan_arithmetic_clause(Lexer *lexer)
{
    unsigned depth = 0;
    char quote = 0;
    while (lexer->offset < lexer->length) {
        char character = current(lexer);
        if (character == '\\') {
            advance(lexer);
            if (lexer->offset == lexer->length) {
                return lexer_error(lexer, "unfinished arithmetic escape", true);
            }
            advance(lexer);
            continue;
        }
        if (quote != 0) {
            if (character == quote) {
                quote = 0;
            }
            advance(lexer);
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
            advance(lexer);
            continue;
        }
        if (character == '(') {
            if (++depth > MISH_MAX_DEPTH) {
                return lexer_error(lexer, "arithmetic clause nesting limit exceeded", false);
            }
        } else if (character == ')') {
            if (depth == 0) {
                return lexer_error(lexer, "unexpected arithmetic closing parenthesis", false);
            }
            --depth;
            advance(lexer);
            if (depth == 0) {
                if (lexer->offset < 2 || lexer->source[lexer->offset - 2] != ')') {
                    return lexer_error(lexer, "arithmetic clause must end with adjacent ))", false);
                }
                return 0;
            }
            continue;
        }
        advance(lexer);
    }
    return lexer_error(lexer, "unterminated arithmetic clause", true);
}

static void delimiter_codepoint(Buffer *buffer, unsigned value)
{
    if (value == 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        return;
    }
    if (value < 0x80) {
        buffer_putc(buffer, (char)value);
    } else if (value < 0x800) {
        buffer_putc(buffer, (char)(0xc0 | (value >> 6)));
        buffer_putc(buffer, (char)(0x80 | (value & 0x3f)));
    } else if (value < 0x10000) {
        buffer_putc(buffer, (char)(0xe0 | (value >> 12)));
        buffer_putc(buffer, (char)(0x80 | ((value >> 6) & 0x3f)));
        buffer_putc(buffer, (char)(0x80 | (value & 0x3f)));
    } else {
        buffer_putc(buffer, (char)(0xf0 | (value >> 18)));
        buffer_putc(buffer, (char)(0x80 | ((value >> 12) & 0x3f)));
        buffer_putc(buffer, (char)(0x80 | ((value >> 6) & 0x3f)));
        buffer_putc(buffer, (char)(0x80 | (value & 0x3f)));
    }
}

static void delimiter_ansi_escape(Buffer *buffer, const char *raw, size_t *index)
{
    char character = raw[++*index];
    const char *keys = "abefnrtv";
    const char *values = "\a\b\033\f\n\r\t\v";
    const char *found = character == '\0' ? NULL : strchr(keys, character);
    if (found != NULL) {
        buffer_putc(buffer, values[found - keys]);
        return;
    }
    if (character == '\\' || character == '\'' || character == '"') {
        buffer_putc(buffer, character);
        return;
    }
    if (character == '\n' || character == '\0') {
        return;
    }
    unsigned base = 0;
    unsigned maximum = 0;
    unsigned value = 0;
    if (character >= '0' && character <= '7') {
        base = 8;
        maximum = 3;
        --*index;
    } else if (character == 'x' || character == 'u' || character == 'U') {
        base = 16;
        maximum = character == 'x' ? 2 : character == 'u' ? 4 : 8;
    }
    if (base != 0) {
        unsigned digits = 0;
        while (digits < maximum) {
            unsigned char next = (unsigned char)raw[*index + 1];
            unsigned digit;
            if (next >= '0' && next <= '9') {
                digit = next - '0';
            } else if (next >= 'a' && next <= 'f') {
                digit = next - 'a' + 10;
            } else if (next >= 'A' && next <= 'F') {
                digit = next - 'A' + 10;
            } else {
                break;
            }
            if (digit >= base) {
                break;
            }
            value = value * base + digit;
            ++*index;
            ++digits;
        }
        if (digits != 0) {
            if (character == 'u' || character == 'U') {
                delimiter_codepoint(buffer, value);
            } else if ((value & 255) != 0) {
                buffer_putc(buffer, (char)(value & 255));
            }
            return;
        }
    }
    buffer_putc(buffer, '\\');
    buffer_putc(buffer, character);
}

static char *document_delimiter(const char *raw, bool *expand)
{
    Buffer delimiter;
    buffer_init(&delimiter);
    char quote = 0;
    bool ansi = false;
    *expand = true;
    for (size_t index = 0; raw[index] != '\0'; ++index) {
        char character = raw[index];
        if (ansi && character == '\\') {
            delimiter_ansi_escape(&delimiter, raw, &index);
        } else if (ansi && character == '\'') {
            ansi = false;
        } else if (ansi) {
            buffer_putc(&delimiter, character);
        } else if (quote == 0 && character == '$' && raw[index + 1] == '\'') {
            *expand = false;
            ansi = true;
            ++index;
        } else if (character == '\\' && quote != '\'') {
            *expand = false;
            if (raw[index + 1] != '\0') {
                if (quote == '"' && strchr("$`\"\\\n", raw[index + 1]) == NULL) {
                    buffer_putc(&delimiter, '\\');
                } else if (raw[index + 1] == '\n') {
                    ++index;
                } else {
                    buffer_putc(&delimiter, raw[++index]);
                }
            }
        } else if ((character == '\'' || character == '"') &&
                   (quote == 0 || quote == character)) {
            *expand = false;
            quote = quote == 0 ? character : 0;
        } else {
            buffer_putc(&delimiter, character);
        }
    }
    return buffer_take(&delimiter);
}

static int queue_document(Lexer *lexer, size_t redirection, const char *raw)
{
    if (lexer->document_count == lexer->document_capacity) {
        size_t capacity = lexer->document_capacity == 0 ? 8 : lexer->document_capacity * 2;
        lexer->documents = xrealloc(lexer->documents, capacity * sizeof(PendingDocument));
        lexer->document_capacity = capacity;
    }
    PendingDocument *document = &lexer->documents[lexer->document_count++];
    document->token = redirection;
    document->delimiter = document_delimiter(raw, &document->expand);
    document->strip_tabs = strstr(lexer->tokens->items[redirection].text, "<<-") != NULL;
    return 0;
}

static int read_documents(Lexer *lexer)
{
    for (size_t index = 0; index < lexer->document_count; ++index) {
        PendingDocument *document = &lexer->documents[index];
        Buffer body;
        buffer_init(&body);
        bool found = false;
        while (lexer->offset < lexer->length) {
            size_t start = lexer->offset;
            while (current(lexer) != '\n' && current(lexer) != '\0') {
                advance(lexer);
            }
            size_t end = lexer->offset;
            if (document->strip_tabs) {
                while (start < end && lexer->source[start] == '\t') {
                    ++start;
                }
            }
            bool newline = current(lexer) == '\n';
            size_t delimiter_length = strlen(document->delimiter);
            bool matches = end - start == delimiter_length &&
                memcmp(lexer->source + start, document->delimiter, delimiter_length) == 0;
            if (newline) {
                advance(lexer);
            }
            if (matches) {
                found = true;
                break;
            }
            buffer_append_n(&body, lexer->source + start, end - start);
            if (newline) {
                buffer_putc(&body, '\n');
            }
        }
        if (!found) {
            buffer_free(&body);
            return lexer_error(lexer, "unterminated here document", true);
        }
        Token *token = &lexer->tokens->items[document->token];
        token->heredoc = buffer_take(&body);
        token->heredoc_expand = document->expand;
        token->heredoc_strip_tabs = document->strip_tabs;
        free(document->delimiter);
        document->delimiter = NULL;
    }
    lexer->document_count = 0;
    return 0;
}

static void lexer_cleanup(Lexer *lexer)
{
    for (size_t index = 0; index < lexer->document_count; ++index) {
        free(lexer->documents[index].delimiter);
    }
    free(lexer->documents);
}

int lex_source(const char *source, TokenVec *tokens, ParseError *error)
{
    memset(tokens, 0, sizeof(*tokens));
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    Lexer lexer;
    memset(&lexer, 0, sizeof(lexer));
    lexer.source = source == NULL ? "" : source;
    lexer.length = strlen(lexer.source);
    lexer.line = 1;
    lexer.column = 1;
    lexer.tokens = tokens;
    lexer.error = error;
    if (lexer.length > MISH_MAX_SOURCE) {
        return lexer_error(&lexer, "source limit exceeded", false);
    }
    while (lexer.offset < lexer.length) {
        char character = current(&lexer);
        if (blank_character(character)) {
            advance(&lexer);
            continue;
        }
        if (character == '\\' && following(&lexer) == '\n') {
            advance(&lexer);
            advance(&lexer);
            continue;
        }
        if (character == '#') {
            while (current(&lexer) != '\0' && current(&lexer) != '\n') {
                advance(&lexer);
            }
            continue;
        }
        size_t start = lexer.offset;
        size_t line = lexer.line;
        size_t column = lexer.column;
        TokenType type = TOK_WORD;
        if (character == '(' && following(&lexer) == '(') {
            if (scan_arithmetic_clause(&lexer) != 0) {
                lexer_cleanup(&lexer);
                tokens_free(tokens);
                return -1;
            }
        } else if (redirection_start(&lexer)) {
            scan_redirection(&lexer);
            type = TOK_REDIR;
        } else if (character == '\n') {
            advance(&lexer);
            type = TOK_NEWLINE;
        } else if (character == ';') {
            advance(&lexer);
            type = TOK_SEMI;
            if (current(&lexer) == ';') {
                advance(&lexer);
                type = TOK_DSEMI;
            }
        } else if (character == '&') {
            advance(&lexer);
            type = TOK_AMP;
            if (current(&lexer) == '&') {
                advance(&lexer);
                type = TOK_AND;
            }
        } else if (character == '|') {
            advance(&lexer);
            type = TOK_PIPE;
            if (current(&lexer) == '|') {
                advance(&lexer);
                type = TOK_OR;
            } else if (current(&lexer) == '&') {
                advance(&lexer);
            }
        } else if (character == '(' || character == ')') {
            advance(&lexer);
            type = character == '(' ? TOK_LPAREN : TOK_RPAREN;
        } else if ((character == '{' || character == '}') && word_boundary(following(&lexer))) {
            advance(&lexer);
            type = character == '{' ? TOK_LBRACE : TOK_RBRACE;
        } else if (scan_word(&lexer) != 0) {
            lexer_cleanup(&lexer);
            tokens_free(tokens);
            return -1;
        }
        if (lexer.offset == start) {
            lexer_cleanup(&lexer);
            tokens_free(tokens);
            return lexer_error(&lexer, "unexpected character", false);
        }
        emit_token(&lexer, type, start, line, column);
        if (lexer.waiting) {
            if (type != TOK_WORD) {
                lexer_cleanup(&lexer);
                tokens_free(tokens);
                return lexer_error(&lexer, "expected here document delimiter", type == TOK_NEWLINE);
            }
            queue_document(&lexer, lexer.waiting_document, tokens->items[tokens->len - 1].text);
            lexer.waiting = false;
        } else if (type == TOK_REDIR) {
            const char *operator = tokens->items[tokens->len - 1].text;
            while (isdigit((unsigned char)*operator)) {
                ++operator;
            }
            if (strcmp(operator, "<<") == 0 || strcmp(operator, "<<-") == 0) {
                lexer.waiting = true;
                lexer.waiting_document = tokens->len - 1;
            }
        }
        if (type == TOK_NEWLINE && lexer.document_count != 0 && read_documents(&lexer) != 0) {
            lexer_cleanup(&lexer);
            tokens_free(tokens);
            return -1;
        }
    }
    if (lexer.waiting || lexer.document_count != 0) {
        lexer_cleanup(&lexer);
        tokens_free(tokens);
        return lexer_error(&lexer, "unfinished here document", true);
    }
    emit_token(&lexer, TOK_EOF, lexer.offset, lexer.line, lexer.column);
    lexer_cleanup(&lexer);
    return 0;
}

void tokens_free(TokenVec *tokens)
{
    if (tokens == NULL) {
        return;
    }
    for (size_t index = 0; index < tokens->len; ++index) {
        free(tokens->items[index].text);
        free(tokens->items[index].heredoc);
    }
    free(tokens->items);
    memset(tokens, 0, sizeof(*tokens));
}
