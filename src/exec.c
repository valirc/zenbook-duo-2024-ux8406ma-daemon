/*
 * exec.c — implementation of the shell-free execution helpers.
 *
 * See exec.h for the API contract.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "exec.h"

/* Helper to log the argv for debugging without quoting issues. */
static void log_invocation(const char *file, char *const argv[])
{
    fprintf(stderr, "exec:");
    for (size_t i = 0; argv && argv[i]; ++i)
    {
        fprintf(stderr, " %s", argv[i]);
    }
    if (file && (!argv || !argv[0]))
    {
        fprintf(stderr, " %s", file);
    }
    fprintf(stderr, "\n");
}

int exec_cmd_argv(const char *file, char *const argv[])
{
    if (!file || !argv || !argv[0])
    {
        errno = EINVAL;
        return -1;
    }

    log_invocation(file, argv);

    pid_t pid = fork();
    if (pid < 0)
    {
        return -1;
    }
    if (pid == 0)
    {
        /* Child: replace image. */
        execvp(file, argv);
        /* execvp only returns on failure. */
        fprintf(stderr, "exec_cmd_argv: execvp(%s) fallo: %s\n",
                file, strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno == EINTR) continue;
        return -1;
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        /* Convention: encode the signal in negative space (-1 - signo)
         * so callers that only care about "did it succeed?" can still
         * test for `!= 0`. */
        return -1 - WTERMSIG(status);
    }
    return -1;
}

FILE *exec_cmd_pipe(const char *file, char *const argv[], pid_t *out_pid)
{
    if (!file || !argv || !argv[0] || !out_pid)
    {
        errno = EINVAL;
        return NULL;
    }

    log_invocation(file, argv);

    int pipefd[2];
    if (pipe(pipefd) < 0)
    {
        return NULL;
    }

    /* Make the read end close-on-exec so other fork()s do not inherit it. */
    int flags = fcntl(pipefd[0], F_GETFD);
    if (flags >= 0)
    {
        fcntl(pipefd[0], F_SETFD, flags | FD_CLOEXEC);
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        int saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        errno = saved;
        return NULL;
    }

    if (pid == 0)
    {
        /* Child: redirect stdout to the pipe, close the read end. */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
        {
            fprintf(stderr, "exec_cmd_pipe: dup2 fallo: %s\n", strerror(errno));
            _exit(127);
        }
        close(pipefd[1]);
        execvp(file, argv);
        fprintf(stderr, "exec_cmd_pipe: execvp(%s) fallo: %s\n",
                file, strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);
    *out_pid = pid;
    FILE *fp = fdopen(pipefd[0], "r");
    if (!fp)
    {
        int saved = errno;
        close(pipefd[0]);
        errno = saved;
        return NULL;
    }
    return fp;
}

int exec_cmd_pipe_close(FILE *fp, pid_t pid)
{
    if (fp)
    {
        fclose(fp);
    }
    if (pid <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno == EINTR) continue;
        return -1;
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        return -1 - WTERMSIG(status);
    }
    return -1;
}
