#include "mish.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    TokenVec tokens;
    size_t position;
    unsigned depth;
    ParseError *error;
    bool failed;
} Parser;

typedef struct {
    const char *const *words;
    TokenType closer;
    bool has_closer;
    bool case_separator;
    bool brace_separator;
} Stop;

static Node *parse_list(Parser *parser, Stop stop);
static Node *parse_command(Parser *parser);
static Node *parse_if(Parser *parser, bool elif);

static Token *peek(Parser *parser)
{
    if (parser->position >= parser->tokens.len) {
        return &parser->tokens.items[parser->tokens.len - 1];
    }
    return &parser->tokens.items[parser->position];
}

static Token *lookahead(Parser *parser, size_t distance)
{
    size_t index = parser->position + distance;
    if (index >= parser->tokens.len) {
        index = parser->tokens.len - 1;
    }
    return &parser->tokens.items[index];
}

static void consume(Parser *parser)
{
    if (peek(parser)->type != TOK_EOF) {
        ++parser->position;
    }
}

static bool word_is(Parser *parser, const char *word)
{
    Token *token = peek(parser);
    return token->type == TOK_WORD && strcmp(token->text, word) == 0;
}

static void parser_error(Parser *parser, const char *message, bool incomplete)
{
    parser->failed = true;
    if (parser->error == NULL || parser->error->message != NULL) {
        return;
    }
    Token *token = peek(parser);
    parser->error->message = xstrdup(message);
    parser->error->line = token->line;
    parser->error->column = token->column;
    parser->error->incomplete = incomplete;
}

static void unexpected(Parser *parser, const char *expected)
{
    Token *token = peek(parser);
    char *message;
    if (token->type == TOK_EOF) {
        message = str_printf("expected %s before end of input", expected);
    } else {
        message = str_printf("expected %s, found '%s'", expected, token->text);
    }
    parser_error(parser, message, token->type == TOK_EOF);
    free(message);
}

static bool expect_type(Parser *parser, TokenType type, const char *description)
{
    if (peek(parser)->type != type) {
        unexpected(parser, description);
        return false;
    }
    consume(parser);
    return true;
}

static bool expect_word(Parser *parser, const char *word)
{
    if (!word_is(parser, word)) {
        unexpected(parser, word);
        return false;
    }
    consume(parser);
    return true;
}

static void skip_newlines(Parser *parser)
{
    while (peek(parser)->type == TOK_NEWLINE) {
        consume(parser);
    }
}

static bool at_stop(Parser *parser, Stop stop)
{
    Token *token = peek(parser);
    if (stop.has_closer && token->type == stop.closer) {
        return true;
    }
    if (stop.case_separator && token->type == TOK_DSEMI) {
        return true;
    }
    if (token->type == TOK_WORD && stop.words != NULL) {
        for (size_t index = 0; stop.words[index] != NULL; ++index) {
            if (strcmp(token->text, stop.words[index]) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool reserved_word(const char *word)
{
    static const char *const reserved[] = {
        "then", "elif", "else", "fi", "do", "done", "in", "esac", NULL
    };
    for (size_t index = 0; reserved[index] != NULL; ++index) {
        if (strcmp(word, reserved[index]) == 0) {
            return true;
        }
    }
    return false;
}

static Node *single_child(NodeType type, Node *child, size_t line)
{
    Node *node = node_new(type);
    node->line = line;
    node_add_child(node, child);
    return node;
}

static void append_redirection(Node *node, Redir *redirection)
{
    Redir **tail = &node->redirs;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = redirection;
}

static Redir *new_redirection(int fd, RedirType type, const char *word)
{
    Redir *redirection = xcalloc(1, sizeof(*redirection));
    redirection->fd = fd;
    redirection->type = type;
    redirection->word = xstrdup(word);
    return redirection;
}

static int parse_redirection(Parser *parser, Node *node)
{
    Token *token = peek(parser);
    const char *operator = token->text;
    int fd = -1;
    if (isdigit((unsigned char)*operator)) {
        errno = 0;
        char *end;
        long descriptor = strtol(operator, &end, 10);
        if (errno != 0 || descriptor > INT_MAX) {
            parser_error(parser, "redirection descriptor out of range", false);
            return -1;
        }
        fd = (int)descriptor;
        operator = end;
    }
    RedirType type;
    bool both = false;
    if (strcmp(operator, "<") == 0) {
        type = REDIR_INPUT;
    } else if (strcmp(operator, ">") == 0) {
        type = REDIR_OUTPUT;
    } else if (strcmp(operator, ">>") == 0) {
        type = REDIR_APPEND;
    } else if (strcmp(operator, ">|") == 0) {
        type = REDIR_CLOBBER;
    } else if (strcmp(operator, "<&") == 0) {
        type = REDIR_DUP_INPUT;
    } else if (strcmp(operator, ">&") == 0) {
        type = REDIR_DUP_OUTPUT;
    } else if (strcmp(operator, "<>") == 0) {
        type = REDIR_READWRITE;
    } else if (strcmp(operator, "<<") == 0 || strcmp(operator, "<<-") == 0) {
        type = REDIR_HEREDOC;
    } else if (strcmp(operator, "<<<") == 0) {
        type = REDIR_HERESTRING;
    } else if (strcmp(operator, "&>") == 0 || strcmp(operator, "&>>") == 0) {
        type = strcmp(operator, "&>>") == 0 ? REDIR_APPEND : REDIR_OUTPUT;
        both = true;
    } else {
        parser_error(parser, "unsupported redirection operator", false);
        return -1;
    }
    if (fd < 0) {
        fd = *operator == '<' ? 0 : 1;
    }
    consume(parser);
    if (peek(parser)->type != TOK_WORD) {
        unexpected(parser, "redirection target");
        return -1;
    }
    Redir *redirection = new_redirection(fd, type, peek(parser)->text);
    if (type == REDIR_HEREDOC) {
        redirection->heredoc = xstrdup(token->heredoc == NULL ? "" : token->heredoc);
        redirection->heredoc_expand = token->heredoc_expand;
    }
    append_redirection(node, redirection);
    if (both) {
        append_redirection(node, new_redirection(2, REDIR_DUP_OUTPUT, "1"));
    }
    consume(parser);
    return 0;
}

static bool require_body(Parser *parser, Node *body, const char *description)
{
    if (body != NULL && body->count != 0) {
        return true;
    }
    if (!parser->failed) {
        parser_error(parser, description, peek(parser)->type == TOK_EOF);
    }
    return false;
}

static Node *parse_simple(Parser *parser)
{
    Node *node = node_new(NODE_SIMPLE);
    node->line = peek(parser)->line;
    while (peek(parser)->type == TOK_WORD || peek(parser)->type == TOK_REDIR) {
        if (peek(parser)->type == TOK_REDIR) {
            if (parse_redirection(parser, node) != 0) {
                node_free(node);
                return NULL;
            }
        } else {
            strvec_push(&node->words, peek(parser)->text);
            consume(parser);
        }
    }
    if (node->words.len == 0 && node->redirs == NULL) {
        unexpected(parser, "command");
        node_free(node);
        return NULL;
    }
    return node;
}

static Node *parse_group(Parser *parser, bool subshell)
{
    size_t line = peek(parser)->line;
    consume(parser);
    Stop stop;
    memset(&stop, 0, sizeof(stop));
    stop.has_closer = true;
    stop.closer = subshell ? TOK_RPAREN : TOK_RBRACE;
    stop.brace_separator = !subshell;
    Node *body = parse_list(parser, stop);
    if (!require_body(parser, body, "empty compound command") ||
        !expect_type(parser, stop.closer, subshell ? ")" : "}")) {
        node_free(body);
        return NULL;
    }
    return single_child(subshell ? NODE_SUBSHELL : NODE_GROUP, body, line);
}

static Node *parse_if(Parser *parser, bool elif)
{
    size_t line = peek(parser)->line;
    if (!expect_word(parser, elif ? "elif" : "if")) {
        return NULL;
    }
    static const char *const condition_stop[] = {"then", NULL};
    static const char *const body_stop[] = {"elif", "else", "fi", NULL};
    static const char *const else_stop[] = {"fi", NULL};
    Stop stop;
    memset(&stop, 0, sizeof(stop));
    stop.words = condition_stop;
    Node *condition = parse_list(parser, stop);
    if (!require_body(parser, condition, "if requires a condition") ||
        !expect_word(parser, "then")) {
        node_free(condition);
        return NULL;
    }
    stop.words = body_stop;
    Node *body = parse_list(parser, stop);
    if (!require_body(parser, body, "then requires a command list")) {
        node_free(condition);
        node_free(body);
        return NULL;
    }
    Node *node = node_new(NODE_IF);
    node->line = line;
    node_add_child(node, condition);
    node_add_child(node, body);
    if (word_is(parser, "elif")) {
        if (++parser->depth >= MISH_MAX_DEPTH) {
            parser_error(parser, "conditional nesting limit exceeded", false);
            node_free(node);
            --parser->depth;
            return NULL;
        }
        Node *alternative = parse_if(parser, true);
        --parser->depth;
        if (alternative == NULL) {
            node_free(node);
            return NULL;
        }
        node_add_child(node, alternative);
        /* The recursive elif owns the final fi token. */
        return node;
    }
    if (word_is(parser, "else")) {
        consume(parser);
        stop.words = else_stop;
        Node *alternative = parse_list(parser, stop);
        if (!require_body(parser, alternative, "else requires a command list")) {
            node_free(alternative);
            node_free(node);
            return NULL;
        }
        node_add_child(node, alternative);
    }
    if (!expect_word(parser, "fi")) {
        node_free(node);
        return NULL;
    }
    return node;
}

static Node *parse_loop(Parser *parser, bool until)
{
    size_t line = peek(parser)->line;
    consume(parser);
    static const char *const condition_stop[] = {"do", NULL};
    static const char *const body_stop[] = {"done", NULL};
    Stop stop;
    memset(&stop, 0, sizeof(stop));
    stop.words = condition_stop;
    Node *condition = parse_list(parser, stop);
    if (!require_body(parser, condition, "loop requires a condition") ||
        !expect_word(parser, "do")) {
        node_free(condition);
        return NULL;
    }
    stop.words = body_stop;
    Node *body = parse_list(parser, stop);
    if (!require_body(parser, body, "loop requires a body") ||
        !expect_word(parser, "done")) {
        node_free(condition);
        node_free(body);
        return NULL;
    }
    Node *node = node_new(until ? NODE_UNTIL : NODE_WHILE);
    node->line = line;
    node_add_child(node, condition);
    node_add_child(node, body);
    return node;
}

static Node *parse_for(Parser *parser)
{
    size_t line = peek(parser)->line;
    consume(parser);
    if (peek(parser)->type == TOK_WORD && strncmp(peek(parser)->text, "((", 2) == 0) {
        const char *raw = peek(parser)->text;
        size_t length = strlen(raw);
        Node *node = node_new(NODE_ARITH_FOR);
        node->line = line;
        size_t start = 2;
        unsigned parentheses = 0;
        for (size_t index = 2; index < length - 2; ++index) {
            if (raw[index] == '(') {
                ++parentheses;
            } else if (raw[index] == ')' && parentheses != 0) {
                --parentheses;
            } else if (raw[index] == ';' && parentheses == 0) {
                strvec_push_owned(&node->words, xstrndup(raw + start, index - start));
                start = index + 1;
            }
        }
        strvec_push_owned(&node->words, xstrndup(raw + start, length - 2 - start));
        if (node->words.len != 3) {
            parser_error(parser, "arithmetic for requires init; condition; step", false);
            node_free(node);
            return NULL;
        }
        consume(parser);
        if (peek(parser)->type == TOK_SEMI) {
            consume(parser);
        }
        skip_newlines(parser);
        if (!expect_word(parser, "do")) {
            node_free(node);
            return NULL;
        }
        static const char *const arithmetic_body_stop[] = {"done", NULL};
        Stop stop;
        memset(&stop, 0, sizeof(stop));
        stop.words = arithmetic_body_stop;
        Node *body = parse_list(parser, stop);
        if (!require_body(parser, body, "arithmetic for requires a body") ||
            !expect_word(parser, "done")) {
            node_free(body);
            node_free(node);
            return NULL;
        }
        node_add_child(node, body);
        return node;
    }
    if (peek(parser)->type != TOK_WORD || !valid_name(peek(parser)->text)) {
        unexpected(parser, "for variable name");
        return NULL;
    }
    Node *node = node_new(NODE_FOR);
    node->line = line;
    node->name = xstrdup(peek(parser)->text);
    consume(parser);
    bool header_newline = peek(parser)->type == TOK_NEWLINE;
    skip_newlines(parser);
    if (word_is(parser, "in")) {
        consume(parser);
        while (peek(parser)->type == TOK_WORD) {
            strvec_push(&node->words, peek(parser)->text);
            consume(parser);
        }
        if (peek(parser)->type != TOK_SEMI && peek(parser)->type != TOK_NEWLINE) {
            unexpected(parser, "separator after for words");
            node_free(node);
            return NULL;
        }
        consume(parser);
        skip_newlines(parser);
    } else {
        strvec_push(&node->words, "\"$@\"");
        if (peek(parser)->type == TOK_SEMI) {
            consume(parser);
            skip_newlines(parser);
        } else if (!header_newline) {
            unexpected(parser, "separator after for variable");
            node_free(node);
            return NULL;
        }
    }
    if (!expect_word(parser, "do")) {
        node_free(node);
        return NULL;
    }
    static const char *const body_stop[] = {"done", NULL};
    Stop stop;
    memset(&stop, 0, sizeof(stop));
    stop.words = body_stop;
    Node *body = parse_list(parser, stop);
    if (!require_body(parser, body, "for requires a body") || !expect_word(parser, "done")) {
        node_free(body);
        node_free(node);
        return NULL;
    }
    node_add_child(node, body);
    return node;
}

static Node *parse_case(Parser *parser)
{
    size_t line = peek(parser)->line;
    consume(parser);
    if (peek(parser)->type != TOK_WORD) {
        unexpected(parser, "case subject");
        return NULL;
    }
    Node *node = node_new(NODE_CASE);
    node->line = line;
    node->name = xstrdup(peek(parser)->text);
    consume(parser);
    skip_newlines(parser);
    if (!expect_word(parser, "in")) {
        node_free(node);
        return NULL;
    }
    skip_newlines(parser);
    CaseArm **tail = &node->cases;
    while (!word_is(parser, "esac")) {
        if (peek(parser)->type == TOK_EOF) {
            unexpected(parser, "esac");
            node_free(node);
            return NULL;
        }
        if (peek(parser)->type == TOK_LPAREN) {
            consume(parser);
        }
        CaseArm *arm = xcalloc(1, sizeof(*arm));
        strvec_init(&arm->patterns);
        for (;;) {
            if (peek(parser)->type != TOK_WORD) {
                unexpected(parser, "case pattern");
                strvec_free(&arm->patterns);
                free(arm);
                node_free(node);
                return NULL;
            }
            strvec_push(&arm->patterns, peek(parser)->text);
            consume(parser);
            if (peek(parser)->type != TOK_PIPE) {
                break;
            }
            consume(parser);
        }
        if (!expect_type(parser, TOK_RPAREN, ") after case patterns")) {
            strvec_free(&arm->patterns);
            free(arm);
            node_free(node);
            return NULL;
        }
        static const char *const arm_stop[] = {"esac", NULL};
        Stop stop;
        memset(&stop, 0, sizeof(stop));
        stop.words = arm_stop;
        stop.case_separator = true;
        arm->body = parse_list(parser, stop);
        if (arm->body == NULL) {
            strvec_free(&arm->patterns);
            free(arm);
            node_free(node);
            return NULL;
        }
        *tail = arm;
        tail = &arm->next;
        if (peek(parser)->type == TOK_DSEMI) {
            consume(parser);
            skip_newlines(parser);
        } else if (!word_is(parser, "esac")) {
            unexpected(parser, ";; or esac");
            node_free(node);
            return NULL;
        }
    }
    consume(parser);
    return node;
}

static bool compound_start(Parser *parser)
{
    TokenType type = peek(parser)->type;
    return type == TOK_LPAREN || type == TOK_LBRACE ||
        word_is(parser, "if") || word_is(parser, "while") ||
        word_is(parser, "until") || word_is(parser, "for") || word_is(parser, "case");
}

static Node *parse_function(Parser *parser, bool keyword)
{
    size_t line = peek(parser)->line;
    if (keyword) {
        consume(parser);
    }
    if (peek(parser)->type != TOK_WORD || !valid_name(peek(parser)->text)) {
        unexpected(parser, "function name");
        return NULL;
    }
    char *name = xstrdup(peek(parser)->text);
    consume(parser);
    if (peek(parser)->type == TOK_LPAREN) {
        consume(parser);
        if (!expect_type(parser, TOK_RPAREN, ") after function name")) {
            free(name);
            return NULL;
        }
    } else if (!keyword) {
        unexpected(parser, "() after function name");
        free(name);
        return NULL;
    }
    skip_newlines(parser);
    if (!compound_start(parser)) {
        unexpected(parser, "compound function body");
        free(name);
        return NULL;
    }
    Node *body = parse_command(parser);
    if (body == NULL) {
        free(name);
        return NULL;
    }
    Node *node = single_child(NODE_FUNCTION, body, line);
    node->name = name;
    return node;
}

static Node *parse_command(Parser *parser)
{
    if (++parser->depth > MISH_MAX_DEPTH) {
        parser_error(parser, "command nesting limit exceeded", false);
        --parser->depth;
        return NULL;
    }
    Node *node = NULL;
    Token *token = peek(parser);
    if (token->type == TOK_WORD && strncmp(token->text, "((", 2) == 0) {
        size_t length = strlen(token->text);
        char *expression = xstrndup(token->text + 2, length - 4);
        node = node_new(NODE_SIMPLE);
        node->line = token->line;
        strvec_push(&node->words, "let");
        strvec_push_owned(&node->words, shell_quote(expression));
        free(expression);
        consume(parser);
    } else if (token->type == TOK_LPAREN) {
        node = parse_group(parser, true);
    } else if (token->type == TOK_LBRACE) {
        node = parse_group(parser, false);
    } else if (word_is(parser, "if")) {
        node = parse_if(parser, false);
    } else if (word_is(parser, "while") || word_is(parser, "until")) {
        node = parse_loop(parser, word_is(parser, "until"));
    } else if (word_is(parser, "for")) {
        node = parse_for(parser);
    } else if (word_is(parser, "case")) {
        node = parse_case(parser);
    } else if (word_is(parser, "function")) {
        node = parse_function(parser, true);
    } else if (token->type == TOK_WORD && valid_name(token->text) &&
               lookahead(parser, 1)->type == TOK_LPAREN && lookahead(parser, 2)->type == TOK_RPAREN) {
        node = parse_function(parser, false);
    } else if (token->type == TOK_WORD && reserved_word(token->text)) {
        parser_error(parser, "unexpected reserved word", false);
    } else {
        node = parse_simple(parser);
    }
    if (node != NULL) {
        while (peek(parser)->type == TOK_REDIR) {
            if (parse_redirection(parser, node) != 0) {
                node_free(node);
                node = NULL;
                break;
            }
        }
    }
    --parser->depth;
    return node;
}

static Node *parse_pipeline(Parser *parser)
{
    bool negate = false;
    size_t line = peek(parser)->line;
    if (word_is(parser, "!")) {
        negate = true;
        consume(parser);
        skip_newlines(parser);
    }
    Node *first = parse_command(parser);
    if (first == NULL) {
        return NULL;
    }
    Node *result = first;
    if (peek(parser)->type == TOK_PIPE) {
        Node *pipeline = node_new(NODE_PIPE);
        pipeline->line = line;
        node_add_child(pipeline, first);
        while (peek(parser)->type == TOK_PIPE) {
            bool stderr_pipe = strcmp(peek(parser)->text, "|&") == 0;
            consume(parser);
            skip_newlines(parser);
            if (stderr_pipe) {
                Node *previous = pipeline->children[pipeline->count - 1];
                append_redirection(previous, new_redirection(2, REDIR_DUP_OUTPUT, "1"));
            }
            Node *next = parse_command(parser);
            if (next == NULL) {
                node_free(pipeline);
                return NULL;
            }
            node_add_child(pipeline, next);
        }
        result = pipeline;
    }
    if (negate) {
        result = single_child(NODE_NOT, result, line);
    }
    return result;
}

static Node *parse_and_or(Parser *parser)
{
    Node *left = parse_pipeline(parser);
    if (left == NULL) {
        return NULL;
    }
    unsigned chain = 0;
    while (peek(parser)->type == TOK_AND || peek(parser)->type == TOK_OR) {
        if (++chain + parser->depth >= MISH_MAX_DEPTH) {
            parser_error(parser, "logical operator nesting limit exceeded", false);
            node_free(left);
            return NULL;
        }
        Token *operator = peek(parser);
        NodeType type = operator->type == TOK_AND ? NODE_AND : NODE_OR;
        size_t line = operator->line;
        consume(parser);
        skip_newlines(parser);
        Node *right = parse_pipeline(parser);
        if (right == NULL) {
            node_free(left);
            return NULL;
        }
        Node *combined = node_new(type);
        combined->line = line;
        node_add_child(combined, left);
        node_add_child(combined, right);
        left = combined;
    }
    return left;
}

static Node *parse_list(Parser *parser, Stop stop)
{
    Node *list = node_new(NODE_LIST);
    list->line = peek(parser)->line;
    skip_newlines(parser);
    while (peek(parser)->type != TOK_EOF && !at_stop(parser, stop)) {
        Node *command = parse_and_or(parser);
        if (command == NULL) {
            node_free(list);
            return NULL;
        }
        TokenType separator = peek(parser)->type;
        if (separator == TOK_AMP) {
            command = single_child(NODE_BACKGROUND, command, command->line);
            consume(parser);
            skip_newlines(parser);
            node_add_child(list, command);
            continue;
        }
        node_add_child(list, command);
        if (separator == TOK_SEMI || separator == TOK_NEWLINE) {
            consume(parser);
            skip_newlines(parser);
            continue;
        }
        if (at_stop(parser, stop)) {
            if (stop.brace_separator && peek(parser)->type == TOK_RBRACE) {
                parser_error(parser, "expected separator before closing brace", false);
                node_free(list);
                return NULL;
            }
            break;
        }
        if (peek(parser)->type == TOK_EOF) {
            break;
        }
        unexpected(parser, "command separator");
        node_free(list);
        return NULL;
    }
    return list;
}

/* Parser recursion alone does not bound a tree assembled by successive
 * left-associative operators. Validate the actual result before handing it to
 * the executor, function registry, or serializer. */
static bool validate_tree(Parser *parser, const Node *node, unsigned depth)
{
    if (node == NULL) {
        parser_error(parser, "missing command tree node", false);
        return false;
    }
    if (depth > MISH_MAX_DEPTH) {
        parser_error(parser, "command tree nesting limit exceeded", false);
        return false;
    }
    size_t minimum = 0;
    size_t maximum = SIZE_MAX;
    bool needs_name = false;
    switch (node->type) {
    case NODE_SIMPLE:
        maximum = 0;
        if (node->words.len == 0 && node->redirs == NULL) {
            parser_error(parser, "empty simple command", false);
            return false;
        }
        break;
    case NODE_LIST:
        break;
    case NODE_AND:
    case NODE_OR:
    case NODE_WHILE:
    case NODE_UNTIL:
        minimum = 2;
        maximum = 2;
        break;
    case NODE_PIPE:
        minimum = 2;
        break;
    case NODE_IF:
        minimum = 2;
        maximum = 3;
        break;
    case NODE_BACKGROUND:
    case NODE_SUBSHELL:
    case NODE_GROUP:
    case NODE_NOT:
        minimum = 1;
        maximum = 1;
        break;
    case NODE_FUNCTION:
    case NODE_FOR:
        minimum = 1;
        maximum = 1;
        needs_name = true;
        break;
    case NODE_ARITH_FOR:
        minimum = 1;
        maximum = 1;
        if (node->words.len != 3) {
            parser_error(parser, "arithmetic loop requires three expressions", false);
            return false;
        }
        break;
    case NODE_CASE:
        maximum = 0;
        needs_name = true;
        break;
    }
    if (node->count < minimum || node->count > maximum) {
        parser_error(parser, "invalid compound command shape", false);
        return false;
    }
    if (needs_name && node->name == NULL) {
        parser_error(parser, "missing compound command name", false);
        return false;
    }
    for (const Redir *redirection = node->redirs; redirection != NULL; redirection = redirection->next) {
        if (redirection->fd < 0 || redirection->word == NULL) {
            parser_error(parser, "invalid redirection metadata", false);
            return false;
        }
        if (redirection->type == REDIR_HEREDOC && redirection->heredoc == NULL) {
            parser_error(parser, "missing here document body", false);
            return false;
        }
    }
    for (size_t index = 0; index < node->count; ++index) {
        if (!validate_tree(parser, node->children[index], depth + 1)) {
            return false;
        }
    }
    for (const CaseArm *arm = node->cases; arm != NULL; arm = arm->next) {
        if (arm->patterns.len == 0 || !validate_tree(parser, arm->body, depth + 1)) {
            if (!parser->failed) {
                parser_error(parser, "empty case pattern list", false);
            }
            return false;
        }
    }
    return true;
}

Node *parse_source(const char *source, ParseError *error)
{
    Parser parser;
    memset(&parser, 0, sizeof(parser));
    parser.error = error;
    if (lex_source(source, &parser.tokens, error) != 0) {
        return NULL;
    }
    Stop stop;
    memset(&stop, 0, sizeof(stop));
    Node *node = parse_list(&parser, stop);
    if (node != NULL && peek(&parser)->type != TOK_EOF) {
        unexpected(&parser, "end of input");
    }
    if (node != NULL && !parser.failed) {
        validate_tree(&parser, node, 0);
    }
    if (parser.failed) {
        node_free(node);
        node = NULL;
    }
    tokens_free(&parser.tokens);
    return node;
}
