#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t pending_signals[128];
static Shell *active_shell;
static bool inherited_ignored[128];

typedef struct {
    int number;
    const char *name;
} SignalName;

static const SignalName signal_names[] = {
    {0, "EXIT"},
    {SIGHUP, "HUP"},
    {SIGINT, "INT"},
    {SIGQUIT, "QUIT"},
    {SIGILL, "ILL"},
    {SIGABRT, "ABRT"},
    {SIGFPE, "FPE"},
    {SIGKILL, "KILL"},
    {SIGSEGV, "SEGV"},
    {SIGPIPE, "PIPE"},
    {SIGALRM, "ALRM"},
    {SIGTERM, "TERM"},
    {SIGUSR1, "USR1"},
    {SIGUSR2, "USR2"},
    {SIGCHLD, "CHLD"},
    {SIGCONT, "CONT"},
    {SIGSTOP, "STOP"},
    {SIGTSTP, "TSTP"},
    {SIGTTIN, "TTIN"},
    {SIGTTOU, "TTOU"},
#ifdef SIGWINCH
    {SIGWINCH, "WINCH"},
#endif
#ifdef SIGBUS
    {SIGBUS, "BUS"},
#endif
#ifdef SIGTRAP
    {SIGTRAP, "TRAP"},
#endif
#ifdef SIGXCPU
    {SIGXCPU, "XCPU"},
#endif
#ifdef SIGXFSZ
    {SIGXFSZ, "XFSZ"},
#endif
#ifdef SIGVTALRM
    {SIGVTALRM, "VTALRM"},
#endif
#ifdef SIGPROF
    {SIGPROF, "PROF"},
#endif
    {126, "DEBUG"},
    {127, "ERR"}
};

int signal_number(const char *name)
{
    if (!name || !*name)
        return -1;
    if (!strncmp(name, "SIG", 3))
        name += 3;
    for (size_t i = 0; i < sizeof(signal_names) / sizeof(signal_names[0]); ++i) {
        if (!strcmp(name, signal_names[i].name))
            return signal_names[i].number;
    }
    int64_t number;
    if (!parse_number(name, &number) && number >= 0 && number < 128)
        return (int)number;
    return -1;
}

const char *signal_name(int number)
{
    for (size_t i = 0; i < sizeof(signal_names) / sizeof(signal_names[0]); ++i) {
        if (signal_names[i].number == number)
            return signal_names[i].name;
    }
    return NULL;
}

static void receive_signal(int number)
{
    int saved_errno = errno;
    if (number > 0 && number < 128)
        pending_signals[number] = 1;
    errno = saved_errno;
}

static int set_handler(int number, void (*handler)(int), bool restart)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = handler;
    action.sa_flags = restart ? SA_RESTART : 0;
    return sigaction(number, &action, NULL);
}

int shell_set_trap(Shell *shell, int number, const char *command)
{
    if (number < 0 || number >= 128 || number == SIGKILL || number == SIGSTOP)
        return 1;
    if (number > 0 && number < 126) {
        void (*handler)(int) = command ? (*command ? receive_signal : SIG_IGN) : SIG_DFL;
        if (!command && number == SIGCHLD)
            handler = receive_signal;
        if (!command && shell->interactive &&
            (number == SIGINT || number == SIGQUIT || number == SIGTSTP ||
             number == SIGTTIN || number == SIGTTOU))
            handler = SIG_IGN;
        if (!command && shell->interactive && (number == SIGTERM || number == SIGHUP))
            handler = receive_signal;
        if (set_handler(number, handler, number == SIGCHLD))
            return 1;
    }
    char *copy = command ? xstrdup(command) : NULL;
    free(shell->traps[number]);
    shell->traps[number] = copy;
    return 0;
}

void shell_signals_init(Shell *shell)
{
    active_shell = shell;
    for (int number = 1; number < 126; ++number) {
        struct sigaction action;
        inherited_ignored[number] = !sigaction(number, NULL, &action) && action.sa_handler == SIG_IGN;
    }
    memset((void *)pending_signals, 0, sizeof(pending_signals));
    set_handler(SIGCHLD, receive_signal, true);
    if (!shell->interactive)
        return;
    shell->monitor = isatty(shell->terminal_fd);
    set_handler(SIGINT, SIG_IGN, false);
    set_handler(SIGQUIT, SIG_IGN, false);
    set_handler(SIGTSTP, SIG_IGN, false);
    set_handler(SIGTTIN, SIG_IGN, false);
    set_handler(SIGTTOU, SIG_IGN, false);
    set_handler(SIGTERM, receive_signal, false);
    set_handler(SIGHUP, receive_signal, false);
    if (!shell->monitor)
        return;
    pid_t foreground = tcgetpgrp(shell->terminal_fd);
    if (foreground != getpgrp()) {
        /* An explicitly interactive shell launched in a background group must
         * not steal the controlling terminal from the current foreground job. */
        shell->monitor = false;
        return;
    }
    shell->shell_pgid = getpid();
    if (setpgid(0, shell->shell_pgid) < 0 && errno != EACCES && errno != EPERM) {
        perror("mish: setpgid");
        shell->monitor = false;
        return;
    }
    if (tcsetpgrp(shell->terminal_fd, shell->shell_pgid) < 0) {
        shell->monitor = false;
        return;
    }
    tcgetattr(shell->terminal_fd, &shell->terminal_modes);
}

void shell_signals_child(void)
{
    for (int number = 1; number < 126; ++number) {
        if (number == SIGKILL || number == SIGSTOP)
            continue;
        bool ignored = inherited_ignored[number] ||
            (active_shell && active_shell->traps[number] && !*active_shell->traps[number]);
        set_handler(number, ignored ? SIG_IGN : SIG_DFL, false);
    }
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
}

void shell_handle_traps(Shell *shell)
{
    if (shell->trap_running)
        return;
    for (int number = 1; number < 126; ++number) {
        if (!pending_signals[number])
            continue;
        pending_signals[number] = 0;
        if (number == SIGCHLD)
            jobs_reap(shell, shell->interactive);
        const char *command = shell->traps[number];
        if (!command && shell->interactive && (number == SIGTERM || number == SIGHUP)) {
            shell->flow = FLOW_EXIT;
            shell->flow_status = 128 + number;
            break;
        }
        if (!command || !*command)
            continue;
        char *copy = xstrdup(command);
        int saved_status = shell->last_status;
        shell->trap_running = true;
        (void)shell_run_string(shell, copy, "trap");
        shell->trap_running = false;
        shell->last_status = saved_status;
        free(copy);
        if (shell->flow == FLOW_EXIT)
            break;
    }
}
