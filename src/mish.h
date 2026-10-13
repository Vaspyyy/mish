#ifndef MISH_H
#define MISH_H

/* Shared interfaces for mish's C shell. All returned strings are owned unless
 * noted otherwise. No module may mutate a parsed AST while executing it. */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <termios.h>

#define MISH_VERSION "0.2.0"
#define MISH_MAX_DEPTH 128
#define MISH_MAX_SOURCE (16u * 1024u * 1024u)

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StrVec;

void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *pointer, size_t size);
char *xstrdup(const char *text);
char *xstrndup(const char *text, size_t length);
char *str_printf(const char *format, ...);
void buffer_init(Buffer *buffer);
void buffer_free(Buffer *buffer);
void buffer_clear(Buffer *buffer);
void buffer_append(Buffer *buffer, const char *text);
void buffer_append_n(Buffer *buffer, const char *text, size_t length);
void buffer_putc(Buffer *buffer, char character);
char *buffer_take(Buffer *buffer);
void strvec_init(StrVec *vector);
void strvec_push(StrVec *vector, const char *text);
void strvec_push_owned(StrVec *vector, char *text);
void strvec_free(StrVec *vector);
char **strvec_take(StrVec *vector);
char *strvec_join(const StrVec *vector, const char *separator);
bool valid_name(const char *name);
char *path_join(const char *directory, const char *name);
char *read_stream(FILE *stream, size_t limit, char **error);
int write_all(int fd, const void *data, size_t length);
int set_cloexec(int fd);
int parse_number(const char *text, int64_t *number);
char *shell_quote(const char *text);

/* The lexer preserves raw quoting in word tokens; expansion belongs to runtime. */
typedef enum {
    TOK_WORD,
    TOK_NEWLINE,
    TOK_SEMI,
    TOK_AND,
    TOK_OR,
    TOK_PIPE,
    TOK_AMP,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_DSEMI,
    TOK_REDIR,
    TOK_EOF
} TokenType;

typedef struct {
    TokenType type;
    char *text;
    size_t line;
    size_t column;
    size_t offset;
    char *heredoc;
    bool heredoc_expand;
    bool heredoc_strip_tabs;
} Token;

typedef struct {
    Token *items;
    size_t len;
    size_t cap;
} TokenVec;

typedef struct {
    char *message;
    size_t line;
    size_t column;
    bool incomplete;
} ParseError;

void parse_error_free(ParseError *error);
int lex_source(const char *source, TokenVec *tokens, ParseError *error);
int lexer_expansion_end(const char *source, size_t dollar, size_t *closer, ParseError *error);
void tokens_free(TokenVec *tokens);

typedef enum {
    REDIR_INPUT,
    REDIR_OUTPUT,
    REDIR_APPEND,
    REDIR_CLOBBER,
    REDIR_DUP_INPUT,
    REDIR_DUP_OUTPUT,
    REDIR_READWRITE,
    REDIR_HEREDOC,
    REDIR_HERESTRING
} RedirType;

typedef struct Redir {
    int fd;
    RedirType type;
    char *word;
    char *heredoc;
    bool heredoc_expand;
    struct Redir *next;
} Redir;

typedef enum {
    NODE_SIMPLE,
    NODE_LIST,
    NODE_AND,
    NODE_OR,
    NODE_PIPE,
    NODE_BACKGROUND,
    NODE_SUBSHELL,
    NODE_GROUP,
    NODE_IF,
    NODE_WHILE,
    NODE_UNTIL,
    NODE_FOR,
    NODE_ARITH_FOR,
    NODE_CASE,
    NODE_FUNCTION,
    NODE_NOT
} NodeType;

typedef struct Node Node;
typedef struct CaseArm {
    StrVec patterns;
    Node *body;
    struct CaseArm *next;
} CaseArm;

struct Node {
    NodeType type;
    StrVec words;
    Node **children;
    size_t count;
    size_t cap;
    char *name;
    Redir *redirs;
    CaseArm *cases;
    size_t line;
};

Node *node_new(NodeType type);
void node_add_child(Node *parent, Node *child);
Node *node_clone(const Node *node);
void node_free(Node *node);
Node *parse_source(const char *source, ParseError *error);
void node_print(const Node *node, FILE *stream, unsigned indent);
char *node_source(const Node *node);

/* Variables carry export/readonly attributes independently of their value. */
typedef struct Var {
    char *name;
    char *value;
    bool exported;
    bool readonly;
    struct Var *next;
} Var;

typedef struct Alias {
    char *name;
    char *value;
    struct Alias *next;
} Alias;

typedef struct Function {
    char *name;
    Node *body;
    struct Function *next;
} Function;

typedef struct CommandCache {
    char *name;
    char *path;
    unsigned hits;
    struct CommandCache *next;
} CommandCache;

typedef struct Job {
    int id;
    pid_t pgid;
    pid_t *pids;
    int *statuses;
    bool *done;
    size_t count;
    bool stopped;
    bool notified;
    char *command;
    struct Job *next;
} Job;

typedef struct History {
    char **lines;
    size_t len;
    size_t cap;
    size_t limit;
    char *path;
    size_t saved;
} History;

typedef struct Scope Scope;
typedef struct KeyBinding KeyBinding;
typedef struct CompletionRule CompletionRule;
typedef enum {
    FLOW_NONE,
    FLOW_BREAK,
    FLOW_CONTINUE,
    FLOW_RETURN,
    FLOW_EXIT
} Flow;

typedef struct Shell {
    Var *variables;
    Alias *aliases;
    Function *functions;
    CommandCache *cache;
    Job *jobs;
    History history;
    StrVec directory_stack;
    StrVec expanding_aliases;
    Scope *scopes;
    KeyBinding *bindings;
    CompletionRule *completion_rules;
    char **positional;
    size_t positional_count;
    char *argv0;
    char *source_name;
    int last_status;
    pid_t last_background;
    pid_t shell_pid;
    pid_t shell_pgid;
    int next_job_id;
    int terminal_fd;
    bool interactive;
    bool login;
    bool subshell;
    bool errexit;
    bool nounset;
    bool noglob;
    bool xtrace;
    bool noexec;
    bool pipefail;
    bool noclobber;
    bool allexport;
    bool monitor;
    bool verbose;
    bool history_enabled;
    bool history_print_only;
    bool ignoreeof;
    bool posix;
    bool vi_mode;
    bool emacs_mode;
    bool globstar;
    bool braceexpand;
    bool nullglob;
    bool failglob;
    bool dotglob;
    unsigned execution_depth;
    unsigned loop_depth;
    unsigned function_depth;
    unsigned source_depth;
    unsigned conditional_depth;
    unsigned alias_depth;
    Flow flow;
    unsigned flow_level;
    int flow_status;
    char *traps[128];
    bool trap_running;
    bool parse_failed;
    unsigned long error_trap_serial;
    struct termios terminal_modes;
} Shell;

int shell_init(Shell *shell, const char *argv0);
void shell_destroy(Shell *shell);
const char *shell_get(const Shell *shell, const char *name);
int shell_set(Shell *shell, const char *name, const char *value);
int shell_export(Shell *shell, const char *name, const char *value);
int shell_unset(Shell *shell, const char *name);
Var *shell_find_var(Shell *shell, const char *name);
char **shell_environment(const Shell *shell);
void environment_free(char **environment);
void shell_set_positional(Shell *shell, size_t count, char *const *values);
void shell_push_scope(Shell *shell);
void shell_pop_scope(Shell *shell);
int shell_local(Shell *shell, const char *name, const char *value);
const char *alias_get(const Shell *shell, const char *name);
int alias_set(Shell *shell, const char *name, const char *value);
int alias_unset(Shell *shell, const char *name);
Function *function_find(Shell *shell, const char *name);
int function_set(Shell *shell, const char *name, const Node *body);
int function_unset(Shell *shell, const char *name);
void cache_clear(Shell *shell);
char *command_resolve(Shell *shell, const char *name, bool cache);

/* Expansion modes: argument fields, a scalar assignment, and prompt text. */
int expand_word(Shell *shell, const char *word, StrVec *fields, bool split, bool glob);
char *expand_scalar(Shell *shell, const char *word, int *status);
char *expand_pattern(Shell *shell, const char *word, int *status);
char *expand_prompt(Shell *shell, const char *prompt);
int expand_heredoc(Shell *shell, const char *text, char **result);
int arithmetic_eval(Shell *shell, const char *expression, int64_t *result, char **error);
int expand_braces(const char *word, StrVec *results, char **error);
int expand_glob(Shell *shell, const char *pattern, StrVec *results, char **error);

int execute_node(Shell *shell, const Node *node);
int shell_external(Shell *shell, int argc, char **argv, bool direct);
int shell_run_string(Shell *shell, const char *source, const char *name);
int shell_run_file(Shell *shell, const char *path, int argc, char **argv);
int shell_capture(Shell *shell, const char *source, char **output);
int shell_wait_status(int status);
void shell_signals_init(Shell *shell);
void shell_signals_child(void);
void shell_handle_traps(Shell *shell);
int signal_number(const char *name);
const char *signal_name(int number);
int shell_set_trap(Shell *shell, int number, const char *command);
void jobs_reap(Shell *shell, bool notify);
Job *job_find(Shell *shell, const char *spec);
int job_wait(Shell *shell, Job *job, bool foreground);
void job_remove(Shell *shell, Job *job);
int jobs_builtin(Shell *shell, int argc, char **argv);
int fg_builtin(Shell *shell, int argc, char **argv);
int bg_builtin(Shell *shell, int argc, char **argv);
int wait_builtin(Shell *shell, int argc, char **argv);
int kill_builtin(Shell *shell, int argc, char **argv);

/* Builtins return -1 when the command name is not recognized. */
int builtin_run(Shell *shell, int argc, char **argv);
bool builtin_exists(const char *name);
bool builtin_special(const char *name);
const char *const *builtin_names(size_t *count);
void builtin_help(const char *name, FILE *stream);
int shell_option(Shell *shell, const char *name, bool enabled);
void shell_print_options(Shell *shell, FILE *stream, bool reusable);

/* Editor/history implementation must restore terminal modes on every exit. */
int history_init(Shell *shell);
void history_destroy(Shell *shell);
void history_add(Shell *shell, const char *line);
int history_load(Shell *shell, const char *path);
int history_save(Shell *shell, const char *path, bool append);
void history_clear(Shell *shell);
int history_builtin(Shell *shell, int argc, char **argv);
char *history_expand(Shell *shell, const char *line, char **error);
char *editor_readline(Shell *shell, const char *prompt);
void editor_reset_terminal(void);
void completion_candidates(Shell *shell, const char *line, size_t cursor, StrVec *results);
int complete_builtin(Shell *shell, int argc, char **argv);
int bind_builtin(Shell *shell, int argc, char **argv);
int prompt_builtin(Shell *shell, int argc, char **argv);
void editor_state_destroy(Shell *shell);
const char *editor_binding_action(const Shell *shell, const char *sequence);
bool editor_action_supported(const char *action);
bool editor_binding_prefix(const Shell *shell, const char *sequence);
void completion_state_destroy(Shell *shell);

#endif
