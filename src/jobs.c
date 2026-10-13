#include "mish.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int shell_wait_status(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    if (WIFSTOPPED(status))
        return 128 + WSTOPSIG(status);
    return 0;
}

static bool job_finished(const Job *job)
{
    for (size_t i = 0; i < job->count; ++i) {
        if (!job->done[i])
            return false;
    }
    return true;
}

static const char *job_state(const Job *job)
{
    if (job_finished(job))
        return "Done";
    return job->stopped ? "Stopped" : "Running";
}

static void job_update(Job *job, pid_t pid, int status)
{
    for (size_t i = 0; i < job->count; ++i) {
        if (job->pids[i] != pid)
            continue;
        job->statuses[i] = status;
        if (WIFEXITED(status) || WIFSIGNALED(status))
            job->done[i] = true;
        else if (WIFSTOPPED(status))
            job->stopped = true;
#ifdef WIFCONTINUED
        else if (WIFCONTINUED(status))
            job->stopped = false;
#endif
        return;
    }
}

void job_remove(Shell *shell, Job *job)
{
    Job **link = &shell->jobs;
    while (*link && *link != job)
        link = &(*link)->next;
    if (!*link)
        return;
    *link = job->next;
    free(job->pids);
    free(job->statuses);
    free(job->done);
    free(job->command);
    free(job);
}

void jobs_reap(Shell *shell, bool notify)
{
    for (Job *job = shell->jobs; job; job = job->next) {
        for (size_t i = 0; i < job->count; ++i) {
            if (job->done[i])
                continue;
            int status;
            pid_t result = waitpid(job->pids[i], &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (result > 0)
                job_update(job, result, status);
        }
        if (notify && !job->notified && (job_finished(job) || job->stopped)) {
            fprintf(stderr, "[%d] %-8s %s\n", job->id, job_state(job), job->command);
            job->notified = true;
        }
    }
}

Job *job_find(Shell *shell, const char *spec)
{
    if (!spec || !*spec || !strcmp(spec, "%") || !strcmp(spec, "%%") || !strcmp(spec, "%+"))
        return shell->jobs;
    if (!strcmp(spec, "%-"))
        return shell->jobs ? shell->jobs->next : NULL;
    if (*spec == '%') {
        ++spec;
        bool substring = *spec == '?';
        if (substring)
            ++spec;
        if (!isdigit((unsigned char)*spec)) {
            Job *match = NULL;
            for (Job *job = shell->jobs; job; job = job->next) {
                bool found = substring ? strstr(job->command, spec) != NULL : !strncmp(job->command, spec, strlen(spec));
                if (found) {
                    if (match)
                        return NULL;
                    match = job;
                }
            }
            return match;
        }
        int64_t id;
        if (parse_number(spec, &id))
            return NULL;
        for (Job *job = shell->jobs; job; job = job->next) {
            if (job->id == id)
                return job;
        }
        return NULL;
    }
    int64_t pid;
    if (parse_number(spec, &pid))
        return NULL;
    for (Job *job = shell->jobs; job; job = job->next) {
        if (job->pgid == pid)
            return job;
        for (size_t i = 0; i < job->count; ++i) {
            if (job->pids[i] == pid)
                return job;
        }
    }
    return NULL;
}

int job_wait(Shell *shell, Job *job, bool foreground)
{
    if (foreground && shell->monitor) {
        editor_reset_terminal();
        tcsetpgrp(shell->terminal_fd, job->pgid);
    }
    if (foreground && job->stopped) {
        kill(-job->pgid, SIGCONT);
        job->stopped = false;
        job->notified = false;
    }
    for (size_t i = 0; i < job->count; ++i) {
        while (!job->done[i]) {
            int status;
            pid_t result = waitpid(job->pids[i], &status, WUNTRACED);
            if (result < 0 && errno == EINTR) {
                shell_handle_traps(shell);
                continue;
            }
            if (result < 0) {
                if (errno == ECHILD)
                    job->done[i] = true;
                else
                    perror("wait");
                break;
            }
            job_update(job, result, status);
            if (WIFSTOPPED(status))
                break;
        }
    }
    if (foreground && shell->monitor) {
        tcsetpgrp(shell->terminal_fd, shell->shell_pgid);
        tcsetattr(shell->terminal_fd, TCSADRAIN, &shell->terminal_modes);
    }
    int result = 0;
    for (size_t i = 0; i < job->count; ++i) {
        int status = shell_wait_status(job->statuses[i]);
        if (!shell->pipefail || status)
            result = status;
    }
    if (job->stopped && shell->interactive) {
        fprintf(stderr, "[%d] Stopped %s\n", job->id, job->command);
        job->notified = true;
    }
    return result;
}

int jobs_builtin(Shell *shell, int argc, char **argv)
{
    bool pid_only = false;
    bool long_format = false;
    bool running = false;
    bool stopped = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        if (!strcmp(argv[first], "--")) {
            ++first;
            break;
        }
        for (const char *flag = argv[first] + 1; *flag; ++flag) {
            switch (*flag) {
            case 'p': pid_only = true; break;
            case 'l': long_format = true; break;
            case 'r': running = true; break;
            case 's': stopped = true; break;
            default:
                fprintf(stderr, "jobs: invalid option: %c\n", *flag);
                return 2;
            }
        }
    }
    jobs_reap(shell, false);
    int result = 0;
    for (Job *job = shell->jobs; job; job = job->next) {
        bool selected = first == argc;
        for (int i = first; i < argc; ++i) {
            if (job_find(shell, argv[i]) == job)
                selected = true;
        }
        if (!selected || (running && (job->stopped || job_finished(job))) || (stopped && !job->stopped))
            continue;
        if (pid_only)
            printf("%ld\n", (long)job->pgid);
        else if (long_format)
            printf("[%d] %ld %-8s %s\n", job->id, (long)job->pgid, job_state(job), job->command);
        else
            printf("[%d]%c %-8s %s\n", job->id, job == shell->jobs ? '+' : ' ', job_state(job), job->command);
    }
    for (int i = first; i < argc; ++i) {
        if (!job_find(shell, argv[i])) {
            fprintf(stderr, "jobs: %s: no such job\n", argv[i]);
            result = 1;
        }
    }
    return result;
}

int fg_builtin(Shell *shell, int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "fg: usage: fg [job]\n");
        return 2;
    }
    if (!shell->monitor) {
        fprintf(stderr, "fg: job control is not enabled\n");
        return 1;
    }
    Job *job = job_find(shell, argc == 2 ? argv[1] : NULL);
    if (!job) {
        fprintf(stderr, "fg: no such job\n");
        return 1;
    }
    fprintf(stderr, "%s\n", job->command);
    int status = job_wait(shell, job, true);
    if (job_finished(job))
        job_remove(shell, job);
    return status;
}

int bg_builtin(Shell *shell, int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "bg: usage: bg [job]\n");
        return 2;
    }
    Job *job = job_find(shell, argc == 2 ? argv[1] : NULL);
    if (!job) {
        fprintf(stderr, "bg: no such job\n");
        return 1;
    }
    if (job_finished(job)) {
        fprintf(stderr, "bg: job has completed\n");
        return 1;
    }
    if (kill(-job->pgid, SIGCONT)) {
        perror("bg");
        return 1;
    }
    job->stopped = false;
    job->notified = false;
    printf("[%d] %s &\n", job->id, job->command);
    return 0;
}

int wait_builtin(Shell *shell, int argc, char **argv)
{
    if (argc == 1) {
        while (shell->jobs) {
            Job *job = shell->jobs;
            (void)job_wait(shell, job, false);
            job_remove(shell, job);
        }
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        Job *job = job_find(shell, argv[i]);
        if (!job) {
            fprintf(stderr, "wait: %s: not a child job\n", argv[i]);
            status = 127;
            continue;
        }
        status = job_wait(shell, job, false);
        if (job_finished(job))
            job_remove(shell, job);
    }
    return status;
}

int kill_builtin(Shell *shell, int argc, char **argv)
{
    int number = SIGTERM;
    int first = 1;
    if (first < argc && !strcmp(argv[first], "-l")) {
        if (++first == argc) {
            for (size_t i = 1; i < 126; ++i) {
                const char *name = signal_name((int)i);
                if (name)
                    printf("%zu) SIG%s\n", i, name);
            }
            return 0;
        }
        int64_t value;
        if (!parse_number(argv[first], &value)) {
            if (value > 128)
                value -= 128;
            const char *name = signal_name((int)value);
            if (name) {
                puts(name);
                return 0;
            }
        }
        number = signal_number(argv[first]);
        if (number >= 0) {
            printf("%d\n", number);
            return 0;
        }
        fprintf(stderr, "kill: unknown signal\n");
        return 2;
    }
    if (first < argc && (!strcmp(argv[first], "-s") || !strcmp(argv[first], "-n"))) {
        if (++first == argc)
            return 2;
        number = signal_number(argv[first++]);
    } else if (first < argc && argv[first][0] == '-' && strcmp(argv[first], "--")) {
        number = signal_number(argv[first++] + 1);
    }
    if (first < argc && !strcmp(argv[first], "--"))
        ++first;
    if (number < 0 || number >= 126 || first == argc) {
        fprintf(stderr, "kill: usage: kill [-s SIGNAL] PID|%%JOB...\n");
        return 2;
    }
    int result = 0;
    for (int i = first; i < argc; ++i) {
        pid_t target;
        if (argv[i][0] == '%') {
            Job *job = job_find(shell, argv[i]);
            if (!job) {
                fprintf(stderr, "kill: %s: no such job\n", argv[i]);
                result = 1;
                continue;
            }
            target = -job->pgid;
        } else {
            int64_t value;
            if (parse_number(argv[i], &value) || value < INT32_MIN || value > INT32_MAX) {
                fprintf(stderr, "kill: invalid pid: %s\n", argv[i]);
                result = 1;
                continue;
            }
            target = (pid_t)value;
        }
        if (kill(target, number)) {
            fprintf(stderr, "kill: %s: %s\n", argv[i], strerror(errno));
            result = 1;
        }
    }
    return result;
}
