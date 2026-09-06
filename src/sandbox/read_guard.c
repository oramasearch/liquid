#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
#include <sys/user.h>
#elif defined(__aarch64__)
#include <asm/ptrace.h>
#include <elf.h>
#else
#error "liquid read guard requires x86_64 or aarch64"
#endif

enum { guard_refusal_exit = 125, maximum_tracees = 1024 };

struct tracee_state {
    pid_t pid;
    int entering;
    int refused;
};

struct syscall_state {
    long number;
    unsigned long arguments[6];
};

static int get_syscall(pid_t pid, struct syscall_state* output) {
#if defined(__x86_64__)
    struct user_regs_struct registers;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &registers) < 0) {
        return -1;
    }
    output->number = (long)registers.orig_rax;
    output->arguments[0] = registers.rdi;
    output->arguments[1] = registers.rsi;
    output->arguments[2] = registers.rdx;
    output->arguments[3] = registers.r10;
    output->arguments[4] = registers.r8;
    output->arguments[5] = registers.r9;
#else
    struct user_pt_regs registers;
    struct iovec vector = {&registers, sizeof(registers)};
    if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &vector) < 0) {
        return -1;
    }
    output->number = (long)registers.regs[8];
    for (size_t index = 0; index != 6; ++index) {
        output->arguments[index] = registers.regs[index];
    }
#endif
    return 0;
}

static int replace_syscall(pid_t pid, long number) {
#if defined(__x86_64__)
    struct user_regs_struct registers;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &registers) < 0) {
        return -1;
    }
    registers.orig_rax = (unsigned long)number;
    return ptrace(PTRACE_SETREGS, pid, NULL, &registers) < 0 ? -1 : 0;
#else
    struct user_pt_regs registers;
    struct iovec vector = {&registers, sizeof(registers)};
    if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &vector) < 0) {
        return -1;
    }
    registers.regs[8] = (unsigned long)number;
    return ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &vector) < 0 ? -1 : 0;
#endif
}

static int replace_result(pid_t pid, long result) {
#if defined(__x86_64__)
    struct user_regs_struct registers;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &registers) < 0) {
        return -1;
    }
    registers.rax = (unsigned long)result;
    return ptrace(PTRACE_SETREGS, pid, NULL, &registers) < 0 ? -1 : 0;
#else
    struct user_pt_regs registers;
    struct iovec vector = {&registers, sizeof(registers)};
    if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &vector) < 0) {
        return -1;
    }
    registers.regs[0] = (unsigned long)result;
    return ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &vector) < 0 ? -1 : 0;
#endif
}

static int read_tracee_string(pid_t pid, unsigned long address, char* output, size_t capacity) {
    size_t offset = 0;
    while (offset < capacity) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, (void*)(address + offset), NULL);
        if (word == -1 && errno != 0) {
            return -1;
        }
        size_t count = sizeof(word);
        if (count > capacity - offset) {
            count = capacity - offset;
        }
        memcpy(output + offset, &word, count);
        if (memchr(&word, '\0', count) != NULL) {
            return 0;
        }
        offset += count;
    }
    output[capacity - 1] = '\0';
    return -1;
}

static int normalize_absolute(const char* input, char* output, size_t capacity) {
    char copy[PATH_MAX];
    char* components[PATH_MAX / 2];
    size_t count = 0;
    char* cursor;
    char* token;
    if (input[0] != '/' || strlen(input) >= sizeof(copy)) {
        return -1;
    }
    strcpy(copy, input);
    cursor = copy;
    while ((token = strsep(&cursor, "/")) != NULL) {
        if (token[0] == '\0' || strcmp(token, ".") == 0) {
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (count != 0) {
                --count;
            }
            continue;
        }
        components[count++] = token;
    }
    if (capacity < 2) {
        return -1;
    }
    output[0] = '/';
    output[1] = '\0';
    for (size_t index = 0; index != count; ++index) {
        size_t used = strlen(output);
        size_t needed = strlen(components[index]) + (used == 1 ? 0 : 1);
        if (used + needed >= capacity) {
            return -1;
        }
        if (used != 1) {
            strcat(output, "/");
        }
        strcat(output, components[index]);
    }
    return 0;
}

static int tracee_base(pid_t pid, int descriptor, char* output, size_t capacity) {
    char link[64];
    if (descriptor == AT_FDCWD) {
        snprintf(link, sizeof(link), "/proc/%ld/cwd", (long)pid);
    } else {
        snprintf(link, sizeof(link), "/proc/%ld/fd/%d", (long)pid, descriptor);
    }
    ssize_t length = readlink(link, output, capacity - 1);
    if (length < 0 || (size_t)length >= capacity) {
        return -1;
    }
    output[length] = '\0';
    return 0;
}

static int
resolve_path(pid_t pid, int descriptor, const char* path, char* output, size_t capacity) {
    char combined[PATH_MAX];
    if (path[0] == '/') {
        return normalize_absolute(path, output, capacity);
    }
    char base[PATH_MAX];
    if (tracee_base(pid, descriptor, base, sizeof(base)) < 0) {
        return -1;
    }
    if (snprintf(combined, sizeof(combined), "%s/%s", base, path) >= (int)sizeof(combined)) {
        return -1;
    }
    return normalize_absolute(combined, output, capacity);
}

static int path_is_within(const char* path, const char* root) {
    size_t length = strlen(root);
    return strncmp(path, root, length) == 0 && (path[length] == '\0' || path[length] == '/');
}

static int is_file_open(long number) {
#if defined(__NR_open)
    if (number == __NR_open) {
        return 1;
    }
#endif
    return number == __NR_openat || number == __NR_openat2;
}

static int inspect_open(pid_t pid,
                        const struct syscall_state* call,
                        const char* work_root,
                        const char* toolchain_root,
                        char* refused,
                        size_t refused_size) {
    unsigned long address;
    int descriptor;
#if defined(__NR_open)
    if (call->number == __NR_open) {
        address = call->arguments[0];
        descriptor = AT_FDCWD;
    } else
#endif
    {
        address = call->arguments[1];
        descriptor = (int)call->arguments[0];
    }
    char raw[PATH_MAX];
    char resolved[PATH_MAX];
    if (read_tracee_string(pid, address, raw, sizeof(raw)) < 0) {
        return -1;
    }
    if (resolve_path(pid, descriptor, raw, resolved, sizeof(resolved)) < 0) {
        return -1;
    }
    if (path_is_within(resolved, work_root) || path_is_within(resolved, toolchain_root) ||
        path_is_within(resolved, "/lib") || path_is_within(resolved, "/lib64") ||
        strcmp(resolved, "/etc/ld.so.cache") == 0 || strcmp(resolved, "/etc/os-release") == 0 ||
        strcmp(resolved, "/etc/lsb-release") == 0 || strcmp(resolved, "/etc/debian_version") == 0 ||
        strcmp(resolved, "/etc/redhat-release") == 0 || strcmp(resolved, "/etc/alpine-release") == 0) {
        return 0;
    }
    snprintf(refused, refused_size, "%s", resolved);
    return 1;
}

static struct tracee_state* find_tracee(struct tracee_state* states, size_t count, pid_t pid) {
    for (size_t index = 0; index != count; ++index) {
        if (states[index].pid == pid) {
            return &states[index];
        }
    }
    return NULL;
}

static int write_refusal(const char* report, const char* path) {
    FILE* output = fopen(report, "w");
    if (output == NULL) {
        return -1;
    }
    int failed = fprintf(output, "%s\n", path) < 0 || fclose(output) != 0;
    return failed ? -1 : 0;
}

static int trace_command(char** command,
                         const char* report,
                         const char* work_root,
                         const char* toolchain_root) {
    pid_t child = fork();
    if (child < 0) {
        return 126;
    }
    if (child == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
            _exit(126);
        }
        raise(SIGSTOP);
        execvp(command[0], command);
        _exit(errno == ENOENT ? 127 : 126);
    }

    struct tracee_state states[maximum_tracees] = {{child, 1, 0}};
    size_t state_count = 1;
    size_t live = 1;
    int command_exit = 126;
    int violation = 0;
    int status;
    if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status)) {
        return 126;
    }
    long options = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK |
                   PTRACE_O_TRACEVFORK | PTRACE_O_EXITKILL;
    if (ptrace(PTRACE_SETOPTIONS, child, NULL, options) < 0 ||
        ptrace(PTRACE_SYSCALL, child, NULL, NULL) < 0) {
        return 126;
    }

    while (live != 0) {
        pid_t pid = waitpid(-1, &status, __WALL);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 126;
        }
        struct tracee_state* state = find_tracee(states, state_count, pid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (pid == child) {
                command_exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            if (state != NULL) {
                state->pid = -1;
                --live;
            }
            continue;
        }
        if (state == NULL && state_count < maximum_tracees) {
            states[state_count] = (struct tracee_state){pid, 1, 0};
            state = &states[state_count++];
            ++live;
            ptrace(PTRACE_SETOPTIONS, pid, NULL, options);
        }
        if (state == NULL) {
            return 126;
        }
        int signal = WSTOPSIG(status);
        if (signal == (SIGTRAP | 0x80)) {
            if (state->entering) {
                struct syscall_state call;
                if (get_syscall(pid, &call) < 0) {
                    return 126;
                }
                if (is_file_open(call.number)) {
                    char refused[PATH_MAX];
                    int decision = inspect_open(
                        pid, &call, work_root, toolchain_root, refused, sizeof(refused));
                    if (decision < 0) {
                        return 126;
                    }
                    if (decision > 0) {
                        if (!violation && write_refusal(report, refused) < 0) {
                            return 126;
                        }
                        violation = 1;
                        state->refused = 1;
                        if (replace_syscall(pid, -1) < 0) {
                            return 126;
                        }
                    }
                }
            } else if (state->refused) {
                if (replace_result(pid, -EACCES) < 0) {
                    return 126;
                }
                state->refused = 0;
            }
            state->entering = !state->entering;
            signal = 0;
        } else if (signal == SIGSTOP || signal == SIGTRAP) {
            signal = 0;
        }
        if (ptrace(PTRACE_SYSCALL, pid, NULL, signal) < 0 && errno != ESRCH) {
            return 126;
        }
    }
    return violation ? guard_refusal_exit : command_exit;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        return 64;
    }
    return trace_command(&argv[4], argv[1], argv[2], argv[3]);
}
