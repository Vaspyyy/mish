#include "mish.h"
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream)
{
    fputs("Usage: mish [options] [-c command | script] [arguments...]\n"
          "\n"
          "  -c COMMAND   execute shell source\n"
          "  -i           force interactive mode\n"
          "  -l           read login startup files\n"
          "  -s           read commands from standard input\n"
          "  -e           exit after an unhandled command failure\n"
          "  -u           reject unset parameter expansions\n"
          "  -f           disable pathname expansion\n"
          "  -x           trace expanded commands\n"
          "  -n           parse without executing\n"
          "  -o OPTION    enable a named shell option\n"
          "  +o OPTION    disable a named shell option\n"
          "  --norc       skip user startup files\n"
          "  --help       show this help\n"
          "  --version    show version\n"
          "\n"
          "Legacy variables: $(NAME); command capture: $[command].\n"
          "Standard $NAME, ${NAME}, arithmetic and shell scripting are supported.\n",
          stream);
}

static int one_option(Shell *shell, char option, bool enabled)
{
    switch (option) {
    case 'e': shell->errexit = enabled; break;
    case 'u': shell->nounset = enabled; break;
    case 'f': shell->noglob = enabled; break;
    case 'x': shell->xtrace = enabled; break;
    case 'n': shell->noexec = enabled; break;
    case 'v': shell->verbose = enabled; break;
    case 'a': shell->allexport = enabled; break;
    case 'm': shell->monitor = enabled; break;
    default:
        fprintf(stderr, "mish: invalid option: %c\n", option);
        return 2;
    }
    return 0;
}

/* FILE buffering on shell stdin can steal bytes belonging to read or a child.
 * Reading the descriptor directly lets foreground commands consume stdin. */
static char *read_descriptor_line(int descriptor, int *error)
{
    Buffer line;
    buffer_init(&line);
    for (;;) {
        char byte;
        ssize_t count = read(descriptor, &byte, 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            *error = errno;
            buffer_free(&line);
            return NULL;
        }
        if (!count) {
            if (!line.len) {
                buffer_free(&line);
                return NULL;
            }
            break;
        }
        if (byte == '\0') {
            *error = EINVAL;
            buffer_free(&line);
            return NULL;
        }
        buffer_putc(&line, byte);
        if (byte == '\n')
            break;
        if (line.len >= MISH_MAX_SOURCE) {
            *error = EFBIG;
            buffer_free(&line);
            return NULL;
        }
    }
    return buffer_take(&line);
}

static void run_startup(Shell *shell, bool no_rc)
{
    if (no_rc)
        return;
    if (shell->login && access("/etc/profile", R_OK) == 0)
        shell_run_file(shell, "/etc/profile", 0, NULL);
    const char *home = shell_get(shell, "HOME");
    if (!home)
        return;
    if (shell->login) {
        char *profile = path_join(home, ".mish_profile");
        if (access(profile, R_OK) == 0)
            shell_run_file(shell, profile, 0, NULL);
        free(profile);
    }
    if (shell->interactive) {
        char *rc = path_join(home, ".mishrc");
        if (access(rc, R_OK) == 0)
            shell_run_file(shell, rc, 0, NULL);
        free(rc);
    }
}

static int input_loop(Shell *shell)
{
    Buffer pending;
    buffer_init(&pending);
    unsigned eof_count = 0;
    unsigned command_number = 1;
    while (shell->flow != FLOW_EXIT) {
        shell_handle_traps(shell);
        jobs_reap(shell, shell->interactive);
        int error = 0;
        char *line;
        if (shell->interactive) {
            const char *format = shell_get(shell, pending.len ? "PS2" : "PS1");
            char *prompt = expand_prompt(shell, format ? format : "> ");
            line = editor_readline(shell, prompt);
            free(prompt);
            if (line && shell->last_status == 130 && !*line) {
                buffer_clear(&pending);
                free(line);
                continue;
            }
        } else {
            line = read_descriptor_line(STDIN_FILENO, &error);
        }
        if (!line) {
            if (error) {
                fprintf(stderr, "mish: input: %s\n", strerror(error));
                shell->last_status = 1;
            }
            if (shell->interactive && shell->ignoreeof && ++eof_count < 10) {
                fputs("Use exit to leave the shell.\n", stderr);
                continue;
            }
            if (pending.len)
                shell->last_status = shell_run_string(shell, pending.data, shell->source_name);
            break;
        }
        eof_count = 0;
        if (shell->verbose)
            fputs(line, stderr);
        buffer_append(&pending, line);
        if (!pending.len || pending.data[pending.len - 1] != '\n')
            buffer_putc(&pending, '\n');
        free(line);
        ParseError parse_error = {0};
        Node *probe = parse_source(pending.data, &parse_error);
        node_free(probe);
        if (parse_error.incomplete) {
            parse_error_free(&parse_error);
            continue;
        }
        parse_error_free(&parse_error);
        char *source = buffer_take(&pending);
        if (shell->interactive && shell->history_enabled) {
            char *history_error = NULL;
            char *expanded = history_expand(shell, source, &history_error);
            if (!expanded) {
                fprintf(stderr, "mish: history: %s\n", history_error ? history_error : "expansion failed");
                free(history_error);
                free(source);
                shell->last_status = 1;
                continue;
            }
            if (strcmp(expanded, source) || shell->history_print_only)
                printf("%s%s", expanded, *expanded && expanded[strlen(expanded) - 1] == '\n' ? "" : "\n");
            free(source);
            source = expanded;
            history_add(shell, source);
            if (shell->history_print_only) {
                free(source);
                continue;
            }
        }
        if (*source && strspn(source, " \t\r\n") != strlen(source)) {
            shell->last_status = shell_run_string(shell, source, shell->source_name);
            if (shell->interactive) {
                char *number = str_printf("%u", ++command_number);
                shell_set(shell, "MISH_COMMAND_NUMBER", number);
                free(number);
            }
        }
        free(source);
        if (shell->parse_failed && !shell->interactive)
            break;
    }
    buffer_free(&pending);
    return shell->flow == FLOW_EXIT ? shell->flow_status : shell->last_status;
}

static int finish_shell(Shell *shell, int status)
{
    shell->last_status = status;
    if (shell->traps[0] && *shell->traps[0] && !shell->trap_running) {
        char *command = xstrdup(shell->traps[0]);
        shell->flow = FLOW_NONE;
        shell->trap_running = true;
        shell_run_string(shell, command, "EXIT trap");
        shell->trap_running = false;
        free(command);
    }
    if (shell->interactive) {
        history_save(shell, shell->history.path, true);
        for (Job *job = shell->jobs; job; job = job->next) {
            if (job->stopped) {
                kill(-job->pgid, SIGHUP);
                kill(-job->pgid, SIGCONT);
            }
        }
    }
    return shell->flow == FLOW_EXIT ? shell->flow_status : status;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    Shell shell;
    if (shell_init(&shell, argv[0])) {
        shell_destroy(&shell);
        return 1;
    }
    bool no_rc = false;
    bool read_stdin = false;
    bool forced_interactive = false;
    const char *command = NULL;
    const char *script = NULL;
    int first = 1;
    int status = 0;
    for (; first < argc; ++first) {
        const char *option = argv[first];
        if (!strcmp(option, "--")) {
            ++first;
            break;
        }
        if (!strcmp(option, "--help")) {
            usage(stdout);
            shell_destroy(&shell);
            return 0;
        }
        if (!strcmp(option, "--version")) {
            printf("mish %s (C)\n", MISH_VERSION);
            shell_destroy(&shell);
            return 0;
        }
        if (!strcmp(option, "--norc")) {
            no_rc = true;
            continue;
        }
        if (!strcmp(option, "-c")) {
            if (++first == argc) {
                usage(stderr);
                status = 2;
            } else {
                command = argv[first++];
            }
            break;
        }
        if (!strcmp(option, "-i")) {
            forced_interactive = true;
            continue;
        }
        if (!strcmp(option, "-l")) {
            shell.login = true;
            continue;
        }
        if (!strcmp(option, "-s")) {
            read_stdin = true;
            continue;
        }
        if (!strcmp(option, "-o") || !strcmp(option, "+o")) {
            if (++first == argc || shell_option(&shell, argv[first], option[0] == '-')) {
                status = 2;
                break;
            }
            continue;
        }
        if ((option[0] == '-' || option[0] == '+') && option[1]) {
            for (const char *flag = option + 1; *flag; ++flag) {
                status = one_option(&shell, *flag, option[0] == '-');
                if (status)
                    break;
            }
            if (status)
                break;
            continue;
        }
        break;
    }
    if (status) {
        shell_destroy(&shell);
        return status;
    }
    if (!command && !read_stdin && first < argc)
        script = argv[first++];
    if (command && first < argc) {
        free(shell.argv0);
        shell.argv0 = xstrdup(argv[first++]);
    } else if (script) {
        free(shell.argv0);
        shell.argv0 = xstrdup(script);
    }
    shell_set_positional(&shell, (size_t)(argc - first), argv + first);
    shell.interactive = forced_interactive || (!command && !script && isatty(STDIN_FILENO) && isatty(STDERR_FILENO));
    shell_signals_init(&shell);
    if (shell.interactive)
        history_init(&shell);
    run_startup(&shell, no_rc);
    if (shell.flow == FLOW_EXIT)
        status = shell.flow_status;
    else if (command)
        status = shell_run_string(&shell, command, "-c");
    else if (script)
        status = shell_run_file(&shell, script, 0, NULL);
    else
        status = input_loop(&shell);
    if (shell.flow == FLOW_EXIT)
        status = shell.flow_status;
    status = finish_shell(&shell, status);
    shell_destroy(&shell);
    return status & 255;
}
