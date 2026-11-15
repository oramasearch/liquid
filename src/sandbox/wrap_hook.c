// SPDX-License-Identifier: MIT

#if defined(__linux__)

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sandbox/wrap_plan.h"

static int liquid_wrap_traced;

static uint64_t monotonic_nanoseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void append_finished(void) {
    if (!liquid_wrap_traced) {
        return;
    }
    char line[128];
    const int size = snprintf(line,
                              sizeof(line),
                              "X\t%ld\t%llu\n",
                              (long)getpid(),
                              (unsigned long long)monotonic_nanoseconds());
    const char* trace_path = getenv("LIQUID_WRAP_TRACE");
    const int trace =
        open(trace_path == NULL ? "/liquid/trace" : trace_path, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (trace >= 0) {
        (void)write(trace, line, (size_t)size);
        close(trace);
    }
    liquid_wrap_traced = 0;
}

__attribute__((destructor)) static void liquid_wrap_leave(void) {
    append_finished();
}

static ssize_t read_all(const char* path, char* output, size_t capacity) {
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return -1;
    }
    const ssize_t size = read(descriptor, output, capacity);
    close(descriptor);
    return size;
}

static void append_hex(char* output, size_t* offset, const char* input, size_t size) {
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < size; ++index) {
        const unsigned char value = (unsigned char)input[index];
        output[(*offset)++] = digits[value >> 4U];
        output[(*offset)++] = digits[value & 0x0fU];
    }
}

static int bytes_contain(const char* input, size_t size, const char* needle) {
    const size_t needle_size = strlen(needle);
    if (needle_size == 0 || needle_size > size) {
        return 0;
    }
    for (size_t index = 0; index + needle_size <= size; ++index) {
        if (memcmp(input + index, needle, needle_size) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_configure_probe(const char* directory, const char* arguments, size_t argument_size) {
    return strstr(directory, "CMakeScratch/TryCompile-") != NULL ||
           strstr(directory, "CMakeTmp") != NULL ||
           bytes_contain(arguments, argument_size, "conftest.c") ||
           bytes_contain(arguments, argument_size, "conftest.cc") ||
           bytes_contain(arguments, argument_size, "conftest.cpp") ||
           bytes_contain(arguments, argument_size, "CMakeScratch/TryCompile-") ||
           bytes_contain(arguments, argument_size, "CMakeTmp") ||
           bytes_contain(arguments, argument_size, "cmTC_");
}

/* Only the plan liquid bind-mounts read-only at /liquid/plan is read, and only when its bytes
 * hash to the digest liquid passed beside it (liquid#401); LIQUID_WRAP_PLAN is not consulted.
 * Conditional inputs are read from the fixed mount, /liquid/work. */
static int planned_hit(const char* key) {
    size_t size = 0;
    char* plan =
        liquid_wrap_plan_load("/liquid/plan", getenv(LIQUID_WRAP_PLAN_DIGEST_VARIABLE), 0, &size);
    if (plan == NULL) {
        return -1;
    }
    const int status = liquid_wrap_plan_status(plan, size, key, "/liquid/work");
    free(plan);
    return status;
}

static long observed_pid(const char* path, long fallback) {
    char status[8192];
    const ssize_t size = read_all(path, status, sizeof(status) - 1);
    if (size <= 0) {
        return fallback;
    }
    status[size] = '\0';
    const char* line = strstr(status, "NSpid:");
    if (line == NULL) {
        return fallback;
    }
    line += strlen("NSpid:");
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    long value = fallback;
    for (;;) {
        char* end = NULL;
        const long next = strtol(line, &end, 10);
        if (end == line) {
            break;
        }
        value = next;
        line = end;
        while (*line == ' ' || *line == '\t') {
            ++line;
        }
    }
    return value;
}

__attribute__((constructor)) static void liquid_wrap_enter(void) {
    char executable[4096];
    char directory[4096];
    char arguments[32768];
    const ssize_t executable_size = readlink("/proc/self/exe", executable, sizeof(executable));
    const ssize_t argument_size = read_all("/proc/self/cmdline", arguments, sizeof(arguments));
    if (executable_size <= 0 || argument_size <= 0 ||
        getcwd(directory, sizeof(directory)) == NULL) {
        return;
    }
    const size_t directory_size = strlen(directory);
    /* BLAKE3-256: a 64-bit key could be collided with a cached command's (liquid#449). */
    char key[LIQUID_WRAP_KEY_HEX_SIZE + 1];
    liquid_wrap_process_key_hex(executable,
                                (size_t)executable_size,
                                directory,
                                directory_size,
                                arguments,
                                (size_t)argument_size,
                                key);

    char line[81920];
    const long pid = observed_pid("/proc/self/status", (long)getpid());
    char parent_path[128];
    (void)snprintf(parent_path, sizeof(parent_path), "/proc/%ld/status", (long)getppid());
    const long parent = observed_pid(parent_path, (long)getppid());
    const int prefix = snprintf(line, sizeof(line), "%ld\t%ld\t%s\t", pid, parent, key);
    if (prefix > 0 && (size_t)prefix < sizeof(line)) {
        size_t offset = (size_t)prefix;
        append_hex(line, &offset, executable, (size_t)executable_size);
        line[offset++] = '\t';
        append_hex(line, &offset, directory, directory_size);
        line[offset++] = '\t';
        append_hex(line, &offset, arguments, (size_t)argument_size);
        const int suffix = snprintf(line + offset,
                                    sizeof(line) - offset,
                                    "\t%llu\n",
                                    (unsigned long long)monotonic_nanoseconds());
        if (suffix <= 0 || (size_t)suffix >= sizeof(line) - offset) {
            return;
        }
        offset += (size_t)suffix;
        const char* trace_path = getenv("LIQUID_WRAP_TRACE");
        const int trace = open(trace_path == NULL ? "/liquid/trace" : trace_path,
                               O_WRONLY | O_APPEND | O_CLOEXEC);
        if (trace >= 0) {
            (void)write(trace, line, offset);
            close(trace);
            liquid_wrap_traced = 1;
        }
    }

    if (getenv("LIQUID_WRAP_REFUSE_PROBES") != NULL &&
        is_configure_probe(directory, arguments, (size_t)argument_size)) {
        _exit(125);
    }
    const int status = planned_hit(key);
    if (status >= 0) {
        append_finished();
        _exit(status);
    }
}

#elif defined(__APPLE__)

#include <crt_externs.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sandbox/wrap_plan.h"

static int liquid_wrap_traced;

static uint64_t monotonic_nanoseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void append_finished(void) {
    if (!liquid_wrap_traced) {
        return;
    }
    char line[128];
    const int size = snprintf(line,
                              sizeof(line),
                              "X\t%ld\t%llu\n",
                              (long)getpid(),
                              (unsigned long long)monotonic_nanoseconds());
    const char* trace_path = getenv("LIQUID_WRAP_TRACE");
    if (trace_path != NULL) {
        const int trace = open(trace_path, O_WRONLY | O_APPEND | O_CLOEXEC);
        if (trace >= 0) {
            (void)write(trace, line, (size_t)size);
            close(trace);
        }
    }
    liquid_wrap_traced = 0;
}

__attribute__((destructor)) static void liquid_wrap_leave(void) {
    append_finished();
}

static void append_hex(char* output, size_t* offset, const char* input, size_t size) {
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < size; ++index) {
        const unsigned char value = (unsigned char)input[index];
        output[(*offset)++] = digits[value >> 4U];
        output[(*offset)++] = digits[value & 0x0fU];
    }
}

static int bytes_contain(const char* input, size_t size, const char* needle) {
    const size_t needle_size = strlen(needle);
    if (needle_size == 0 || needle_size > size) {
        return 0;
    }
    for (size_t index = 0; index + needle_size <= size; ++index) {
        if (memcmp(input + index, needle, needle_size) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_configure_probe(const char* directory, const char* arguments, size_t argument_size) {
    return strstr(directory, "CMakeScratch/TryCompile-") != NULL ||
           strstr(directory, "CMakeTmp") != NULL ||
           bytes_contain(arguments, argument_size, "conftest.c") ||
           bytes_contain(arguments, argument_size, "conftest.cc") ||
           bytes_contain(arguments, argument_size, "conftest.cpp") ||
           bytes_contain(arguments, argument_size, "CMakeScratch/TryCompile-") ||
           bytes_contain(arguments, argument_size, "CMakeTmp") ||
           bytes_contain(arguments, argument_size, "cmTC_");
}

/* macOS has no mount namespace, so the plan is trusted only at liquid's own location, beside the
 * input mount (<run>/plan next to <run>/mount), as a private file of this user, and only when its
 * bytes hash to the digest liquid passed beside it (liquid#401). */
static int planned_hit(const char* key) {
    const char* path = getenv("LIQUID_WRAP_PLAN");
    const char* root = getenv("LIQUID_WRAP_ROOT");
    if (path == NULL || root == NULL || root[0] != '/') {
        return -1;
    }
    const char* slash = strrchr(root, '/');
    char expected[8192];
    const int expected_size =
        snprintf(expected, sizeof(expected), "%.*s/plan", (int)(slash - root), root);
    if (expected_size <= 0 || (size_t)expected_size >= sizeof(expected) ||
        strcmp(path, expected) != 0) {
        return -1;
    }
    size_t size = 0;
    char* plan = liquid_wrap_plan_load(path, getenv(LIQUID_WRAP_PLAN_DIGEST_VARIABLE), 1, &size);
    if (plan == NULL) {
        return -1;
    }
    const int status = liquid_wrap_plan_status(plan, size, key, root);
    free(plan);
    return status;
}

__attribute__((constructor)) static void liquid_wrap_enter(void) {
    char executable[4096];
    uint32_t executable_capacity = sizeof(executable);
    char directory[4096];
    char arguments[32768];
    if (_NSGetExecutablePath(executable, &executable_capacity) != 0 ||
        getcwd(directory, sizeof(directory)) == NULL) {
        return;
    }
    const char* root = getenv("LIQUID_WRAP_ROOT");
    if (root != NULL) {
        const size_t root_size = strlen(root);
        if (strncmp(directory, root, root_size) == 0 &&
            (directory[root_size] == '\0' || directory[root_size] == '/')) {
            const char* suffix = directory + root_size;
            memmove(directory + strlen("/liquid/work"), suffix, strlen(suffix) + 1);
            memcpy(directory, "/liquid/work", strlen("/liquid/work"));
        }
    }
    char** argv = *_NSGetArgv();
    const int argc = *_NSGetArgc();
    size_t argument_size = 0;
    for (int index = 0; index < argc; ++index) {
        const size_t size = strlen(argv[index]) + 1;
        if (argument_size + size > sizeof(arguments)) {
            return;
        }
        memcpy(arguments + argument_size, argv[index], size);
        argument_size += size;
    }
    const size_t executable_size = strlen(executable);
    const size_t directory_size = strlen(directory);
    /* BLAKE3-256: a 64-bit key could be collided with a cached command's (liquid#449). */
    char key[LIQUID_WRAP_KEY_HEX_SIZE + 1];
    liquid_wrap_process_key_hex(
        executable, executable_size, directory, directory_size, arguments, argument_size, key);

    char line[81920];
    const int prefix =
        snprintf(line, sizeof(line), "%ld\t%ld\t%s\t", (long)getpid(), (long)getppid(), key);
    if (prefix > 0 && (size_t)prefix < sizeof(line)) {
        size_t offset = (size_t)prefix;
        append_hex(line, &offset, executable, executable_size);
        line[offset++] = '\t';
        append_hex(line, &offset, directory, directory_size);
        line[offset++] = '\t';
        append_hex(line, &offset, arguments, argument_size);
        const int suffix = snprintf(line + offset,
                                    sizeof(line) - offset,
                                    "\t%llu\n",
                                    (unsigned long long)monotonic_nanoseconds());
        if (suffix <= 0 || (size_t)suffix >= sizeof(line) - offset) {
            return;
        }
        offset += (size_t)suffix;
        const char* trace_path = getenv("LIQUID_WRAP_TRACE");
        if (trace_path != NULL) {
            const int trace = open(trace_path, O_WRONLY | O_APPEND | O_CLOEXEC);
            if (trace >= 0) {
                (void)write(trace, line, offset);
                close(trace);
                liquid_wrap_traced = 1;
            }
        }
    }
    if (getenv("LIQUID_WRAP_REFUSE_PROBES") != NULL &&
        is_configure_probe(directory, arguments, argument_size)) {
        _exit(125);
    }
    const int status = planned_hit(key);
    if (status >= 0) {
        append_finished();
        _exit(status);
    }
}

#else

int liquid_wrap_hook_unavailable;

#endif
