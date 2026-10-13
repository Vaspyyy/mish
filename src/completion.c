#include "mish.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct CompletionRule {
    char *command;
    char *words;
    char *action;
    char *prefix;
    char *suffix;
    struct CompletionRule *next;
};

typedef struct {
    size_t start;
    bool command;
    bool redirect;
    char quote;
    char *word;
    char *command_name;
} CompletionContext;

static bool completion_operator(char character)
{
    return strchr(";|&()<>\n", character) != NULL;
}

static char *completion_unquote(const char *word, size_t length, char *active_quote)
{
    Buffer decoded;
    buffer_init(&decoded);
    char quote = '\0';
    for (size_t index = 0; index < length; index++) {
        char character = word[index];
        if (character == '\\' && quote != '\'' && index + 1 < length) {
            char next = word[index + 1];
            if (quote == '"' && !strchr("\\\"$`", next)) {
                buffer_putc(&decoded, character);
                continue;
            }
            buffer_putc(&decoded, next);
            index++;
        } else if ((character == '\'' || character == '"') &&
                   (!quote || quote == character)) {
            quote = quote ? '\0' : character;
        } else {
            buffer_putc(&decoded, character);
        }
    }
    *active_quote = quote;
    return buffer_take(&decoded);
}

static void completion_context_free(CompletionContext *context)
{
    free(context->word);
    free(context->command_name);
}

static CompletionContext completion_context(const char *line, size_t cursor)
{
    CompletionContext context = {0};
    context.command = true;
    size_t start = 0;
    char quote = '\0';
    bool escaped = false;
    bool in_word = false;
    bool expect_command = true;
    bool redirect = false;
    for (size_t index = 0; index < cursor; index++) {
        char character = line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && quote != '\'') {
            if (!in_word) {
                start = index;
                in_word = true;
            }
            escaped = true;
            continue;
        }
        if ((character == '\'' || character == '"') &&
            (!quote || quote == character)) {
            if (!in_word) {
                start = index;
                in_word = true;
            }
            quote = quote ? '\0' : character;
            continue;
        }
        if (quote) {
            continue;
        }
        bool delimiter = isspace((unsigned char)character) || completion_operator(character);
        if (!delimiter) {
            if (!in_word) {
                start = index;
                in_word = true;
            }
            continue;
        }
        if (in_word) {
            char active;
            char *word = completion_unquote(line + start, index - start, &active);
            bool assignment = strchr(word, '=') != NULL;
            if (expect_command && !assignment && !redirect) {
                free(context.command_name);
                context.command_name = xstrdup(word);
                expect_command = false;
            }
            free(word);
            if (redirect) {
                redirect = false;
            }
            in_word = false;
        }
        if (character == '>' || character == '<') {
            redirect = true;
        } else if (strchr(";|&()\n", character)) {
            expect_command = true;
            redirect = false;
            free(context.command_name);
            context.command_name = NULL;
        }
        start = index + 1;
    }
    context.start = in_word ? start : cursor;
    context.command = expect_command && !redirect;
    context.redirect = redirect;
    context.word = completion_unquote(line + context.start, cursor - context.start, &context.quote);
    if (!context.command_name) {
        context.command_name = xstrdup("");
    }
    return context;
}

static int completion_compare(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static void completion_unique(StrVec *results)
{
    if (results->len < 2) {
        return;
    }
    qsort(results->items, results->len, sizeof(*results->items), completion_compare);
    size_t output = 1;
    for (size_t index = 1; index < results->len; index++) {
        if (!strcmp(results->items[index], results->items[output - 1])) {
            free(results->items[index]);
        } else {
            results->items[output++] = results->items[index];
        }
    }
    results->len = output;
}

static char *completion_escape(const char *word, char quote)
{
    if (quote == '\'') {
        return shell_quote(word);
    }
    Buffer escaped;
    buffer_init(&escaped);
    if (quote == '"') {
        buffer_putc(&escaped, '"');
    }
    for (const char *cursor = word; *cursor; cursor++) {
        bool special = quote == '"' ? strchr("\\\"$`", *cursor) != NULL :
                       (isspace((unsigned char)*cursor) ||
                        strchr("\\\"'`$;&|(){}<>*?[]!#", *cursor) != NULL);
        if (special) {
            buffer_putc(&escaped, '\\');
        }
        buffer_putc(&escaped, *cursor);
    }
    if (quote == '"') {
        buffer_putc(&escaped, '"');
    }
    return buffer_take(&escaped);
}

static void completion_add(StrVec *results, const char *candidate,
                           const char *prefix, char quote)
{
    if (!strncmp(candidate, prefix, strlen(prefix))) {
        strvec_push_owned(results, completion_escape(candidate, quote));
    }
}

static void completion_variables(Shell *shell, const char *prefix, StrVec *results)
{
    const char *name = prefix;
    const char *opening = "";
    const char *closing = "";
    if (prefix[0] == '$') {
        opening = "$";
        name++;
        if (*name == '{') {
            opening = "${";
            closing = "}";
            name++;
        } else if (*name == '(') {
            opening = "$(";
            closing = ")";
            name++;
        }
    }
    size_t length = strlen(name);
    for (Var *variable = shell->variables; variable; variable = variable->next) {
        if (!strncmp(variable->name, name, length)) {
            strvec_push_owned(results, str_printf("%s%s%s", opening, variable->name, closing));
        }
    }
}

static void completion_aliases(Shell *shell, const char *prefix, char quote, StrVec *results)
{
    for (Alias *alias = shell->aliases; alias; alias = alias->next) {
        completion_add(results, alias->name, prefix, quote);
    }
}

static void completion_functions(Shell *shell, const char *prefix, char quote, StrVec *results)
{
    for (Function *function = shell->functions; function; function = function->next) {
        completion_add(results, function->name, prefix, quote);
    }
}

static void completion_builtins(const char *prefix, char quote, StrVec *results)
{
    size_t count = 0;
    const char *const *names = builtin_names(&count);
    for (size_t index = 0; index < count; index++) {
        completion_add(results, names[index], prefix, quote);
    }
}

static bool completion_executable(const char *path)
{
    struct stat metadata;
    return !stat(path, &metadata) && S_ISREG(metadata.st_mode) && !access(path, X_OK);
}

static void completion_commands(Shell *shell, const char *prefix, char quote, StrVec *results)
{
    completion_builtins(prefix, quote, results);
    completion_aliases(shell, prefix, quote, results);
    completion_functions(shell, prefix, quote, results);
    const char *path = shell_get(shell, "PATH");
    if (!path) {
        return;
    }
    do {
        const char *end = strchr(path, ':');
        size_t length = end ? (size_t)(end - path) : strlen(path);
        char *directory = length ? xstrndup(path, length) : xstrdup(".");
        DIR *stream = opendir(directory);
        if (stream) {
            struct dirent *entry;
            while ((entry = readdir(stream))) {
                if (strncmp(entry->d_name, prefix, strlen(prefix))) {
                    continue;
                }
                char *file = path_join(directory, entry->d_name);
                if (completion_executable(file)) {
                    completion_add(results, entry->d_name, prefix, quote);
                }
                free(file);
            }
            closedir(stream);
        }
        free(directory);
        if (!end) {
            break;
        }
        path = end + 1;
    } while (true);
}

static char *completion_directory(Shell *shell, const char *directory, bool tilde)
{
    Buffer result;
    buffer_init(&result);
    size_t index = 0;
    if (tilde && directory[0] == '~' && (directory[1] == '/' || !directory[1])) {
        const char *home = shell_get(shell, "HOME");
        if (home) {
            buffer_append(&result, home);
            index++;
        }
    }
    while (directory[index]) {
        if (directory[index] != '$') {
            buffer_putc(&result, directory[index++]);
            continue;
        }
        size_t start = ++index;
        char closer = '\0';
        if (directory[index] == '{' || directory[index] == '(') {
            closer = directory[index] == '{' ? '}' : ')';
            start = ++index;
        }
        while (isalnum((unsigned char)directory[index]) || directory[index] == '_') {
            index++;
        }
        if (start == index || (closer && directory[index] != closer)) {
            buffer_putc(&result, '$');
            index = start;
            if (closer) {
                buffer_putc(&result, closer == '}' ? '{' : '(');
            }
            continue;
        }
        char *name = xstrndup(directory + start, index - start);
        const char *value = shell_get(shell, name);
        if (value) {
            buffer_append(&result, value);
        }
        free(name);
        if (closer) {
            index++;
        }
    }
    return buffer_take(&result);
}

static void completion_files(Shell *shell, const char *prefix, char quote,
                             bool directories_only, bool executable_only, StrVec *results)
{
    const char *slash = strrchr(prefix, '/');
    size_t directory_length = slash ? (size_t)(slash - prefix) + 1 : 0;
    char *display_directory = xstrndup(prefix, directory_length);
    bool expand_directory = quote != '\'';
    char *directory = expand_directory ?
                      completion_directory(shell, directory_length ? display_directory : ".", !quote) :
                      xstrdup(directory_length ? display_directory : ".");
    if (expand_directory && strchr(display_directory, '$')) {
        free(display_directory);
        display_directory = str_printf("%s%s", directory,
                                       *directory && directory[strlen(directory) - 1] != '/' ? "/" : "");
    }
    const char *base = prefix + directory_length;
    DIR *stream = opendir(*directory ? directory : ".");
    if (!stream) {
        free(display_directory);
        free(directory);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(stream))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) {
            continue;
        }
        if (name[0] == '.' && base[0] != '.') {
            continue;
        }
        if (strncmp(name, base, strlen(base))) {
            continue;
        }
        char *path = path_join(directory, name);
        struct stat metadata;
        if (stat(path, &metadata)) {
            free(path);
            continue;
        }
        bool is_directory = S_ISDIR(metadata.st_mode);
        if ((directories_only && !is_directory) ||
            (executable_only && !is_directory && !completion_executable(path))) {
            free(path);
            continue;
        }
        char *candidate = str_printf("%s%s%s", display_directory, name, is_directory ? "/" : "");
        char *matching_prefix = str_printf("%s%s", display_directory, base);
        completion_add(results, candidate, matching_prefix, quote);
        free(matching_prefix);
        free(candidate);
        free(path);
    }
    closedir(stream);
    free(display_directory);
    free(directory);
}

static CompletionRule *completion_rule(Shell *shell, const char *command)
{
    for (CompletionRule *rule = shell->completion_rules; rule; rule = rule->next) {
        if (!strcmp(rule->command, command)) {
            return rule;
        }
    }
    return NULL;
}

static void completion_wordlist(const char *list, const char *prefix, char quote,
                                StrVec *results)
{
    TokenVec tokens = {0};
    ParseError error = {0};
    if (lex_source(list, &tokens, &error)) {
        parse_error_free(&error);
        tokens_free(&tokens);
        return;
    }
    for (size_t index = 0; index < tokens.len; index++) {
        if (tokens.items[index].type != TOK_WORD) {
            continue;
        }
        char active_quote;
        char *word = completion_unquote(tokens.items[index].text,
                                        strlen(tokens.items[index].text), &active_quote);
        completion_add(results, word, prefix, quote);
        free(word);
    }
    tokens_free(&tokens);
}

static void completion_action(Shell *shell, const char *action,
                              CompletionContext *context, StrVec *results)
{
    if (!strcmp(action, "command")) {
        completion_commands(shell, context->word, context->quote, results);
    } else if (!strcmp(action, "builtin")) {
        completion_builtins(context->word, context->quote, results);
    } else if (!strcmp(action, "alias")) {
        completion_aliases(shell, context->word, context->quote, results);
    } else if (!strcmp(action, "function")) {
        completion_functions(shell, context->word, context->quote, results);
    } else if (!strcmp(action, "variable")) {
        completion_variables(shell, context->word, results);
    } else {
        completion_files(shell, context->word, context->quote,
                         !strcmp(action, "directory"), false, results);
    }
}

void completion_candidates(Shell *shell, const char *line, size_t cursor, StrVec *results)
{
    if (cursor > strlen(line)) {
        cursor = strlen(line);
    }
    CompletionContext context = completion_context(line, cursor);
    CompletionRule *rule = completion_rule(shell, context.command_name);
    if (context.word[0] == '$' && context.quote != '\'' && !strchr(context.word, '/')) {
        completion_variables(shell, context.word, results);
        if (context.quote == '"') {
            for (size_t index = 0; index < results->len; index++) {
                char *quoted = str_printf("\"%s\"", results->items[index]);
                free(results->items[index]);
                results->items[index] = quoted;
            }
        }
    } else if (!context.command && rule) {
        if (rule->words) {
            completion_wordlist(rule->words, context.word, context.quote, results);
        }
        if (rule->action) {
            completion_action(shell, rule->action, &context, results);
        }
        char *prefix = completion_escape(rule->prefix ? rule->prefix : "", '\0');
        char *suffix = completion_escape(rule->suffix ? rule->suffix : "", '\0');
        for (size_t index = 0; index < results->len; index++) {
            char *decorated = str_printf("%s%s%s", prefix, results->items[index], suffix);
            free(results->items[index]);
            results->items[index] = decorated;
        }
        free(prefix);
        free(suffix);
    } else if (context.command && !strchr(context.word, '/')) {
        completion_commands(shell, context.word, context.quote, results);
    } else {
        bool directories = !strcmp(context.command_name, "cd") ||
                           !strcmp(context.command_name, "pushd") ||
                           !strcmp(context.command_name, "rmdir");
        completion_files(shell, context.word, context.quote, directories,
                         context.command, results);
    }
    completion_unique(results);
    completion_context_free(&context);
}

static void completion_rule_free(CompletionRule *rule)
{
    free(rule->command);
    free(rule->words);
    free(rule->action);
    free(rule->prefix);
    free(rule->suffix);
    free(rule);
}

void completion_state_destroy(Shell *shell)
{
    while (shell->completion_rules) {
        CompletionRule *next = shell->completion_rules->next;
        completion_rule_free(shell->completion_rules);
        shell->completion_rules = next;
    }
}

static bool completion_remove(Shell *shell, const char *command)
{
    CompletionRule **link = &shell->completion_rules;
    while (*link) {
        if (!strcmp((*link)->command, command)) {
            CompletionRule *rule = *link;
            *link = rule->next;
            completion_rule_free(rule);
            return true;
        }
        link = &(*link)->next;
    }
    return false;
}

static void completion_print(const CompletionRule *rule)
{
    fputs("complete", stdout);
    if (rule->words) {
        char *quoted = shell_quote(rule->words);
        printf(" -W %s", quoted);
        free(quoted);
    }
    if (rule->action) {
        printf(" -A %s", rule->action);
    }
    if (rule->prefix) {
        char *quoted = shell_quote(rule->prefix);
        printf(" -P %s", quoted);
        free(quoted);
    }
    if (rule->suffix) {
        char *quoted = shell_quote(rule->suffix);
        printf(" -S %s", quoted);
        free(quoted);
    }
    char *command = shell_quote(rule->command);
    printf(" %s\n", command);
    free(command);
}

static bool completion_valid_action(const char *action)
{
    const char *actions[] = {
        "command", "builtin", "file", "directory", "variable", "alias", "function"
    };
    for (size_t index = 0; index < sizeof(actions) / sizeof(actions[0]); index++) {
        if (!strcmp(action, actions[index])) {
            return true;
        }
    }
    return false;
}

int complete_builtin(Shell *shell, int argc, char **argv)
{
    bool remove = false;
    bool print = false;
    const char *words = NULL;
    const char *action = NULL;
    const char *prefix = NULL;
    const char *suffix = NULL;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        const char *option = argv[index++];
        if (!strcmp(option, "--")) {
            break;
        }
        if (!strcmp(option, "--help")) {
            puts("complete [-p|-r] [-W WORDS] [-A ACTION] [-P PREFIX] [-S SUFFIX] COMMAND...");
            puts("Actions: command, builtin, file, directory, variable, alias, function.");
            puts("Static words support shell quoting; completion never executes commands.");
            return 0;
        }
        if (!strcmp(option, "-p")) {
            print = true;
            continue;
        }
        if (!strcmp(option, "-r")) {
            remove = true;
            continue;
        }
        if (strcmp(option, "-W") && strcmp(option, "-A") &&
            strcmp(option, "-P") && strcmp(option, "-S")) {
            fprintf(stderr, "complete: unknown option: %s\n", option);
            return 2;
        }
        if (index == argc) {
            fprintf(stderr, "complete: %s requires an argument\n", option);
            return 2;
        }
        if (!strcmp(option, "-W")) {
            words = argv[index++];
        } else if (!strcmp(option, "-A")) {
            action = argv[index++];
            if (!completion_valid_action(action)) {
                fprintf(stderr, "complete: unknown action: %s\n", action);
                return 2;
            }
        } else if (!strcmp(option, "-P")) {
            prefix = argv[index++];
        } else {
            suffix = argv[index++];
        }
    }
    if (remove && (print || words || action || prefix || suffix)) {
        fputs("complete: -r cannot be combined with a completion definition\n", stderr);
        return 2;
    }
    if (index == argc) {
        if (remove) {
            completion_state_destroy(shell);
            return 0;
        }
        if (words || action || prefix || suffix) {
            fputs("complete: command name required\n", stderr);
            return 2;
        }
        for (CompletionRule *rule = shell->completion_rules; rule; rule = rule->next) {
            completion_print(rule);
        }
        return 0;
    }
    int status = 0;
    for (; index < argc; index++) {
        const char *command = argv[index];
        if (remove) {
            if (!completion_remove(shell, command)) {
                fprintf(stderr, "complete: no rule for %s\n", command);
                status = 1;
            }
        } else if (print || (!words && !action && !prefix && !suffix)) {
            CompletionRule *rule = completion_rule(shell, command);
            if (rule) {
                completion_print(rule);
            } else {
                fprintf(stderr, "complete: no rule for %s\n", command);
                status = 1;
            }
        } else {
            completion_remove(shell, command);
            CompletionRule *rule = xcalloc(1, sizeof(*rule));
            rule->command = xstrdup(command);
            rule->words = words ? xstrdup(words) : NULL;
            rule->action = action ? xstrdup(action) : NULL;
            rule->prefix = prefix ? xstrdup(prefix) : NULL;
            rule->suffix = suffix ? xstrdup(suffix) : NULL;
            rule->next = shell->completion_rules;
            shell->completion_rules = rule;
        }
    }
    return status;
}
