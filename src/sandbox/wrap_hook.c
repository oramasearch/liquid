#if defined(__linux__)

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static uint64_t fnv_update(uint64_t value, const void* data, size_t size) {
    const unsigned char* bytes = data;
    for (size_t index = 0; index < size; ++index) {
        value ^= bytes[index];
        value *= UINT64_C(1099511628211);
    }
    return value;
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

static int planned_hit(uint64_t key) {
    const char* path = getenv("LIQUID_WRAP_PLAN");
    FILE* input = fopen(path == NULL ? "/liquid/plan" : path, "r");
    if (input == NULL) {
        return -1;
    }
    unsigned long long candidate = 0;
    int status = 0;
    while (fscanf(input, "%llx\t%d", &candidate, &status) == 2) {
        if ((uint64_t)candidate == key) {
            fclose(input);
            return status;
        }
    }
    fclose(input);
    return -1;
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
    uint64_t key = UINT64_C(1469598103934665603);
    key = fnv_update(key, executable, (size_t)executable_size);
    key = fnv_update(key, "\0", 1);
    key = fnv_update(key, directory, directory_size);
    key = fnv_update(key, "\0", 1);
    key = fnv_update(key, arguments, (size_t)argument_size);

    char line[81920];
    const long pid = observed_pid("/proc/self/status", (long)getpid());
    char parent_path[128];
    (void)snprintf(parent_path, sizeof(parent_path), "/proc/%ld/status", (long)getppid());
    const long parent = observed_pid(parent_path, (long)getppid());
    const int prefix =
        snprintf(line, sizeof(line), "%ld\t%ld\t%016llx\t", pid, parent, (unsigned long long)key);
    if (prefix > 0 && (size_t)prefix < sizeof(line)) {
        size_t offset = (size_t)prefix;
        append_hex(line, &offset, executable, (size_t)executable_size);
        line[offset++] = '\t';
        append_hex(line, &offset, directory, directory_size);
        line[offset++] = '\t';
        append_hex(line, &offset, arguments, (size_t)argument_size);
        line[offset++] = '\n';
        const char* trace_path = getenv("LIQUID_WRAP_TRACE");
        const int trace = open(trace_path == NULL ? "/liquid/trace" : trace_path,
                               O_WRONLY | O_APPEND | O_CLOEXEC);
        if (trace >= 0) {
            (void)write(trace, line, offset);
            close(trace);
        }
    }

    const int status = planned_hit(key);
    if (status >= 0) {
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
#include <unistd.h>

static uint64_t fnv_update(uint64_t value, const void* data, size_t size) {
    const unsigned char* bytes = data;
    for (size_t index = 0; index < size; ++index) {
        value ^= bytes[index];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static void append_hex(char* output, size_t* offset, const char* input, size_t size) {
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < size; ++index) {
        const unsigned char value = (unsigned char)input[index];
        output[(*offset)++] = digits[value >> 4U];
        output[(*offset)++] = digits[value & 0x0fU];
    }
}

static int planned_hit(uint64_t key) {
    const char* path = getenv("LIQUID_WRAP_PLAN");
    if (path == NULL) {
        return -1;
    }
    FILE* input = fopen(path, "r");
    if (input == NULL) {
        return -1;
    }
    unsigned long long candidate = 0;
    int status = 0;
    while (fscanf(input, "%llx\t%d", &candidate, &status) == 2) {
        if ((uint64_t)candidate == key) {
            fclose(input);
            return status;
        }
    }
    fclose(input);
    return -1;
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
    uint64_t key = UINT64_C(1469598103934665603);
    key = fnv_update(key, executable, executable_size);
    key = fnv_update(key, "\0", 1);
    key = fnv_update(key, directory, directory_size);
    key = fnv_update(key, "\0", 1);
    key = fnv_update(key, arguments, argument_size);

    char line[81920];
    const int prefix = snprintf(line,
                                sizeof(line),
                                "%ld\t%ld\t%016llx\t",
                                (long)getpid(),
                                (long)getppid(),
                                (unsigned long long)key);
    if (prefix > 0 && (size_t)prefix < sizeof(line)) {
        size_t offset = (size_t)prefix;
        append_hex(line, &offset, executable, executable_size);
        line[offset++] = '\t';
        append_hex(line, &offset, directory, directory_size);
        line[offset++] = '\t';
        append_hex(line, &offset, arguments, argument_size);
        line[offset++] = '\n';
        const char* trace_path = getenv("LIQUID_WRAP_TRACE");
        if (trace_path != NULL) {
            const int trace = open(trace_path, O_WRONLY | O_APPEND | O_CLOEXEC);
            if (trace >= 0) {
                (void)write(trace, line, offset);
                close(trace);
            }
        }
    }
    const int status = planned_hit(key);
    if (status >= 0) {
        _exit(status);
    }
}

#else

int liquid_wrap_hook_unavailable;

#endif
