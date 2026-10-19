/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#include "common/utilities/common.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static volatile LONG interrupted;
static BOOL WINAPI stage_interrupt(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&interrupted, 1);
    return TRUE;
}
static wchar_t* stage_wide(const char* text) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t* out;
    if (!n) return NULL;
    out = (wchar_t*)snx_alloc((size_t)n * sizeof(*out));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, n);
    return out;
}
/* Quote argv according to the Windows CRT rules, never through a shell. */
static wchar_t* stage_command(const char* const* argv) {
    size_t cap = 1, i, j, slashes, n = 0;
    char* text;
    wchar_t* out;
    for (i = 0; argv[i]; ++i) cap += 2 * strlen(argv[i]) + 4;
    text = (char*)snx_alloc(cap);
    for (i = 0; argv[i]; ++i) {
        if (i) text[n++] = ' ';
        text[n++] = '"';
        slashes = 0;
        for (j = 0; argv[i][j]; ++j) {
            if (argv[i][j] == '\\') {
                ++slashes;
                continue;
            }
            if (argv[i][j] == '"') slashes = slashes * 2 + 1;
            while (slashes) {
                text[n++] = '\\';
                --slashes;
            }
            text[n++] = argv[i][j];
        }
        slashes *= 2;
        while (slashes) {
            text[n++] = '\\';
            --slashes;
        }
        text[n++] = '"';
    }
    text[n] = 0;
    out = stage_wide(text);
    free(text);
    return out;
}
static int run_stage(const char* const* argv, const char* cwd, const char* stage, int debugger) {
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    HANDLE job;
    wchar_t *command = stage_command(argv), *directory = cwd ? stage_wide(cwd) : NULL;
    DWORD code = 1, error = 0, waited;
    int rc = 1, handler = 0;
    (void)debugger;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    job = CreateJobObjectW(NULL, NULL);
    interrupted = 0;
    if (!command || (cwd && !directory) || !job ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        goto failed;
    handler = SetConsoleCtrlHandler(stage_interrupt, TRUE);
    if (!handler) goto failed;
    /* Assign the suspended child before it can create compilers or extraction subprocesses. */
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, NULL, directory,
                        &startup, &process))
        goto failed;
    if (!AssignProcessToJobObject(job, process.hProcess) || ResumeThread(process.hThread) == (DWORD)-1) {
        error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        goto failed;
    }
    for (;;) {
        if (InterlockedCompareExchange(&interrupted, 0, 0)) {
            TerminateJobObject(job, 130);
            WaitForSingleObject(process.hProcess, INFINITE);
            snx_error("%s interrupted. You can run the same command again.", stage);
            rc = 130;
            goto done;
        }
        waited = WaitForSingleObject(process.hProcess, 50);
        if (waited == WAIT_OBJECT_0) break;
        if (waited != WAIT_TIMEOUT) goto failed;
    }
    if (!GetExitCodeProcess(process.hProcess, &code)) goto failed;
    /* Negative driver results mean "not a tool command" to the runtime entry point. */
    rc = code <= 255 ? (int)code : 1;
    if (rc) snx_error("%s failed (exit status %lu). You can run the same command again.", stage, (unsigned long)code);
    goto done;
failed:
    if (!error) error = GetLastError();
    snx_error("%s: cannot start or wait for %s%s%s (Windows error %lu)", stage, argv[0], cwd ? " in " : "",
              cwd ? cwd : "", (unsigned long)error);
done:
    if (job) CloseHandle(job);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (handler) SetConsoleCtrlHandler(stage_interrupt, FALSE);
    free(command);
    free(directory);
    return rc;
}
#else
static volatile sig_atomic_t stage_pid, interrupted;
static void stage_interrupt(int sig) {
    int saved = errno;
    interrupted = sig;
    if (stage_pid > 0) {
        kill(-(pid_t)stage_pid, sig);
        if (sig == SIGTERM || sig == SIGHUP || sig == SIGINT) kill(-(pid_t)stage_pid, SIGCONT);
    }
    errno = saved;
}
static int run_stage(const char* const* argv, const char* cwd, const char* stage, int debugger) {
    static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGTSTP};
    struct sigaction action, old[4], ignore, old_ttou;
    sigset_t blocked, previous;
    pid_t pid, waited, foreground = -1;
    int status = 0, rc = 1, sig = 0, installed = 0, tty = 0;
    size_t i;
    sigemptyset(&blocked);
    for (i = 0; i < 4; ++i) sigaddset(&blocked, signals[i]);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous)) {
        perror("scarletnx: blocking stage signals");
        return 1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = stage_interrupt;
    sigemptyset(&action.sa_mask);
    interrupted = 0;
    stage_pid = 0;
    for (i = 0; i < 4; ++i) {
        if (sigaction(signals[i], &action, &old[i])) goto setup_failed;
        ++installed;
    }
    /* Give the child the terminal while it runs, so interactive games can read stdin. */
    foreground = tcgetpgrp(STDIN_FILENO);
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (foreground == getpgrp()) {
        if (sigaction(SIGTTOU, &ignore, &old_ttou)) goto setup_failed;
        tty = 1;
    }
    fflush(NULL);
    pid = fork();
    if (!pid) {
        if (setpgid(0, 0)) {
            perror("scarletnx: creating child process group");
            _exit(126);
        }
        if (tty && tcsetpgrp(STDIN_FILENO, getpgrp())) {
            perror("scarletnx: assigning child terminal");
            _exit(126);
        }
        for (i = 0; i < 4; ++i) {
            action.sa_handler = SIG_DFL;
            sigaction(signals[i], &action, NULL);
        }
        if (tty) sigaction(SIGTTOU, &old_ttou, NULL);
        sigprocmask(SIG_SETMASK, &previous, NULL);
        if (cwd && chdir(cwd)) {
            perror("scarletnx: chdir to the child's working directory");
            _exit(126);
        }
        execvp(argv[0], (char* const*)argv);
        perror("scarletnx: executing child process");
        _exit(127);
    }
    if (pid < 0) goto setup_failed;
    /* Either side may win the race to setpgid. The child also does it before exec. */
    setpgid(pid, pid);
    stage_pid = (sig_atomic_t)pid;
    sigprocmask(SIG_SETMASK, &previous, NULL);
    /* Debugger stops are intentional, unlike a suspended extraction/compiler. */
    do { waited = waitpid(pid, &status, debugger ? 0 : WUNTRACED); } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        perror("scarletnx: waiting for child process");
    else if (WIFSTOPPED(status))
        sig = WSTOPSIG(status);
    else if (WIFSIGNALED(status))
        sig = WTERMSIG(status);
    else if (WIFEXITED(status))
        rc = WEXITSTATUS(status);
    /* Stop/abort is a failed stage, not a checkpoint. Also stop any outstanding workers. */
    sigprocmask(SIG_BLOCK, &blocked, NULL);
    stage_pid = 0;
    if (interrupted) sig = (int)interrupted;
    if (sig || rc) kill(-pid, SIGKILL);
    if (waited > 0 && WIFSTOPPED(status)) {
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    }
    if (sig) {
        rc = 128 + sig;
        snx_error("%s interrupted by signal %d (%s). You can run the same command again.", stage, sig, strsignal(sig));
    } else if (rc)
        snx_error("%s failed (exit status %d). You can run the same command again.", stage, rc);
    goto done;
setup_failed:
    perror("scarletnx: starting child process");
done:
    if (tty) {
        if (tcsetpgrp(STDIN_FILENO, foreground)) perror("scarletnx: restoring terminal");
        sigaction(SIGTTOU, &old_ttou, NULL);
    }
    for (i = 0; i < (size_t)installed; ++i) sigaction(signals[i], &old[i], NULL);
    sigprocmask(SIG_SETMASK, &previous, NULL);
    return rc;
}
#endif
int snx_run_stage(const char* const* argv, const char* cwd, const char* stage) {
    return run_stage(argv, cwd, stage, 0);
}
int snx_run_debug_stage(const char* const* argv, const char* cwd, const char* stage) {
    return run_stage(argv, cwd, stage, 1);
}
