/*
 * exec.h — shell-free command execution helpers.
 *
 * The historical `ejecutar_comando(fmt, ...)` helper formatted a string
 * and handed it to system(3), which spawned `/bin/sh -c <string>`. Any
 * value that came from the configuration file ended up evaluated by the
 * shell, so a path containing `;`, backticks, `$()` or simply spaces
 * resulted in either silent corruption or, in the worst case, arbitrary
 * code execution as root (zbd-system holds CAP_SYS_ADMIN).
 *
 * This module provides a strict, shell-free replacement. The caller
 * builds an explicit argv vector and the helper fork()s and execve()s
 * directly; values are passed as opaque arguments and the shell is
 * never involved.
 *
 * Conventions:
 *   - `argv[0]` MUST be the program name (typically the same as `file`).
 *   - The argv array MUST be NULL-terminated.
 *   - All API entry points are thread-safe (each call uses its own
 *     fork+pipe pair).
 */

#ifndef ZBD_EXEC_H
#define ZBD_EXEC_H

#include <stdio.h>
#include <sys/types.h>

/*
 * Run `file` with argv[] using execvp(3) (PATH lookup) and wait for it
 * to terminate. No shell is involved.
 *
 * Returns:
 *   >= 0  — the child's exit status (0 == success).
 *   -1    — fork(), waitpid() or execvp() failed before exec'ing
 *           (errno is preserved; the child exits with 127 in that case).
 */
int exec_cmd_argv(const char *file, char *const argv[]);

/*
 * Like exec_cmd_argv, but the child's stdout is redirected to a pipe
 * and the function returns a FILE* the caller can read from.
 *
 * On success, *out_pid is set to the child's pid; the caller MUST
 * eventually call exec_cmd_pipe_close() on the returned FILE* + pid to
 * reap the child and avoid zombies.
 *
 * Returns NULL on error (errno preserved).
 */
FILE *exec_cmd_pipe(const char *file, char *const argv[], pid_t *out_pid);

/*
 * Close a FILE* returned by exec_cmd_pipe and reap the child. The pid
 * argument is the one written by exec_cmd_pipe to *out_pid.
 *
 * Returns the child's exit status (>= 0) or -1 on error.
 */
int exec_cmd_pipe_close(FILE *fp, pid_t pid);

#endif /* ZBD_EXEC_H */
