#include "mish.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Parsing into a small expression tree separates syntax validation from
 * execution. Skipped logical and ternary operands therefore have no effects. */
typedef enum {
    AR_END,
    AR_NUMBER,
    AR_NAME,
    AR_OPERATOR,
    AR_LPAREN,
    AR_RPAREN,
    AR_QUESTION,
    AR_COLON
} ArithmeticTokenType;

typedef enum {
    OP_NONE,
    OP_COMMA,
    OP_ASSIGN,
    OP_ADD_ASSIGN,
    OP_SUB_ASSIGN,
    OP_MUL_ASSIGN,
    OP_DIV_ASSIGN,
    OP_MOD_ASSIGN,
    OP_SHL_ASSIGN,
    OP_SHR_ASSIGN,
    OP_AND_ASSIGN,
    OP_XOR_ASSIGN,
    OP_OR_ASSIGN,
    OP_POW_ASSIGN,
    OP_LOGICAL_OR,
    OP_LOGICAL_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_BIT_AND,
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    OP_LEFT_SHIFT,
    OP_RIGHT_SHIFT,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_POWER,
    OP_NOT,
    OP_COMPLEMENT,
    OP_INCREMENT,
    OP_DECREMENT
} ArithmeticOperator;

typedef enum {
    EX_LITERAL,
    EX_VARIABLE,
    EX_UNARY,
    EX_BINARY,
    EX_TERNARY,
    EX_POSTFIX
} ExpressionType;

typedef struct Expression Expression;
struct Expression {
    ExpressionType type;
    ArithmeticOperator operator;
    int64_t number;
    bool wide_literal;
    char *name;
    size_t offset;
    Expression *first;
    Expression *second;
    Expression *third;
};

typedef struct {
    ArithmeticTokenType type;
    ArithmeticOperator operator;
    char *text;
    int64_t number;
    bool wide_literal;
    size_t offset;
} ArithmeticToken;

typedef struct {
    const char *source;
    size_t position;
    ArithmeticToken token;
    char *error;
    unsigned depth;
    size_t nodes;
} ArithmeticParser;

typedef struct {
    Shell *shell;
    char *error;
    unsigned depth;
    const char *variables[MISH_MAX_DEPTH];
    size_t variable_count;
    size_t evaluation_steps;
    size_t parsed_bytes;
} Evaluation;

typedef struct {
    const char *spelling;
    ArithmeticOperator operator;
} OperatorSpelling;

static Expression *parse_expression(ArithmeticParser *parser, unsigned minimum);
static int evaluate(Evaluation *evaluation, const Expression *expression, int64_t *result);
static int evaluate_source(Evaluation *evaluation, const char *source, int64_t *result);

static void arithmetic_error(ArithmeticParser *parser, const char *message, size_t offset)
{
    if (parser->error == NULL) {
        parser->error = str_printf("%s at arithmetic byte %zu", message, offset + 1);
    }
}

static int evaluation_error(Evaluation *evaluation, const Expression *expression,
                          const char *message)
{
    if (evaluation->error == NULL) {
        size_t offset = expression == NULL ? 0 : expression->offset;
        evaluation->error = str_printf("%s at arithmetic byte %zu", message, offset + 1);
    }
    return -1;
}

static int digit_value(unsigned char character, unsigned base)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'Z') {
        return character - 'A' + (base <= 36 ? 10 : 36);
    }
    if (character == '@') {
        return 62;
    }
    if (character == '_') {
        return 63;
    }
    return -1;
}

static int parse_literal(ArithmeticParser *parser, const char *text,
                         int64_t *number, bool *wide)
{
    unsigned base = 10;
    const char *digits = text;
    const char *hash = strchr(text, '#');
    if (hash != NULL) {
        if (hash == text) {
            arithmetic_error(parser, "missing numeric base", parser->token.offset);
            return -1;
        }
        unsigned selected = 0;
        for (const char *position = text; position < hash; ++position) {
            if (!isdigit((unsigned char)*position) || selected > 64) {
                arithmetic_error(parser, "invalid numeric base", parser->token.offset);
                return -1;
            }
            selected = selected * 10 + (unsigned)(*position - '0');
        }
        if (selected < 2 || selected > 64) {
            arithmetic_error(parser, "numeric base must be between 2 and 64", parser->token.offset);
            return -1;
        }
        base = selected;
        digits = hash + 1;
    } else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        digits = text + 2;
    } else if (text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        digits = text + 2;
    } else if (text[0] == '0' && text[1] != '\0') {
        base = 8;
        digits = text + 1;
    }
    if (*digits == '\0') {
        arithmetic_error(parser, "numeric literal has no digits", parser->token.offset);
        return -1;
    }
    uint64_t magnitude = 0;
    const uint64_t limit = UINT64_C(1) << 63;
    for (const unsigned char *position = (const unsigned char *)digits;
         *position != '\0'; ++position) {
        int digit = digit_value(*position, base);
        if (digit < 0 || (unsigned)digit >= base) {
            arithmetic_error(parser, "invalid digit in numeric literal", parser->token.offset);
            return -1;
        }
        if (magnitude > (limit - (unsigned)digit) / base) {
            arithmetic_error(parser, "numeric literal exceeds signed 64-bit range", parser->token.offset);
            return -1;
        }
        magnitude = magnitude * base + (unsigned)digit;
    }
    *wide = magnitude == limit;
    *number = *wide ? INT64_MIN : (int64_t)magnitude;
    return 0;
}

static void token_clear(ArithmeticToken *token)
{
    free(token->text);
    memset(token, 0, sizeof(*token));
}

static int next_token(ArithmeticParser *parser)
{
    static const OperatorSpelling operators[] = {
        {"<<=", OP_SHL_ASSIGN},
        {">>=", OP_SHR_ASSIGN},
        {"**=", OP_POW_ASSIGN},
        {"++", OP_INCREMENT},
        {"--", OP_DECREMENT},
        {"+=", OP_ADD_ASSIGN},
        {"-=", OP_SUB_ASSIGN},
        {"*=", OP_MUL_ASSIGN},
        {"/=", OP_DIV_ASSIGN},
        {"%=", OP_MOD_ASSIGN},
        {"&=", OP_AND_ASSIGN},
        {"^=", OP_XOR_ASSIGN},
        {"|=", OP_OR_ASSIGN},
        {"||", OP_LOGICAL_OR},
        {"&&", OP_LOGICAL_AND},
        {"==", OP_EQUAL},
        {"!=", OP_NOT_EQUAL},
        {"<=", OP_LESS_EQUAL},
        {">=", OP_GREATER_EQUAL},
        {"<<", OP_LEFT_SHIFT},
        {">>", OP_RIGHT_SHIFT},
        {"**", OP_POWER},
        {",", OP_COMMA},
        {"=", OP_ASSIGN},
        {"|", OP_BIT_OR},
        {"^", OP_BIT_XOR},
        {"&", OP_BIT_AND},
        {"<", OP_LESS},
        {">", OP_GREATER},
        {"+", OP_ADD},
        {"-", OP_SUBTRACT},
        {"*", OP_MULTIPLY},
        {"/", OP_DIVIDE},
        {"%", OP_MODULO},
        {"!", OP_NOT},
        {"~", OP_COMPLEMENT}
    };
    token_clear(&parser->token);
    while (isspace((unsigned char)parser->source[parser->position])) {
        ++parser->position;
    }
    size_t start = parser->position;
    parser->token.offset = start;
    char character = parser->source[start];
    if (character == '\0') {
        parser->token.type = AR_END;
        return 0;
    }
    if (character == '(' || character == ')' || character == '?' || character == ':') {
        parser->token.type = character == '(' ? AR_LPAREN : character == ')' ? AR_RPAREN :
            character == '?' ? AR_QUESTION : AR_COLON;
        ++parser->position;
        return 0;
    }
    if (isdigit((unsigned char)character)) {
        ++parser->position;
        while (isalnum((unsigned char)parser->source[parser->position]) ||
               strchr("#@_", parser->source[parser->position]) != NULL) {
            if (parser->source[parser->position] == '\0') {
                break;
            }
            ++parser->position;
        }
        parser->token.type = AR_NUMBER;
        parser->token.text = xstrndup(parser->source + start, parser->position - start);
        return parse_literal(parser, parser->token.text, &parser->token.number,
                             &parser->token.wide_literal);
    }
    bool dollar = character == '$';
    bool braced = dollar && parser->source[start + 1] == '{';
    size_t name_start = start + (dollar ? (braced ? 2 : 1) : 0);
    unsigned char first = (unsigned char)parser->source[name_start];
    if (isalpha(first) || first == '_') {
        parser->position = name_start + 1;
        while (isalnum((unsigned char)parser->source[parser->position]) ||
               parser->source[parser->position] == '_') {
            ++parser->position;
        }
        parser->token.type = AR_NAME;
        parser->token.text = xstrndup(parser->source + name_start, parser->position - name_start);
        if (braced) {
            if (parser->source[parser->position] != '}') {
                arithmetic_error(parser, "expected } after arithmetic variable", parser->position);
                return -1;
            }
            ++parser->position;
        }
        return 0;
    }
    for (size_t index = 0; index < sizeof(operators) / sizeof(operators[0]); ++index) {
        size_t length = strlen(operators[index].spelling);
        if (strncmp(parser->source + start, operators[index].spelling, length) == 0) {
            parser->token.type = AR_OPERATOR;
            parser->token.operator = operators[index].operator;
            parser->position += length;
            return 0;
        }
    }
    arithmetic_error(parser, "unexpected arithmetic character", start);
    return -1;
}

static Expression *expression_new(ArithmeticParser *parser, ExpressionType type,
                                  size_t offset)
{
    if (++parser->nodes > MISH_MAX_SOURCE / sizeof(Expression)) {
        arithmetic_error(parser, "arithmetic expression node limit exceeded", offset);
        return NULL;
    }
    Expression *expression = xcalloc(1, sizeof(*expression));
    expression->type = type;
    expression->offset = offset;
    return expression;
}

static void expression_free(Expression *expression)
{
    if (expression == NULL) {
        return;
    }
    size_t count = 1;
    size_t capacity = 32;
    Expression **pending = xmalloc(capacity * sizeof(*pending));
    pending[0] = expression;
    while (count != 0) {
        Expression *current = pending[--count];
        if (count + 3 > capacity) {
            capacity *= 2;
            pending = xrealloc(pending, capacity * sizeof(*pending));
        }
        if (current->first != NULL) {
            pending[count++] = current->first;
        }
        if (current->second != NULL) {
            pending[count++] = current->second;
        }
        if (current->third != NULL) {
            pending[count++] = current->third;
        }
        free(current->name);
        free(current);
    }
    free(pending);
}

static unsigned precedence(ArithmeticOperator operator)
{
    switch (operator) {
    case OP_COMMA:
        return 1;
    case OP_ASSIGN:
    case OP_ADD_ASSIGN:
    case OP_SUB_ASSIGN:
    case OP_MUL_ASSIGN:
    case OP_DIV_ASSIGN:
    case OP_MOD_ASSIGN:
    case OP_SHL_ASSIGN:
    case OP_SHR_ASSIGN:
    case OP_AND_ASSIGN:
    case OP_XOR_ASSIGN:
    case OP_OR_ASSIGN:
    case OP_POW_ASSIGN:
        return 2;
    case OP_LOGICAL_OR:
        return 4;
    case OP_LOGICAL_AND:
        return 5;
    case OP_BIT_OR:
        return 6;
    case OP_BIT_XOR:
        return 7;
    case OP_BIT_AND:
        return 8;
    case OP_EQUAL:
    case OP_NOT_EQUAL:
        return 9;
    case OP_LESS:
    case OP_LESS_EQUAL:
    case OP_GREATER:
    case OP_GREATER_EQUAL:
        return 10;
    case OP_LEFT_SHIFT:
    case OP_RIGHT_SHIFT:
        return 11;
    case OP_ADD:
    case OP_SUBTRACT:
        return 12;
    case OP_MULTIPLY:
    case OP_DIVIDE:
    case OP_MODULO:
        return 13;
    case OP_POWER:
        return 15;
    default:
        return 0;
    }
}

static bool assignment_operator(ArithmeticOperator operator)
{
    return operator >= OP_ASSIGN && operator <= OP_POW_ASSIGN;
}

static bool right_associative(ArithmeticOperator operator)
{
    return assignment_operator(operator) || operator == OP_POWER;
}

static bool variable_target(ArithmeticParser *parser, const Expression *expression)
{
    if (expression != NULL && expression->type == EX_VARIABLE) {
        return true;
    }
    arithmetic_error(parser, "assignment requires a variable target",
                     expression == NULL ? parser->token.offset : expression->offset);
    return false;
}

static Expression *parse_prefix(ArithmeticParser *parser)
{
    ArithmeticToken *token = &parser->token;
    Expression *expression = NULL;
    if (token->type == AR_NUMBER) {
        expression = expression_new(parser, EX_LITERAL, token->offset);
        if (expression != NULL) {
            expression->number = token->number;
            expression->wide_literal = token->wide_literal;
        }
        if (next_token(parser) != 0) {
            expression_free(expression);
            return NULL;
        }
    } else if (token->type == AR_NAME) {
        expression = expression_new(parser, EX_VARIABLE, token->offset);
        if (expression != NULL) {
            expression->name = xstrdup(token->text);
        }
        if (next_token(parser) != 0) {
            expression_free(expression);
            return NULL;
        }
    } else if (token->type == AR_LPAREN) {
        if (next_token(parser) != 0) {
            return NULL;
        }
        expression = parse_expression(parser, 1);
        if (expression == NULL) {
            return NULL;
        }
        if (parser->token.type != AR_RPAREN) {
            arithmetic_error(parser, "expected )", parser->token.offset);
            expression_free(expression);
            return NULL;
        }
        if (next_token(parser) != 0) {
            expression_free(expression);
            return NULL;
        }
    } else if (token->type == AR_OPERATOR &&
               (token->operator == OP_ADD || token->operator == OP_SUBTRACT ||
                token->operator == OP_NOT || token->operator == OP_COMPLEMENT ||
                token->operator == OP_INCREMENT || token->operator == OP_DECREMENT)) {
        ArithmeticOperator operator = token->operator;
        size_t offset = token->offset;
        if (next_token(parser) != 0) {
            return NULL;
        }
        /* Shell unary operators bind more tightly than exponentiation. */
        Expression *operand = parse_expression(parser, 16);
        if (operand == NULL) {
            return NULL;
        }
        if ((operator == OP_INCREMENT || operator == OP_DECREMENT) &&
            !variable_target(parser, operand)) {
            expression_free(operand);
            return NULL;
        }
        expression = expression_new(parser, EX_UNARY, offset);
        if (expression == NULL) {
            expression_free(operand);
            return NULL;
        }
        expression->operator = operator;
        expression->first = operand;
    } else {
        arithmetic_error(parser, "expected arithmetic operand", token->offset);
        return NULL;
    }
    while (expression != NULL && parser->token.type == AR_OPERATOR &&
           (parser->token.operator == OP_INCREMENT || parser->token.operator == OP_DECREMENT)) {
        if (!variable_target(parser, expression)) {
            expression_free(expression);
            return NULL;
        }
        Expression *postfix = expression_new(parser, EX_POSTFIX, parser->token.offset);
        if (postfix == NULL) {
            expression_free(expression);
            return NULL;
        }
        postfix->operator = parser->token.operator;
        postfix->first = expression;
        expression = postfix;
        if (next_token(parser) != 0) {
            expression_free(expression);
            return NULL;
        }
    }
    return expression;
}

static Expression *parse_expression(ArithmeticParser *parser, unsigned minimum)
{
    if (++parser->depth > MISH_MAX_DEPTH) {
        arithmetic_error(parser, "arithmetic nesting limit exceeded", parser->token.offset);
        --parser->depth;
        return NULL;
    }
    Expression *left = parse_prefix(parser);
    if (left == NULL) {
        --parser->depth;
        return NULL;
    }
    unsigned chain = 0;
    while (parser->error == NULL) {
        if (parser->token.type == AR_QUESTION && minimum <= 3) {
            size_t offset = parser->token.offset;
            if (next_token(parser) != 0) {
                break;
            }
            Expression *middle = parse_expression(parser, 1);
            if (middle == NULL) {
                break;
            }
            if (parser->token.type != AR_COLON) {
                arithmetic_error(parser, "expected : in conditional expression", parser->token.offset);
                expression_free(middle);
                break;
            }
            if (next_token(parser) != 0) {
                expression_free(middle);
                break;
            }
            Expression *right = parse_expression(parser, 2);
            if (right == NULL) {
                expression_free(middle);
                break;
            }
            Expression *conditional = expression_new(parser, EX_TERNARY, offset);
            if (conditional == NULL) {
                expression_free(middle);
                expression_free(right);
                break;
            }
            conditional->first = left;
            conditional->second = middle;
            conditional->third = right;
            left = conditional;
            continue;
        }
        if (parser->token.type != AR_OPERATOR) {
            break;
        }
        ArithmeticOperator operator = parser->token.operator;
        unsigned power = precedence(operator);
        if (power == 0 || power < minimum) {
            break;
        }
        if (++chain + parser->depth > MISH_MAX_DEPTH) {
            arithmetic_error(parser, "arithmetic operator chain limit exceeded", parser->token.offset);
            break;
        }
        if (assignment_operator(operator) && !variable_target(parser, left)) {
            break;
        }
        size_t offset = parser->token.offset;
        if (next_token(parser) != 0) {
            break;
        }
        Expression *right = parse_expression(parser, power + (right_associative(operator) ? 0 : 1));
        if (right == NULL) {
            break;
        }
        Expression *binary = expression_new(parser, EX_BINARY, offset);
        if (binary == NULL) {
            expression_free(right);
            break;
        }
        binary->operator = operator;
        binary->first = left;
        binary->second = right;
        left = binary;
    }
    --parser->depth;
    if (parser->error != NULL) {
        expression_free(left);
        return NULL;
    }
    return left;
}

static int checked_add(int64_t left, int64_t right, int64_t *result)
{
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
        return -1;
    }
    *result = left + right;
    return 0;
}

static int checked_subtract(int64_t left, int64_t right, int64_t *result)
{
    if ((right > 0 && left < INT64_MIN + right) ||
        (right < 0 && left > INT64_MAX + right)) {
        return -1;
    }
    *result = left - right;
    return 0;
}

static uint64_t unsigned_magnitude(int64_t value)
{
    if (value >= 0) {
        return (uint64_t)value;
    }
    return (uint64_t)(-(value + 1)) + 1;
}

static int checked_multiply(int64_t left, int64_t right, int64_t *result)
{
    bool negative = (left < 0) != (right < 0);
    uint64_t left_magnitude = unsigned_magnitude(left);
    uint64_t right_magnitude = unsigned_magnitude(right);
    uint64_t limit = negative ? UINT64_C(1) << 63 : INT64_MAX;
    if (right_magnitude != 0 && left_magnitude > limit / right_magnitude) {
        return -1;
    }
    uint64_t product = left_magnitude * right_magnitude;
    if (negative) {
        *result = product == (UINT64_C(1) << 63) ? INT64_MIN : -(int64_t)product;
    } else {
        *result = (int64_t)product;
    }
    return 0;
}

static int checked_power(int64_t base, int64_t exponent, int64_t *result)
{
    if (exponent < 0) {
        return -2;
    }
    int64_t product = 1;
    uint64_t remaining = (uint64_t)exponent;
    while (remaining != 0) {
        if ((remaining & 1) != 0 && checked_multiply(product, base, &product) != 0) {
            return -1;
        }
        remaining >>= 1;
        if (remaining != 0 && checked_multiply(base, base, &base) != 0) {
            return -1;
        }
    }
    *result = product;
    return 0;
}

static int checked_left_shift(int64_t value, int64_t count, int64_t *result)
{
    if (count < 0 || count >= 64) {
        return -2;
    }
    if (count == 0) {
        *result = value;
        return 0;
    }
    bool negative = value < 0;
    uint64_t magnitude = unsigned_magnitude(value);
    uint64_t limit = negative ? UINT64_C(1) << 63 : INT64_MAX;
    if (magnitude > (limit >> (unsigned)count)) {
        return -1;
    }
    uint64_t shifted = magnitude << (unsigned)count;
    *result = negative ? (shifted == (UINT64_C(1) << 63) ? INT64_MIN : -(int64_t)shifted) : (int64_t)shifted;
    return 0;
}

static int checked_right_shift(int64_t value, int64_t count, int64_t *result)
{
    if (count < 0 || count >= 64) {
        return -1;
    }
    /* Specify arithmetic right shift independently of the compiler's rule
     * for signed negative values. Unsigned shifts are always defined. */
    uint64_t shifted = (uint64_t)value >> (unsigned)count;
    if (value < 0 && count != 0) {
        shifted |= UINT64_MAX << (64 - (unsigned)count);
    }
    if ((shifted & (UINT64_C(1) << 63)) != 0) {
        *result = INT64_MIN + (int64_t)(shifted & INT64_MAX);
    } else {
        *result = (int64_t)shifted;
    }
    return 0;
}

static int64_t signed_bits(uint64_t value)
{
    if ((value & (UINT64_C(1) << 63)) != 0) {
        return INT64_MIN + (int64_t)(value & INT64_MAX);
    }
    return (int64_t)value;
}

static ArithmeticOperator underlying_operator(ArithmeticOperator operator)
{
    switch (operator) {
    case OP_ADD_ASSIGN:
        return OP_ADD;
    case OP_SUB_ASSIGN:
        return OP_SUBTRACT;
    case OP_MUL_ASSIGN:
        return OP_MULTIPLY;
    case OP_DIV_ASSIGN:
        return OP_DIVIDE;
    case OP_MOD_ASSIGN:
        return OP_MODULO;
    case OP_SHL_ASSIGN:
        return OP_LEFT_SHIFT;
    case OP_SHR_ASSIGN:
        return OP_RIGHT_SHIFT;
    case OP_AND_ASSIGN:
        return OP_BIT_AND;
    case OP_XOR_ASSIGN:
        return OP_BIT_XOR;
    case OP_OR_ASSIGN:
        return OP_BIT_OR;
    case OP_POW_ASSIGN:
        return OP_POWER;
    default:
        return operator;
    }
}

static int apply_binary(Evaluation *evaluation, const Expression *expression,
                        ArithmeticOperator operator, int64_t left, int64_t right,
                        int64_t *result)
{
    int status = 0;
    switch (operator) {
    case OP_ADD:
        status = checked_add(left, right, result);
        break;
    case OP_SUBTRACT:
        status = checked_subtract(left, right, result);
        break;
    case OP_MULTIPLY:
        status = checked_multiply(left, right, result);
        break;
    case OP_DIVIDE:
    case OP_MODULO:
        if (right == 0) {
            return evaluation_error(evaluation, expression, "division by zero");
        }
        if (left == INT64_MIN && right == -1) {
            if (operator == OP_MODULO) {
                *result = 0;
                return 0;
            }
            status = -1;
        } else {
            *result = operator == OP_DIVIDE ? left / right : left % right;
        }
        break;
    case OP_POWER:
        status = checked_power(left, right, result);
        if (status == -2) {
            return evaluation_error(evaluation, expression, "negative exponent");
        }
        break;
    case OP_LEFT_SHIFT:
        status = checked_left_shift(left, right, result);
        if (status == -2) {
            return evaluation_error(evaluation, expression, "shift count must be between 0 and 63");
        }
        break;
    case OP_RIGHT_SHIFT:
        status = checked_right_shift(left, right, result);
        if (status != 0) {
            return evaluation_error(evaluation, expression, "shift count must be between 0 and 63");
        }
        break;
    case OP_BIT_AND:
        *result = signed_bits((uint64_t)left & (uint64_t)right);
        break;
    case OP_BIT_XOR:
        *result = signed_bits((uint64_t)left ^ (uint64_t)right);
        break;
    case OP_BIT_OR:
        *result = signed_bits((uint64_t)left | (uint64_t)right);
        break;
    case OP_EQUAL:
        *result = left == right;
        break;
    case OP_NOT_EQUAL:
        *result = left != right;
        break;
    case OP_LESS:
        *result = left < right;
        break;
    case OP_LESS_EQUAL:
        *result = left <= right;
        break;
    case OP_GREATER:
        *result = left > right;
        break;
    case OP_GREATER_EQUAL:
        *result = left >= right;
        break;
    default:
        return evaluation_error(evaluation, expression, "invalid arithmetic operator");
    }
    if (status != 0) {
        return evaluation_error(evaluation, expression, "signed 64-bit arithmetic overflow");
    }
    return 0;
}

static int read_variable(Evaluation *evaluation, const Expression *expression, int64_t *result)
{
    const char *value = shell_get(evaluation->shell, expression->name);
    if (value == NULL || *value == '\0') {
        if (value == NULL && evaluation->shell->nounset) {
            return evaluation_error(evaluation, expression, "unset arithmetic variable");
        }
        *result = 0;
        return 0;
    }
    for (size_t index = 0; index < evaluation->variable_count; ++index) {
        if (strcmp(evaluation->variables[index], expression->name) == 0) {
            return evaluation_error(evaluation, expression, "recursive arithmetic variable");
        }
    }
    if (evaluation->variable_count >= MISH_MAX_DEPTH) {
        return evaluation_error(evaluation, expression, "arithmetic variable nesting limit exceeded");
    }
    /* Evaluation can assign to this variable. Copy the value so the backing
     * Var allocation may safely change while its expression is evaluated. */
    char *copy = xstrdup(value);
    evaluation->variables[evaluation->variable_count++] = expression->name;
    int status = evaluate_source(evaluation, copy, result);
    --evaluation->variable_count;
    free(copy);
    return status;
}

static int write_variable(Evaluation *evaluation, const Expression *expression,
                          const char *name, int64_t value)
{
    char text[64];
    snprintf(text, sizeof(text), "%" PRId64, value);
    if (shell_set(evaluation->shell, name, text) != 0) {
        return evaluation_error(evaluation, expression, "cannot assign arithmetic variable");
    }
    return 0;
}

static int evaluate_increment(Evaluation *evaluation, const Expression *expression,
                              int64_t *result)
{
    int64_t original;
    if (read_variable(evaluation, expression->first, &original) != 0) {
        return -1;
    }
    int64_t updated;
    int64_t delta = expression->operator == OP_INCREMENT ? 1 : -1;
    if (checked_add(original, delta, &updated) != 0) {
        return evaluation_error(evaluation, expression, "increment overflows signed 64-bit integer");
    }
    if (write_variable(evaluation, expression, expression->first->name, updated) != 0) {
        return -1;
    }
    *result = expression->type == EX_POSTFIX ? original : updated;
    return 0;
}

static int evaluate_unary(Evaluation *evaluation, const Expression *expression,
                          int64_t *result)
{
    if (expression->operator == OP_INCREMENT || expression->operator == OP_DECREMENT) {
        return evaluate_increment(evaluation, expression, result);
    }
    if (expression->operator == OP_SUBTRACT && expression->first->type == EX_LITERAL &&
        expression->first->wide_literal) {
        *result = INT64_MIN;
        return 0;
    }
    int64_t operand;
    if (evaluate(evaluation, expression->first, &operand) != 0) {
        return -1;
    }
    switch (expression->operator) {
    case OP_ADD:
        *result = operand;
        return 0;
    case OP_SUBTRACT:
        if (operand == INT64_MIN) {
            return evaluation_error(evaluation, expression, "negation overflows signed 64-bit integer");
        }
        *result = -operand;
        return 0;
    case OP_NOT:
        *result = operand == 0;
        return 0;
    case OP_COMPLEMENT:
        *result = signed_bits(~(uint64_t)operand);
        return 0;
    default:
        return evaluation_error(evaluation, expression, "invalid unary operator");
    }
}

static int evaluate_binary(Evaluation *evaluation, const Expression *expression,
                           int64_t *result)
{
    ArithmeticOperator operator = expression->operator;
    int64_t left = 0;
    int64_t right;
    if (operator == OP_ASSIGN) {
        if (evaluate(evaluation, expression->second, &right) != 0 ||
            write_variable(evaluation, expression, expression->first->name, right) != 0) {
            return -1;
        }
        *result = right;
        return 0;
    }
    if (evaluate(evaluation, expression->first, &left) != 0) {
        return -1;
    }
    if (operator == OP_LOGICAL_AND && left == 0) {
        *result = 0;
        return 0;
    }
    if (operator == OP_LOGICAL_OR && left != 0) {
        *result = 1;
        return 0;
    }
    if (evaluate(evaluation, expression->second, &right) != 0) {
        return -1;
    }
    if (operator == OP_COMMA) {
        *result = right;
        return 0;
    }
    if (operator == OP_LOGICAL_AND || operator == OP_LOGICAL_OR) {
        *result = right != 0;
        return 0;
    }
    if (apply_binary(evaluation, expression, underlying_operator(operator), left, right, result) != 0) {
        return -1;
    }
    if (assignment_operator(operator)) {
        return write_variable(evaluation, expression, expression->first->name, *result);
    }
    return 0;
}

static int evaluate(Evaluation *evaluation, const Expression *expression, int64_t *result)
{
    if (++evaluation->evaluation_steps > 100000) {
        return evaluation_error(evaluation, expression, "arithmetic evaluation work limit exceeded");
    }
    if (++evaluation->depth > MISH_MAX_DEPTH) {
        --evaluation->depth;
        return evaluation_error(evaluation, expression, "arithmetic evaluation nesting limit exceeded");
    }
    int status = 0;
    switch (expression->type) {
    case EX_LITERAL:
        if (expression->wide_literal) {
            status = evaluation_error(evaluation, expression, "positive literal exceeds signed 64-bit range");
        } else {
            *result = expression->number;
        }
        break;
    case EX_VARIABLE:
        status = read_variable(evaluation, expression, result);
        break;
    case EX_UNARY:
        status = evaluate_unary(evaluation, expression, result);
        break;
    case EX_POSTFIX:
        status = evaluate_increment(evaluation, expression, result);
        break;
    case EX_BINARY:
        status = evaluate_binary(evaluation, expression, result);
        break;
    case EX_TERNARY: {
        int64_t condition;
        status = evaluate(evaluation, expression->first, &condition);
        if (status == 0) {
            status = evaluate(evaluation, condition != 0 ? expression->second : expression->third, result);
        }
        break;
    }
    }
    --evaluation->depth;
    return status;
}

static int evaluate_source(Evaluation *evaluation, const char *source, int64_t *result)
{
    ArithmeticParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.source = source;
    size_t length = strlen(source);
    if (length > MISH_MAX_SOURCE || evaluation->parsed_bytes > MISH_MAX_SOURCE - length) {
        return evaluation_error(evaluation, NULL, "arithmetic source limit exceeded");
    }
    evaluation->parsed_bytes += length;
    if (next_token(&parser) != 0) {
        token_clear(&parser.token);
        if (evaluation->error == NULL) {
            evaluation->error = parser.error;
        } else {
            free(parser.error);
        }
        return -1;
    }
    if (parser.token.type == AR_END) {
        *result = 0;
        token_clear(&parser.token);
        return 0;
    }
    Expression *expression = parse_expression(&parser, 1);
    if (expression != NULL && parser.token.type != AR_END) {
        arithmetic_error(&parser, "unexpected trailing arithmetic token", parser.token.offset);
    }
    int status = -1;
    if (parser.error == NULL && expression != NULL) {
        status = evaluate(evaluation, expression, result);
    }
    if (parser.error != NULL) {
        if (evaluation->error == NULL) {
            evaluation->error = parser.error;
        } else {
            free(parser.error);
        }
    }
    expression_free(expression);
    token_clear(&parser.token);
    return status;
}

int arithmetic_eval(Shell *shell, const char *expression, int64_t *result, char **error)
{
    if (error != NULL) {
        *error = NULL;
    }
    if (result == NULL || shell == NULL) {
        if (error != NULL) {
            *error = xstrdup("invalid arithmetic evaluation arguments");
        }
        return -1;
    }
    Evaluation evaluation;
    memset(&evaluation, 0, sizeof(evaluation));
    evaluation.shell = shell;
    *result = 0;
    int status = evaluate_source(&evaluation, expression == NULL ? "" : expression, result);
    if (status != 0 && evaluation.error == NULL) {
        evaluation.error = xstrdup("invalid arithmetic expression");
    }
    if (error != NULL) {
        *error = evaluation.error;
    } else {
        free(evaluation.error);
    }
    return status;
}
