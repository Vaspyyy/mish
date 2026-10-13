#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct SavedFd {
    int target;
    int saved;
    struct SavedFd *next;
} SavedFd;

typedef struct Assignment {
    char *name;
    char *old_value;
    bool existed;
    bool exported;
    bool readonly;
    struct Assignment *next;
} Assignment;

static int execute_internal(Shell *shell, const Node *node);
static int execute_simple(Shell *shell, const Node *node, bool direct);

static void child_finish(int status)
{
    fflush(NULL);
    _exit(status & 255);
}

static bool completed_job(const Job *job)
{
    for (size_t i = 0; i < job->count; ++i) {
        if (!job->done[i])
            return false;
    }
    return true;
}

static Job *make_job(Shell *shell, size_t count, const char *command)
{
    Job *job = xcalloc(1, sizeof(*job));
    job->id = shell->next_job_id++;
    job->count = count;
    job->pids = xcalloc(count, sizeof(pid_t));
    job->statuses = xcalloc(count, sizeof(int));
    job->done = xcalloc(count, sizeof(bool));
    job->command = xstrdup(command);
    job->next = shell->jobs;
    shell->jobs = job;
    return job;
}

static char *node_description(const Node *node)
{
    if (node->type == NODE_SIMPLE)
        return strvec_join(&node->words, " ");
    if (node->type == NODE_PIPE) {
        Buffer description;
        buffer_init(&description);
        for (size_t i = 0; i < node->count; ++i) {
            if (i)
                buffer_append(&description, " | ");
            char *child = node_description(node->children[i]);
            buffer_append(&description, child);
            free(child);
        }
        return buffer_take(&description);
    }
    if (node->count)
        return node_description(node->children[0]);
    return xstrdup("compound command");
}

static int temporary_input(const char *content)
{
    const char *temporary = getenv("TMPDIR");
    char *template = path_join(temporary && *temporary ? temporary : "/tmp", "mish-heredoc-XXXXXX");
    int descriptor = mkstemp(template);
    if (descriptor >= 0)
        unlink(template);
    free(template);
    if (descriptor < 0)
        return -1;
    set_cloexec(descriptor);
    if (write_all(descriptor, content, strlen(content)) || lseek(descriptor, 0, SEEK_SET) < 0) {
        int saved_errno = errno;
        close(descriptor);
        errno = saved_errno;
        return -1;
    }
    return descriptor;
}

static int save_descriptor(SavedFd **saved, int target)
{
    for (SavedFd *entry = *saved; entry; entry = entry->next) {
        if (entry->target == target)
            return 0;
    }
    SavedFd *entry = xcalloc(1, sizeof(*entry));
    entry->target = target;
    entry->saved = fcntl(target, F_DUPFD_CLOEXEC, 1024);
    if (entry->saved < 0 && errno != EBADF) {
        free(entry);
        return 1;
    }
    entry->next = *saved;
    *saved = entry;
    return 0;
}

static void restore_descriptors(SavedFd *saved)
{
    fflush(NULL);
    while (saved) {
        SavedFd *next = saved->next;
        if (saved->saved >= 0) {
            dup2(saved->saved, saved->target);
            close(saved->saved);
        } else {
            close(saved->target);
        }
        free(saved);
        saved = next;
    }
    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);
}

static int apply_redirections(Shell *shell, const Redir *redirection, SavedFd **saved)
{
    fflush(NULL);
    for (; redirection; redirection = redirection->next) {
        int target = redirection->fd;
        if (target < 0 || target > 1023) {
            fprintf(stderr, "mish: redirection descriptor out of range\n");
            return 1;
        }
        if (saved && save_descriptor(saved, target)) {
            perror("mish: save descriptor");
            return 1;
        }
        char *word = NULL;
        int descriptor = -1;
        if (redirection->type == REDIR_HEREDOC) {
            if (redirection->heredoc_expand) {
                if (expand_heredoc(shell, redirection->heredoc ? redirection->heredoc : "", &word))
                    return 1;
            } else {
                word = xstrdup(redirection->heredoc);
            }
            descriptor = temporary_input(word);
        } else {
            StrVec fields;
            strvec_init(&fields);
            int status = expand_word(shell, redirection->word, &fields, false, true);
            if (status || fields.len != 1) {
                if (!status)
                    fprintf(stderr, "mish: ambiguous redirection: %s\n", redirection->word);
                strvec_free(&fields);
                return 1;
            }
            word = xstrdup(fields.items[0]);
            strvec_free(&fields);
            int flags = 0;
            switch (redirection->type) {
            case REDIR_INPUT:
                flags = O_RDONLY;
                break;
            case REDIR_OUTPUT:
                flags = O_WRONLY | O_CREAT | (shell->noclobber ? O_EXCL : O_TRUNC);
                break;
            case REDIR_CLOBBER:
                flags = O_WRONLY | O_CREAT | O_TRUNC;
                break;
            case REDIR_APPEND:
                flags = O_WRONLY | O_CREAT | O_APPEND;
                break;
            case REDIR_READWRITE:
                flags = O_RDWR | O_CREAT;
                break;
            case REDIR_HERESTRING: {
                char *line = str_printf("%s\n", word);
                descriptor = temporary_input(line);
                free(line);
                break;
            }
            case REDIR_DUP_INPUT:
            case REDIR_DUP_OUTPUT:
                if (!strcmp(word, "-")) {
                    close(target);
                    free(word);
                    continue;
                } else {
                    int64_t source;
                    if (parse_number(word, &source) || source < 0 || source > 1023) {
                        fprintf(stderr, "mish: invalid descriptor: %s\n", word);
                        free(word);
                        return 1;
                    }
                    if (dup2((int)source, target) < 0) {
                        perror("mish: duplicate descriptor");
                        free(word);
                        return 1;
                    }
                    free(word);
                    continue;
                }
            case REDIR_HEREDOC:
                break;
            }
            if (redirection->type != REDIR_HERESTRING)
                descriptor = open(word, flags | O_CLOEXEC, 0666);
        }
        if (descriptor < 0) {
            fprintf(stderr, "mish: %s: %s\n", word ? word : "redirection", strerror(errno));
            free(word);
            return 1;
        }
        if (descriptor != target) {
            if (dup2(descriptor, target) < 0) {
                perror("mish: redirect");
                close(descriptor);
                free(word);
                return 1;
            }
            close(descriptor);
        } else {
            int flags = fcntl(descriptor, F_GETFD);
            if (flags >= 0)
                fcntl(descriptor, F_SETFD, flags & ~FD_CLOEXEC);
        }
        free(word);
    }
    return 0;
}

static size_t assignment_length(const char *word)
{
    const char *equal = strchr(word, '=');
    if (!equal)
        return 0;
    char *name = xstrndup(word, (size_t)(equal - word));
    bool valid = valid_name(name);
    free(name);
    return valid ? (size_t)(equal - word) : 0;
}

static int apply_assignments(Shell *shell, const Node *node, size_t count, Assignment **saved)
{
    for (size_t i = 0; i < count; ++i) {
        const char *word = node->words.items[i];
        size_t length = assignment_length(word);
        char *name = xstrndup(word, length);
        if (saved) {
            Assignment *entry = xcalloc(1, sizeof(*entry));
            entry->name = xstrdup(name);
            Var *variable = shell_find_var(shell, name);
            if (variable) {
                entry->existed = true;
                entry->old_value = xstrdup(variable->value);
                entry->exported = variable->exported;
                entry->readonly = variable->readonly;
            }
            entry->next = *saved;
            *saved = entry;
        }
        int status = 0;
        char *value = expand_scalar(shell, word + length + 1, &status);
        if (!status)
            status = shell_set(shell, name, value);
        if (!status && saved)
            shell_find_var(shell, name)->exported = true;
        free(value);
        free(name);
        if (status)
            return status;
    }
    return 0;
}

static void release_assignments(Shell *shell, Assignment *saved, bool restore)
{
    while (saved) {
        Assignment *next = saved->next;
        if (restore) {
            Var *variable = shell_find_var(shell, saved->name);
            if (variable)
                variable->readonly = false;
            if (saved->existed) {
                shell_set(shell, saved->name, saved->old_value);
                variable = shell_find_var(shell, saved->name);
                variable->readonly = saved->readonly;
                variable->exported = saved->exported;
            } else {
                shell_unset(shell, saved->name);
            }
        }
        free(saved->name);
        free(saved->old_value);
        free(saved);
        saved = next;
    }
}

static int execute_function(Shell *shell, Function *function, int argc, char **argv)
{
    if (shell->function_depth >= MISH_MAX_DEPTH) {
        fprintf(stderr, "mish: function nesting limit exceeded\n");
        return 2;
    }
    Node *body = node_clone(function->body);
    char **old_positional = shell->positional;
    size_t old_count = shell->positional_count;
    shell->positional = NULL;
    shell->positional_count = 0;
    shell_set_positional(shell, (size_t)(argc - 1), argv + 1);
    shell_push_scope(shell);
    ++shell->function_depth;
    int status = execute_node(shell, body);
    --shell->function_depth;
    shell_pop_scope(shell);
    node_free(body);
    environment_free(shell->positional);
    shell->positional = old_positional;
    shell->positional_count = old_count;
    if (shell->flow == FLOW_RETURN) {
        status = shell->flow_status;
        shell->flow = FLOW_NONE;
    }
    return status;
}

static int exec_external(Shell *shell, int argc, char **argv)
{
    char *path = command_resolve(shell, argv[0], true);
    if (!path) {
        int status = 127;
        if (strchr(argv[0], '/') && access(argv[0], F_OK) == 0)
            status = 126;
        fprintf(stderr, "mish: %s: %s\n", argv[0], status == 127 ? "command not found" : "not executable");
        return status;
    }
    char **environment = shell_environment(shell);
    execve(path, argv, environment);
    int saved_errno = errno;
    environment_free(environment);
    if (saved_errno == ENOEXEC) {
        shell_set_positional(shell, (size_t)(argc - 1), argv + 1);
        free(shell->argv0);
        shell->argv0 = xstrdup(argv[0]);
        int status = shell_run_file(shell, path, 0, NULL);
        free(path);
        return status;
    }
    free(path);
    fprintf(stderr, "mish: %s: %s\n", argv[0], strerror(saved_errno));
    return saved_errno == ENOENT ? 127 : 126;
}

static void gate_wait(int gate[2])
{
    close(gate[1]);
    char byte;
    while (read(gate[0], &byte, 1) < 0 && errno == EINTR) {}
    close(gate[0]);
}

static int external_wait(Shell *shell, int argc, char **argv)
{
    int gate[2] = {-1, -1};
    if (shell->monitor && pipe(gate)) {
        perror("mish: pipe");
        return 1;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("mish: fork");
        if (gate[0] >= 0) {
            close(gate[0]);
            close(gate[1]);
        }
        return 1;
    }
    if (!pid) {
        shell_signals_child();
        if (shell->monitor) {
            setpgid(0, 0);
            gate_wait(gate);
        }
        shell->interactive = false;
        shell->monitor = false;
        shell->subshell = true;
        child_finish(exec_external(shell, argc, argv));
    }
    Job *job = make_job(shell, 1, argv[0]);
    job->pids[0] = pid;
    job->pgid = pid;
    if (shell->monitor) {
        close(gate[0]);
        setpgid(pid, pid);
        tcsetpgrp(shell->terminal_fd, pid);
        close(gate[1]);
    }
    int status = job_wait(shell, job, true);
    if (completed_job(job))
        job_remove(shell, job);
    return status;
}

int shell_external(Shell *shell, int argc, char **argv, bool direct)
{
    return direct ? exec_external(shell, argc, argv) : external_wait(shell, argc, argv);
}

static int execute_simple(Shell *shell, const Node *node, bool direct)
{
    size_t assignments = 0;
    while (assignments < node->words.len && assignment_length(node->words.items[assignments]))
        ++assignments;
    Assignment *saved_assignments = NULL;
    int status = apply_assignments(shell, node, assignments,
                                   assignments < node->words.len ? &saved_assignments : NULL);
    StrVec arguments;
    strvec_init(&arguments);
    if (!status) {
        for (size_t i = assignments; i < node->words.len; ++i) {
            status = expand_word(shell, node->words.items[i], &arguments, true, true);
            if (status)
                break;
        }
    }
    SavedFd *saved_fds = NULL;
    bool special = arguments.len && builtin_special(arguments.items[0]);
    if (!status)
        status = apply_redirections(shell, node->redirs, &saved_fds);
    if (!status && shell->xtrace) {
        fputs("+", stderr);
        for (size_t i = 0; i < arguments.len; ++i)
            fprintf(stderr, " %s", arguments.items[i]);
        fputc('\n', stderr);
    }
    if (!status && arguments.len && !shell->noexec) {
        const char *raw_command = assignments < node->words.len ? node->words.items[assignments] : "";
        const char *replacement = !strcmp(raw_command, arguments.items[0]) ? alias_get(shell, arguments.items[0]) : NULL;
        for (size_t i = 0; replacement && i < shell->expanding_aliases.len; ++i) {
            if (!strcmp(shell->expanding_aliases.items[i], arguments.items[0]))
                replacement = NULL;
        }
        if (replacement && shell->alias_depth < 32) {
            Buffer command;
            buffer_init(&command);
            buffer_append(&command, replacement);
            for (size_t i = 1; i < arguments.len; ++i) {
                char *quoted = shell_quote(arguments.items[i]);
                buffer_putc(&command, ' ');
                buffer_append(&command, quoted);
                free(quoted);
            }
            ++shell->alias_depth;
            strvec_push(&shell->expanding_aliases, arguments.items[0]);
            status = shell_run_string(shell, command.data, "alias");
            free(shell->expanding_aliases.items[--shell->expanding_aliases.len]);
            shell->expanding_aliases.items[shell->expanding_aliases.len] = NULL;
            --shell->alias_depth;
            buffer_free(&command);
            goto simple_cleanup;
        }
        if (replacement) {
            fprintf(stderr, "mish: alias expansion limit exceeded\n");
            status = 2;
            goto simple_cleanup;
        }
        if (shell->traps[126] && !shell->trap_running) {
            char *command = xstrdup(shell->traps[126]);
            shell->trap_running = true;
            shell_run_string(shell, command, "DEBUG trap");
            shell->trap_running = false;
            free(command);
        }
        Function *function = function_find(shell, arguments.items[0]);
        if (function) {
            status = execute_function(shell, function, (int)arguments.len, arguments.items);
        } else {
            status = builtin_run(shell, (int)arguments.len, arguments.items);
            if (status < 0)
                status = direct ? exec_external(shell, (int)arguments.len, arguments.items) : external_wait(shell, (int)arguments.len, arguments.items);
        }
    }
simple_cleanup:
    if (!status && arguments.len == 1 && !strcmp(arguments.items[0], "exec")) {
        while (saved_fds) {
            SavedFd *next = saved_fds->next;
            if (saved_fds->saved >= 0)
                close(saved_fds->saved);
            free(saved_fds);
            saved_fds = next;
        }
    } else {
        restore_descriptors(saved_fds);
    }
    release_assignments(shell, saved_assignments, !special);
    strvec_free(&arguments);
    return status;
}

static int execute_pipeline(Shell *shell, const Node *node)
{
    size_t count = node->count;
    if (!count)
        return 0;
    int (*pipes)[2] = xcalloc(count > 1 ? count - 1 : 1, sizeof(int[2]));
    size_t made = 0;
    for (; made + 1 < count; ++made) {
        if (pipe(pipes[made])) {
            perror("mish: pipeline");
            for (size_t i = 0; i < made; ++i) {
                close(pipes[i][0]);
                close(pipes[i][1]);
            }
            free(pipes);
            return 1;
        }
        set_cloexec(pipes[made][0]);
        set_cloexec(pipes[made][1]);
    }
    int gate[2] = {-1, -1};
    if (shell->monitor && pipe(gate)) {
        perror("mish: pipeline gate");
        for (size_t i = 0; i < made; ++i) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        free(pipes);
        return 1;
    }
    char *description = node_description(node);
    Job *job = make_job(shell, count, description);
    free(description);
    fflush(NULL);
    bool failed = false;
    for (size_t i = 0; i < count; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("mish: pipeline fork");
            for (size_t j = i; j < count; ++j)
                job->done[j] = true;
            failed = true;
            break;
        }
        if (!pid) {
            shell_signals_child();
            if (shell->monitor) {
                setpgid(0, i ? job->pgid : 0);
                gate_wait(gate);
            }
            if (i && dup2(pipes[i - 1][0], STDIN_FILENO) < 0)
                child_finish(1);
            if (i + 1 < count && dup2(pipes[i][1], STDOUT_FILENO) < 0)
                child_finish(1);
            for (size_t j = 0; j < made; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            shell->interactive = false;
            shell->monitor = false;
            shell->subshell = true;
            shell->jobs = NULL;
            shell->flow = FLOW_NONE;
            const Node *command = node->children[i];
            int status = command->type == NODE_SIMPLE ? execute_simple(shell, command, true) : execute_node(shell, command);
            child_finish(status);
        }
        job->pids[i] = pid;
        if (!i)
            job->pgid = pid;
        if (shell->monitor)
            setpgid(pid, job->pgid);
    }
    for (size_t i = 0; i < made; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    free(pipes);
    if (shell->monitor) {
        close(gate[0]);
        tcsetpgrp(shell->terminal_fd, job->pgid);
        close(gate[1]);
    }
    if (failed) {
        for (size_t i = 0; i < count; ++i) {
            if (job->pids[i] > 0)
                kill(job->pids[i], SIGTERM);
        }
    }
    int status = job_wait(shell, job, true);
    if (completed_job(job))
        job_remove(shell, job);
    return failed ? 1 : status;
}

static int execute_subprocess(Shell *shell, const Node *node, bool background)
{
    int gate[2] = {-1, -1};
    if (shell->monitor && !background && pipe(gate)) {
        perror("mish: process gate");
        return 1;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("mish: fork");
        if (gate[0] >= 0) {
            close(gate[0]);
            close(gate[1]);
        }
        return 1;
    }
    if (!pid) {
        shell_signals_child();
        if (background || shell->monitor)
            setpgid(0, 0);
        if (gate[0] >= 0)
            gate_wait(gate);
        if (background && !shell->monitor) {
            int descriptor = open("/dev/null", O_RDONLY);
            if (descriptor >= 0) {
                dup2(descriptor, STDIN_FILENO);
                if (descriptor != STDIN_FILENO)
                    close(descriptor);
            }
        }
        shell->interactive = false;
        shell->monitor = false;
        shell->subshell = true;
        shell->jobs = NULL;
        shell->flow = FLOW_NONE;
        child_finish(execute_node(shell, node));
    }
    char *description = node_description(node);
    Job *job = make_job(shell, 1, description);
    free(description);
    job->pids[0] = pid;
    job->pgid = pid;
    if (shell->monitor && !background) {
        close(gate[0]);
        setpgid(pid, pid);
        tcsetpgrp(shell->terminal_fd, pid);
        close(gate[1]);
    }
    if (background) {
        setpgid(pid, pid);
        shell->last_background = pid;
        if (shell->interactive)
            fprintf(stderr, "[%d] %ld\n", job->id, (long)pid);
        return 0;
    }
    int status = job_wait(shell, job, shell->monitor);
    if (completed_job(job))
        job_remove(shell, job);
    return status;
}

static bool loop_flow(Shell *shell, bool *stop)
{
    if (shell->flow == FLOW_BREAK || shell->flow == FLOW_CONTINUE) {
        bool is_break = shell->flow == FLOW_BREAK;
        if (shell->flow_level > 1) {
            --shell->flow_level;
            *stop = true;
            return true;
        }
        shell->flow = FLOW_NONE;
        shell->flow_level = 0;
        *stop = is_break;
        return true;
    }
    if (shell->flow != FLOW_NONE) {
        *stop = true;
        return true;
    }
    *stop = false;
    return false;
}

static int execute_loop(Shell *shell, const Node *node)
{
    int status = 0;
    ++shell->loop_depth;
    while (shell->flow == FLOW_NONE) {
        ++shell->conditional_depth;
        int condition = execute_node(shell, node->children[0]);
        --shell->conditional_depth;
        if (shell->flow != FLOW_NONE)
            break;
        if ((node->type == NODE_WHILE && condition) || (node->type == NODE_UNTIL && !condition))
            break;
        status = execute_node(shell, node->children[1]);
        bool stop;
        if (loop_flow(shell, &stop) && stop)
            break;
    }
    --shell->loop_depth;
    return status;
}

static int execute_for(Shell *shell, const Node *node)
{
    StrVec values;
    strvec_init(&values);
    int status = 0;
    if (!node->words.len) {
        for (size_t i = 0; i < shell->positional_count; ++i)
            strvec_push(&values, shell->positional[i]);
    } else {
        for (size_t i = 0; i < node->words.len; ++i) {
            status = expand_word(shell, node->words.items[i], &values, true, true);
            if (status)
                break;
        }
    }
    if (!status) {
        ++shell->loop_depth;
        for (size_t i = 0; i < values.len; ++i) {
            if (shell_set(shell, node->name, values.items[i])) {
                status = 1;
                break;
            }
            status = execute_node(shell, node->children[0]);
            bool stop;
            if (loop_flow(shell, &stop) && stop)
                break;
        }
        --shell->loop_depth;
    }
    strvec_free(&values);
    return status;
}

static int execute_case(Shell *shell, const Node *node)
{
    int status = 0;
    char *subject = expand_scalar(shell, node->name, &status);
    if (!subject)
        return status;
    for (CaseArm *arm = node->cases; arm; arm = arm->next) {
        for (size_t i = 0; i < arm->patterns.len; ++i) {
            char *pattern = expand_pattern(shell, arm->patterns.items[i], &status);
            if (!pattern) {
                free(subject);
                return status;
            }
            bool matched = fnmatch(pattern, subject, 0) == 0;
            free(pattern);
            if (matched) {
                free(subject);
                return execute_node(shell, arm->body);
            }
        }
    }
    free(subject);
    return 0;
}

static int execute_arithmetic_for(Shell *shell, const Node *node)
{
    if (node->words.len != 3 || node->count != 1)
        return 2;
    int64_t value = 0;
    char *error = NULL;
    if (*node->words.items[0] && arithmetic_eval(shell, node->words.items[0], &value, &error)) {
        fprintf(stderr, "mish: for initialization: %s\n", error ? error : "invalid expression");
        free(error);
        return 2;
    }
    int status = 0;
    ++shell->loop_depth;
    while (shell->flow == FLOW_NONE) {
        if (*node->words.items[1]) {
            if (arithmetic_eval(shell, node->words.items[1], &value, &error)) {
                fprintf(stderr, "mish: for condition: %s\n", error ? error : "invalid expression");
                free(error);
                status = 2;
                break;
            }
            if (!value)
                break;
        }
        status = execute_node(shell, node->children[0]);
        bool stop;
        if (loop_flow(shell, &stop) && stop)
            break;
        if (*node->words.items[2] && arithmetic_eval(shell, node->words.items[2], &value, &error)) {
            fprintf(stderr, "mish: for step: %s\n", error ? error : "invalid expression");
            free(error);
            status = 2;
            break;
        }
    }
    --shell->loop_depth;
    return status;
}

static int execute_internal(Shell *shell, const Node *node)
{
    if (!node)
        return 0;
    switch (node->type) {
    case NODE_SIMPLE:
        return execute_simple(shell, node, false);
    case NODE_LIST: {
        int status = 0;
        for (size_t i = 0; i < node->count && shell->flow == FLOW_NONE; ++i)
            status = execute_node(shell, node->children[i]);
        return status;
    }
    case NODE_AND:
    case NODE_OR: {
        ++shell->conditional_depth;
        int status = execute_node(shell, node->children[0]);
        --shell->conditional_depth;
        if (shell->flow == FLOW_NONE && ((node->type == NODE_AND && !status) || (node->type == NODE_OR && status)))
            status = execute_node(shell, node->children[1]);
        return status;
    }
    case NODE_PIPE:
        return execute_pipeline(shell, node);
    case NODE_BACKGROUND:
        return execute_subprocess(shell, node->children[0], true);
    case NODE_SUBSHELL:
        return execute_subprocess(shell, node->children[0], false);
    case NODE_GROUP:
        return execute_node(shell, node->children[0]);
    case NODE_IF: {
        ++shell->conditional_depth;
        int status = execute_node(shell, node->children[0]);
        --shell->conditional_depth;
        if (shell->flow != FLOW_NONE)
            return status;
        if (!status)
            return execute_node(shell, node->children[1]);
        return node->count > 2 ? execute_node(shell, node->children[2]) : 0;
    }
    case NODE_WHILE:
    case NODE_UNTIL:
        return execute_loop(shell, node);
    case NODE_FOR:
        return execute_for(shell, node);
    case NODE_ARITH_FOR:
        return execute_arithmetic_for(shell, node);
    case NODE_CASE:
        return execute_case(shell, node);
    case NODE_FUNCTION:
        return function_set(shell, node->name, node->children[0]);
    case NODE_NOT: {
        ++shell->conditional_depth;
        int status = execute_node(shell, node->children[0]);
        --shell->conditional_depth;
        return !status;
    }
    }
    return 2;
}

int execute_node(Shell *shell, const Node *node)
{
    if (!node || shell->flow != FLOW_NONE)
        return shell->last_status;
    if (shell->noexec)
        return 0;
    if (shell->execution_depth >= MISH_MAX_DEPTH) {
        fprintf(stderr, "mish: execution nesting limit exceeded\n");
        shell->parse_failed = true;
        return 2;
    }
    ++shell->execution_depth;
    unsigned long previous_trap = shell->error_trap_serial;
    shell_handle_traps(shell);
    SavedFd *saved = NULL;
    int status = 0;
    if (node->type != NODE_SIMPLE)
        status = apply_redirections(shell, node->redirs, &saved);
    if (!status && !shell->noexec)
        status = execute_internal(shell, node);
    restore_descriptors(saved);
    --shell->execution_depth;
    shell->last_status = status;
    bool owns_failure = node->type == NODE_SIMPLE || node->type == NODE_PIPE || node->type == NODE_SUBSHELL;
    if (status && owns_failure && !shell->conditional_depth && shell->flow == FLOW_NONE) {
        if (shell->traps[127] && !shell->trap_running && shell->error_trap_serial == previous_trap) {
            ++shell->error_trap_serial;
            char *command = xstrdup(shell->traps[127]);
            shell->trap_running = true;
            shell_run_string(shell, command, "ERR trap");
            shell->trap_running = false;
            shell->last_status = status;
            free(command);
        }
        if (shell->errexit) {
            shell->flow = FLOW_EXIT;
            shell->flow_status = status;
        }
    }
    return status;
}

int shell_run_string(Shell *shell, const char *source, const char *name)
{
    shell->parse_failed = false;
    if (strlen(source) > MISH_MAX_SOURCE) {
        fprintf(stderr, "mish: source exceeds size limit\n");
        shell->parse_failed = true;
        return 2;
    }
    ParseError error = {0};
    Node *program = parse_source(source, &error);
    if (!program) {
        if (error.message)
            fprintf(stderr, "%s:%zu:%zu: %s\n", name ? name : "mish", error.line, error.column, error.message);
        bool failed = error.message != NULL;
        shell->parse_failed = failed;
        parse_error_free(&error);
        return failed ? 2 : 0;
    }
    int status = execute_node(shell, program);
    node_free(program);
    parse_error_free(&error);
    return status;
}

static int execute_source_lines(Shell *shell, const char *source, const char *name)
{
    Buffer pending;
    buffer_init(&pending);
    const char *position = source;
    int status = 0;
    while (*position && shell->flow == FLOW_NONE) {
        const char *newline = strchr(position, '\n');
        size_t length = newline ? (size_t)(newline - position) + 1 : strlen(position);
        buffer_append_n(&pending, position, length);
        position += length;
        ParseError error = {0};
        Node *probe = parse_source(pending.data, &error);
        node_free(probe);
        bool incomplete = error.incomplete;
        bool invalid = error.message != NULL && !incomplete;
        parse_error_free(&error);
        if (incomplete && *position)
            continue;
        if (invalid) {
            status = shell_run_string(shell, pending.data, name);
            break;
        }
        if (strspn(pending.data, " \t\r\n") != pending.len)
            status = shell_run_string(shell, pending.data, name);
        buffer_clear(&pending);
        if (status == 2 && shell->noexec)
            break;
    }
    buffer_free(&pending);
    return status;
}

int shell_run_file(Shell *shell, const char *path, int argc, char **argv)
{
    if (shell->source_depth >= MISH_MAX_DEPTH) {
        fprintf(stderr, "mish: source nesting limit exceeded\n");
        return 2;
    }
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "mish: %s: %s\n", path, strerror(errno));
        return 1;
    }
    char *error = NULL;
    char *source = read_stream(file, MISH_MAX_SOURCE, &error);
    fclose(file);
    if (!source) {
        fprintf(stderr, "mish: %s: %s\n", path, error ? error : "read error");
        free(error);
        return 1;
    }
    char **old_positional = NULL;
    size_t old_count = 0;
    if (argc > 0) {
        old_positional = shell->positional;
        old_count = shell->positional_count;
        shell->positional = NULL;
        shell_set_positional(shell, (size_t)argc, argv);
    }
    char *old_source = shell->source_name;
    shell->source_name = xstrdup(path);
    ++shell->source_depth;
    int status = execute_source_lines(shell, source, path);
    --shell->source_depth;
    free(shell->source_name);
    shell->source_name = old_source;
    free(source);
    if (argc > 0) {
        environment_free(shell->positional);
        shell->positional = old_positional;
        shell->positional_count = old_count;
    }
    if (shell->flow == FLOW_RETURN) {
        status = shell->flow_status;
        shell->flow = FLOW_NONE;
    }
    return status;
}

int shell_capture(Shell *shell, const char *source, char **output)
{
    *output = NULL;
    if (shell->execution_depth >= MISH_MAX_DEPTH) {
        fprintf(stderr, "mish: command substitution nesting limit exceeded\n");
        return 2;
    }
    int pipes[2];
    if (pipe(pipes)) {
        perror("mish: capture pipe");
        return 1;
    }
    int errors[2];
    if (pipe(errors)) {
        close(pipes[0]);
        close(pipes[1]);
        return 1;
    }
    set_cloexec(errors[0]);
    set_cloexec(errors[1]);
    set_cloexec(pipes[0]);
    set_cloexec(pipes[1]);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("mish: capture fork");
        close(pipes[0]);
        close(pipes[1]);
        close(errors[0]);
        close(errors[1]);
        return 1;
    }
    if (!pid) {
        close(errors[0]);
        close(pipes[0]);
        shell_signals_child();
        if (dup2(pipes[1], STDOUT_FILENO) < 0)
            child_finish(1);
        close(pipes[1]);
        int input = open("/dev/null", O_RDONLY);
        if (input >= 0) {
            dup2(input, STDIN_FILENO);
            if (input != STDIN_FILENO)
                close(input);
        }
        shell->interactive = false;
        shell->monitor = false;
        shell->subshell = true;
        shell->jobs = NULL;
        shell->flow = FLOW_NONE;
        ++shell->execution_depth;
        int status = shell_run_string(shell, source, "command substitution");
        char fatal = shell->parse_failed ? 1 : 0;
        (void)write_all(errors[1], &fatal, 1);
        close(errors[1]);
        child_finish(status);
    }
    close(errors[1]);
    close(pipes[1]);
    Buffer result;
    buffer_init(&result);
    char chunk[8192];
    bool failed = false;
    for (;;) {
        ssize_t count = read(pipes[0], chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) {
            shell_handle_traps(shell);
            continue;
        }
        if (count < 0) {
            perror("mish: capture read");
            failed = true;
            break;
        }
        if (!count)
            break;
        if (result.len + (size_t)count > MISH_MAX_SOURCE) {
            fprintf(stderr, "mish: captured output exceeds size limit\n");
            failed = true;
            kill(pid, SIGTERM);
            break;
        }
        for (ssize_t i = 0; i < count; ++i) {
            if (chunk[i])
                buffer_putc(&result, chunk[i]);
        }
    }
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    char fatal = 0;
    while (read(errors[0], &fatal, 1) < 0 && errno == EINTR) {}
    close(errors[0]);
    if (failed || fatal) {
        buffer_free(&result);
        if (fatal)
            shell->parse_failed = true;
        return fatal ? 2 : 1;
    }
    while (result.len && result.data[result.len - 1] == '\n')
        result.data[--result.len] = '\0';
    *output = buffer_take(&result);
    return shell_wait_status(status);
}
