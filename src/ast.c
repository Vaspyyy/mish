#include "mish.h"

#include <stdlib.h>
#include <string.h>

Node *node_new(NodeType type)
{
    Node *node = xcalloc(1, sizeof(*node));
    node->type = type;
    strvec_init(&node->words);
    return node;
}

void node_add_child(Node *parent, Node *child)
{
    if (parent == NULL || child == NULL) {
        return;
    }
    if (parent->count == parent->cap) {
        size_t capacity = parent->cap == 0 ? 4 : parent->cap * 2;
        parent->children = xrealloc(parent->children, capacity * sizeof(Node *));
        parent->cap = capacity;
    }
    parent->children[parent->count++] = child;
}

static Redir *redirections_clone(const Redir *source)
{
    Redir *result = NULL;
    Redir **tail = &result;
    for (const Redir *current = source; current != NULL; current = current->next) {
        Redir *copy = xcalloc(1, sizeof(*copy));
        copy->fd = current->fd;
        copy->type = current->type;
        copy->word = current->word == NULL ? NULL : xstrdup(current->word);
        copy->heredoc = current->heredoc == NULL ? NULL : xstrdup(current->heredoc);
        copy->heredoc_expand = current->heredoc_expand;
        *tail = copy;
        tail = &copy->next;
    }
    return result;
}

static void redirections_free(Redir *redirection)
{
    while (redirection != NULL) {
        Redir *next = redirection->next;
        free(redirection->word);
        free(redirection->heredoc);
        free(redirection);
        redirection = next;
    }
}

static CaseArm *case_arms_clone(const CaseArm *source)
{
    CaseArm *result = NULL;
    CaseArm **tail = &result;
    for (const CaseArm *current = source; current != NULL; current = current->next) {
        CaseArm *copy = xcalloc(1, sizeof(*copy));
        strvec_init(&copy->patterns);
        for (size_t index = 0; index < current->patterns.len; ++index) {
            strvec_push(&copy->patterns, current->patterns.items[index]);
        }
        copy->body = node_clone(current->body);
        *tail = copy;
        tail = &copy->next;
    }
    return result;
}

Node *node_clone(const Node *source)
{
    if (source == NULL) {
        return NULL;
    }
    Node *copy = node_new(source->type);
    copy->line = source->line;
    copy->name = source->name == NULL ? NULL : xstrdup(source->name);
    copy->redirs = redirections_clone(source->redirs);
    for (size_t index = 0; index < source->words.len; ++index) {
        strvec_push(&copy->words, source->words.items[index]);
    }
    for (size_t index = 0; index < source->count; ++index) {
        node_add_child(copy, node_clone(source->children[index]));
    }
    copy->cases = case_arms_clone(source->cases);
    return copy;
}

void node_free(Node *node)
{
    if (node == NULL) {
        return;
    }
    size_t capacity = 32;
    size_t count = 1;
    Node **pending = xmalloc(capacity * sizeof(*pending));
    pending[0] = node;
    while (count != 0) {
        Node *current = pending[--count];
        if (current == NULL) {
            continue;
        }
        size_t needed = count + current->count;
        for (CaseArm *arm = current->cases; arm != NULL; arm = arm->next) {
            ++needed;
        }
        while (capacity < needed) {
            capacity *= 2;
        }
        pending = xrealloc(pending, capacity * sizeof(*pending));
        for (size_t index = 0; index < current->count; ++index) {
            pending[count++] = current->children[index];
        }
        free(current->children);
        strvec_free(&current->words);
        redirections_free(current->redirs);
        CaseArm *arm = current->cases;
        while (arm != NULL) {
            CaseArm *next = arm->next;
            pending[count++] = arm->body;
            strvec_free(&arm->patterns);
            free(arm);
            arm = next;
        }
        free(current->name);
        free(current);
    }
    free(pending);
}

static void indentation(FILE *stream, unsigned indent)
{
    for (unsigned index = 0; index < indent; ++index) {
        fputc(' ', stream);
    }
}

static const char *node_type_name(NodeType type)
{
    switch (type) {
    case NODE_SIMPLE:
        return "simple";
    case NODE_LIST:
        return "list";
    case NODE_AND:
        return "and";
    case NODE_OR:
        return "or";
    case NODE_PIPE:
        return "pipeline";
    case NODE_BACKGROUND:
        return "background";
    case NODE_SUBSHELL:
        return "subshell";
    case NODE_GROUP:
        return "group";
    case NODE_IF:
        return "if";
    case NODE_WHILE:
        return "while";
    case NODE_UNTIL:
        return "until";
    case NODE_FOR:
        return "for";
    case NODE_ARITH_FOR:
        return "arithmetic-for";
    case NODE_CASE:
        return "case";
    case NODE_FUNCTION:
        return "function";
    case NODE_NOT:
        return "not";
    }
    return "unknown";
}

static const char *redirection_name(RedirType type)
{
    switch (type) {
    case REDIR_INPUT:
        return "<";
    case REDIR_OUTPUT:
        return ">";
    case REDIR_APPEND:
        return ">>";
    case REDIR_CLOBBER:
        return ">|";
    case REDIR_DUP_INPUT:
        return "<&";
    case REDIR_DUP_OUTPUT:
        return ">&";
    case REDIR_READWRITE:
        return "<>";
    case REDIR_HEREDOC:
        return "<<";
    case REDIR_HERESTRING:
        return "<<<";
    }
    return "?";
}

static void print_escaped(FILE *stream, const char *text)
{
    fputc('"', stream);
    if (text != NULL) {
        for (const unsigned char *position = (const unsigned char *)text;
             *position != '\0'; ++position) {
            switch (*position) {
            case '\n':
                fputs("\\n", stream);
                break;
            case '\r':
                fputs("\\r", stream);
                break;
            case '\t':
                fputs("\\t", stream);
                break;
            case '\\':
                fputs("\\\\", stream);
                break;
            case '"':
                fputs("\\\"", stream);
                break;
            default:
                if (*position < 32 || *position == 127) {
                    fprintf(stream, "\\x%02x", *position);
                } else {
                    fputc(*position, stream);
                }
                break;
            }
        }
    }
    fputc('"', stream);
}

static void print_redirections(const Redir *redirection, FILE *stream, unsigned indent)
{
    for (const Redir *current = redirection; current != NULL; current = current->next) {
        indentation(stream, indent);
        fprintf(stream, "redirect fd=%d operator=%s target=", current->fd,
                redirection_name(current->type));
        print_escaped(stream, current->word);
        if (current->type == REDIR_HEREDOC) {
            fprintf(stream, " expand=%s body=", current->heredoc_expand ? "true" : "false");
            print_escaped(stream, current->heredoc);
        }
        fputc('\n', stream);
    }
}

static void print_tree(const Node *node, FILE *stream, unsigned indent, unsigned depth)
{
    if (node == NULL) {
        indentation(stream, indent);
        fputs("empty\n", stream);
        return;
    }
    if (depth > MISH_MAX_DEPTH) {
        indentation(stream, indent);
        fputs("<depth limit>\n", stream);
        return;
    }
    indentation(stream, indent);
    fprintf(stream, "%s line=%zu", node_type_name(node->type), node->line);
    if (node->name != NULL) {
        fputs(" name=", stream);
        print_escaped(stream, node->name);
    }
    if (node->words.len != 0) {
        fputs(" words=[", stream);
        for (size_t index = 0; index < node->words.len; ++index) {
            if (index != 0) {
                fputs(", ", stream);
            }
            print_escaped(stream, node->words.items[index]);
        }
        fputc(']', stream);
    }
    fputc('\n', stream);
    print_redirections(node->redirs, stream, indent + 2);
    for (size_t index = 0; index < node->count; ++index) {
        print_tree(node->children[index], stream, indent + 2, depth + 1);
    }
    for (const CaseArm *arm = node->cases; arm != NULL; arm = arm->next) {
        indentation(stream, indent + 2);
        fputs("case-arm patterns=[", stream);
        for (size_t index = 0; index < arm->patterns.len; ++index) {
            if (index != 0) {
                fputs(", ", stream);
            }
            print_escaped(stream, arm->patterns.items[index]);
        }
        fputs("]\n", stream);
        print_tree(arm->body, stream, indent + 4, depth + 1);
    }
}

void node_print(const Node *node, FILE *stream, unsigned indent)
{
    if (stream == NULL) {
        return;
    }
    print_tree(node, stream, indent, 0);
}

/* Canonical serialization is separate from the debug tree printer. Heredocs
 * are deferred until the header's newline, preserving pipelines and logical
 * operators while retaining a function definition's raw expansion syntax. */
typedef struct SourceDocument {
    char *delimiter;
    char *body;
    struct SourceDocument *next;
} SourceDocument;

typedef struct {
    Buffer output;
    SourceDocument *documents;
    unsigned delimiter_number;
    unsigned depth;
    unsigned indent;
    bool failed;
} SourceWriter;

static void source_node(SourceWriter *writer, const Node *node);

static void source_indentation(SourceWriter *writer)
{
    for (unsigned index = 0; index < writer->indent; ++index) {
        buffer_append(&writer->output, "    ");
    }
}

static bool delimiter_in_body(const char *body, const char *delimiter)
{
    size_t length = strlen(delimiter);
    const char *line = body;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        if ((size_t)(end - line) == length && memcmp(line, delimiter, length) == 0) {
            return true;
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
    return false;
}

static char *source_delimiter(SourceWriter *writer, const char *body)
{
    for (;;) {
        char *candidate = str_printf("__MISH_DOCUMENT_%u__", writer->delimiter_number++);
        if (!delimiter_in_body(body, candidate)) {
            return candidate;
        }
        free(candidate);
    }
}

static void source_queue_document(SourceWriter *writer, const Redir *redirection)
{
    SourceDocument *document = xcalloc(1, sizeof(*document));
    document->body = xstrdup(redirection->heredoc == NULL ? "" : redirection->heredoc);
    document->delimiter = source_delimiter(writer, document->body);
    if (redirection->heredoc_expand) {
        buffer_append(&writer->output, document->delimiter);
    } else {
        buffer_putc(&writer->output, '\'');
        buffer_append(&writer->output, document->delimiter);
        buffer_putc(&writer->output, '\'');
    }
    SourceDocument **tail = &writer->documents;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = document;
}

static void source_documents(SourceWriter *writer)
{
    while (writer->documents != NULL) {
        SourceDocument *document = writer->documents;
        writer->documents = document->next;
        buffer_append(&writer->output, document->body);
        size_t length = strlen(document->body);
        if (length != 0 && document->body[length - 1] != '\n') {
            buffer_putc(&writer->output, '\n');
        }
        buffer_append(&writer->output, document->delimiter);
        buffer_putc(&writer->output, '\n');
        free(document->body);
        free(document->delimiter);
        free(document);
    }
}

static void source_newline(SourceWriter *writer)
{
    buffer_putc(&writer->output, '\n');
    source_documents(writer);
}

static void source_redirections(SourceWriter *writer, const Redir *redirection)
{
    for (const Redir *current = redirection; current != NULL; current = current->next) {
        char *descriptor = str_printf(" %d", current->fd);
        buffer_append(&writer->output, descriptor);
        free(descriptor);
        buffer_append(&writer->output, redirection_name(current->type));
        buffer_putc(&writer->output, ' ');
        if (current->type == REDIR_HEREDOC) {
            source_queue_document(writer, current);
        } else if (current->word != NULL) {
            buffer_append(&writer->output, current->word);
        }
    }
}

static void source_list(SourceWriter *writer, const Node *node)
{
    if (node == NULL) {
        writer->failed = true;
        return;
    }
    if (node->type != NODE_LIST) {
        source_indentation(writer);
        source_node(writer, node);
        source_newline(writer);
        return;
    }
    for (size_t index = 0; index < node->count; ++index) {
        source_indentation(writer);
        source_node(writer, node->children[index]);
        source_newline(writer);
    }
}

static void source_binary(SourceWriter *writer, const Node *node, const char *operator)
{
    if (node->count != 2) {
        writer->failed = true;
        return;
    }
    source_node(writer, node->children[0]);
    buffer_putc(&writer->output, ' ');
    buffer_append(&writer->output, operator);
    buffer_putc(&writer->output, ' ');
    source_node(writer, node->children[1]);
}

static void source_group(SourceWriter *writer, const Node *node, bool subshell)
{
    if (node->count != 1) {
        writer->failed = true;
        return;
    }
    buffer_append(&writer->output, subshell ? "(" : "{");
    source_newline(writer);
    ++writer->indent;
    source_list(writer, node->children[0]);
    --writer->indent;
    source_indentation(writer);
    buffer_append(&writer->output, subshell ? ")" : "}");
}

static void source_condition(SourceWriter *writer, const Node *node)
{
    if (node->count < 2 || node->count > 3) {
        writer->failed = true;
        return;
    }
    buffer_append(&writer->output, "if");
    source_newline(writer);
    ++writer->indent;
    source_list(writer, node->children[0]);
    --writer->indent;
    source_indentation(writer);
    buffer_append(&writer->output, "then");
    source_newline(writer);
    ++writer->indent;
    source_list(writer, node->children[1]);
    --writer->indent;
    if (node->count == 3) {
        source_indentation(writer);
        buffer_append(&writer->output, "else");
        source_newline(writer);
        ++writer->indent;
        source_list(writer, node->children[2]);
        --writer->indent;
    }
    source_indentation(writer);
    buffer_append(&writer->output, "fi");
}

static void source_loop(SourceWriter *writer, const Node *node, bool until)
{
    if (node->count != 2) {
        writer->failed = true;
        return;
    }
    buffer_append(&writer->output, until ? "until" : "while");
    source_newline(writer);
    ++writer->indent;
    source_list(writer, node->children[0]);
    --writer->indent;
    source_indentation(writer);
    buffer_append(&writer->output, "do");
    source_newline(writer);
    ++writer->indent;
    source_list(writer, node->children[1]);
    --writer->indent;
    source_indentation(writer);
    buffer_append(&writer->output, "done");
}

static void source_for(SourceWriter *writer, const Node *node)
{
    if (node->count != 1 || node->name == NULL) {
        writer->failed = true;
        return;
    }
    buffer_append(&writer->output, "for ");
    buffer_append(&writer->output, node->name);
    buffer_append(&writer->output, " in");
    for (size_t index = 0; index < node->words.len; ++index) {
        buffer_putc(&writer->output, ' ');
        buffer_append(&writer->output, node->words.items[index]);
    }
    buffer_append(&writer->output, "; do");
    source_newline(writer);
    ++writer->indent;
    source_list(writer, node->children[0]);
    --writer->indent;
    source_indentation(writer);
    buffer_append(&writer->output, "done");
}

static void source_arithmetic_for(SourceWriter *writer, const Node *node)
{
    if (node->count != 1 || node->words.len != 3) {
        writer->failed = true;
        return;
    }
    buffer_append(&writer->output, "for ((");
    for (size_t index = 0; index < 3; ++index) {
        if (index != 0) {
            buffer_append(&writer->output, "; ");
        }
        buffer_append(&writer->output, node->words.items[index]);
    }
    buffer_append(&writer->output, ")); do");
    source_newline(writer);
    ++writer->indent;
    source_list(writer, node->children[0]);
    --writer->indent;
    source_indentation(writer);
    buffer_append(&writer->output, "done");
}

static void source_case(SourceWriter *writer, const Node *node)
{
    if (node->name == NULL) {
        writer->failed = true;
        return;
    }
    buffer_append(&writer->output, "case ");
    buffer_append(&writer->output, node->name);
    buffer_append(&writer->output, " in");
    source_newline(writer);
    ++writer->indent;
    for (const CaseArm *arm = node->cases; arm != NULL; arm = arm->next) {
        source_indentation(writer);
        buffer_putc(&writer->output, '(');
        for (size_t index = 0; index < arm->patterns.len; ++index) {
            if (index != 0) {
                buffer_putc(&writer->output, '|');
            }
            buffer_append(&writer->output, arm->patterns.items[index]);
        }
        buffer_putc(&writer->output, ')');
        source_newline(writer);
        ++writer->indent;
        source_list(writer, arm->body);
        --writer->indent;
        source_indentation(writer);
        buffer_append(&writer->output, ";;");
        source_newline(writer);
    }
    --writer->indent;
    source_indentation(writer);
    buffer_append(&writer->output, "esac");
}

static void source_node(SourceWriter *writer, const Node *node)
{
    if (writer->failed || node == NULL) {
        writer->failed = true;
        return;
    }
    if (++writer->depth > MISH_MAX_DEPTH) {
        writer->failed = true;
        --writer->depth;
        return;
    }
    switch (node->type) {
    case NODE_SIMPLE:
        for (size_t index = 0; index < node->words.len; ++index) {
            if (index != 0) {
                buffer_putc(&writer->output, ' ');
            }
            buffer_append(&writer->output, node->words.items[index]);
        }
        break;
    case NODE_LIST:
        source_list(writer, node);
        break;
    case NODE_AND:
        source_binary(writer, node, "&&");
        break;
    case NODE_OR:
        source_binary(writer, node, "||");
        break;
    case NODE_PIPE:
        for (size_t index = 0; index < node->count; ++index) {
            if (index != 0) {
                buffer_append(&writer->output, " | ");
            }
            source_node(writer, node->children[index]);
        }
        break;
    case NODE_BACKGROUND:
        if (node->count == 1) {
            source_node(writer, node->children[0]);
            buffer_append(&writer->output, " &");
        } else {
            writer->failed = true;
        }
        break;
    case NODE_SUBSHELL:
        source_group(writer, node, true);
        break;
    case NODE_GROUP:
        source_group(writer, node, false);
        break;
    case NODE_IF:
        source_condition(writer, node);
        break;
    case NODE_WHILE:
        source_loop(writer, node, false);
        break;
    case NODE_UNTIL:
        source_loop(writer, node, true);
        break;
    case NODE_FOR:
        source_for(writer, node);
        break;
    case NODE_ARITH_FOR:
        source_arithmetic_for(writer, node);
        break;
    case NODE_CASE:
        source_case(writer, node);
        break;
    case NODE_FUNCTION:
        if (node->name != NULL && node->count == 1) {
            buffer_append(&writer->output, node->name);
            buffer_append(&writer->output, "() ");
            source_node(writer, node->children[0]);
        } else {
            writer->failed = true;
        }
        break;
    case NODE_NOT:
        if (node->count == 1) {
            buffer_append(&writer->output, "! ");
            source_node(writer, node->children[0]);
        } else {
            writer->failed = true;
        }
        break;
    }
    source_redirections(writer, node->redirs);
    --writer->depth;
}

char *node_source(const Node *node)
{
    SourceWriter writer;
    memset(&writer, 0, sizeof(writer));
    buffer_init(&writer.output);
    if (node != NULL && node->type == NODE_LIST) {
        source_list(&writer, node);
    } else {
        source_node(&writer, node);
        source_newline(&writer);
    }
    if (writer.documents != NULL) {
        source_newline(&writer);
    }
    if (writer.failed) {
        buffer_free(&writer.output);
        return NULL;
    }
    return buffer_take(&writer.output);
}
