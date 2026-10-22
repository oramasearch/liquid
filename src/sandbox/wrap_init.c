#define _GNU_SOURCE
#include <errno.h>
#include <linux/capability.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* PID 1 accepts ordinary signals only when a handler is installed. They stay blocked and
 * are consumed synchronously by sigwaitinfo below. */
static void queued_signal(int number) {
    (void)number;
}

/* PID 1 owns all descendants, including commands that daemonize into another session.
 * unshare --kill-child handles a killed launcher; PDEATHSIG also covers its early exit. */
int main(int argc, char** argv) {
    if (argc >= 5 && strcmp(argv[1], "--launch") == 0) {
        char* end;
        const long parent = strtol(argv[2], &end, 10);
        if (*end != '\0' || parent <= 0 || prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
            getppid() != parent)
            return 125;
        execv(argv[3], argv + 3);
        return 127;
    }
    if (argc < 2 || getpid() != 1)
        return 125;
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || prctl(PR_SET_DUMPABLE, 0) != 0 || getppid() != 0)
        return 125;
    sigset_t signals;
    sigemptyset(&signals);
    const int caught[] = {SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    struct sigaction action = {0};
    action.sa_handler = queued_signal;
    sigemptyset(&action.sa_mask);
    for (size_t index = 0; index < sizeof(caught) / sizeof(caught[0]); ++index) {
        sigaddset(&signals, caught[index]);
        if (sigaction(caught[index], &action, NULL) != 0)
            return 125;
    }
    if (sigprocmask(SIG_BLOCK, &signals, NULL) != 0)
        return 125;
    const pid_t child = fork();
    if (child < 0)
        return 125;
    if (child == 0) {
        /* The command cannot rewrite PID 1's memory or disable its parent-death cleanup.
         * Removing the bounding bit also prevents regaining this capability on exec. */
        struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
        struct __user_cap_data_struct capabilities[2];
        if (prctl(PR_CAPBSET_DROP, CAP_SYS_PTRACE) != 0 ||
            syscall(SYS_capget, &header, capabilities) != 0)
            _exit(125);
        const unsigned int mask = ~(1U << (CAP_SYS_PTRACE % 32));
        capabilities[CAP_SYS_PTRACE / 32].effective &= mask;
        capabilities[CAP_SYS_PTRACE / 32].permitted &= mask;
        capabilities[CAP_SYS_PTRACE / 32].inheritable &= mask;
        if (syscall(SYS_capset, &header, capabilities) != 0)
            _exit(125);
        action.sa_handler = SIG_DFL;
        for (size_t index = 0; index < sizeof(caught) / sizeof(caught[0]); ++index) {
            if (sigaction(caught[index], &action, NULL) != 0)
                _exit(125);
        }
        if (sigprocmask(SIG_UNBLOCK, &signals, NULL) != 0)
            _exit(125);
        execvp(argv[1], argv + 1);
        _exit(127);
    }
    for (;;) {
        int status;
        pid_t reaped;
        while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) {
            if (reaped == child) {
                /* Exiting PID 1 makes the kernel kill every remaining namespace member. */
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
        }
        if (reaped < 0 && errno != EINTR)
            return 125;
        const int signal = sigwaitinfo(&signals, NULL);
        if (signal < 0) {
            if (errno == EINTR)
                continue;
            return 125;
        }
        if (signal != SIGCHLD) {
            /* Cancellation is bounded even if a command ignores the forwarded signal. */
            kill(-1, signal);
            kill(-1, SIGKILL);
            while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {
            }
            return 128 + signal;
        }
    }
}
