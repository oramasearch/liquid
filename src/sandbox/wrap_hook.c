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

static int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

static int conditional_input_matches(const char* encoded,
                                     uint64_t expected_fingerprint,
                                     uint64_t expected_size) {
    char relative[4096];
    const size_t encoded_size = strlen(encoded);
    if (encoded_size == 0 || encoded_size % 2 != 0 || encoded_size / 2 >= sizeof(relative)) {
        return 0;
    }
    for (size_t offset = 0; offset < encoded_size; offset += 2) {
        const int high = hex_value(encoded[offset]);
        const int low = hex_value(encoded[offset + 1]);
        if (high < 0 || low < 0) {
            return 0;
        }
        relative[offset / 2] = (char)((high << 4) | low);
    }
    relative[encoded_size / 2] = '\0';
    const char* root = getenv("LIQUID_WRAP_ROOT");
    if (root == NULL) {
        root = "/liquid/work";
    }
    char path[8192];
    const int path_size = snprintf(path, sizeof(path), "%s/%s", root, relative);
    if (path_size <= 0 || (size_t)path_size >= sizeof(path)) {
        return 0;
    }
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return 0;
    }
    char buffer[65536];
    uint64_t fingerprint = UINT64_C(1469598103934665603);
    uint64_t size = 0;
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0) {
            close(descriptor);
            return 0;
        }
        if (count == 0) {
            break;
        }
        fingerprint = fnv_update(fingerprint, buffer, (size_t)count);
        size += (uint64_t)count;
    }
    close(descriptor);
    return fingerprint == expected_fingerprint && size == expected_size;
}

static int planned_hit(uint64_t key) {
    const char* path = getenv("LIQUID_WRAP_PLAN");
    FILE* input = fopen(path == NULL ? "/liquid/plan" : path, "r");
    if (input == NULL) {
        return -1;
    }
    char line[16384];
    int found = 0;
    int status = -1;
    while (fgets(line, sizeof(line), input) != NULL) {
        unsigned long long candidate = 0;
        int candidate_status = 0;
        if (sscanf(line, "H\t%llx\t%d", &candidate, &candidate_status) == 2) {
            if ((uint64_t)candidate == key) {
                found = 1;
                status = candidate_status;
            }
            continue;
        }
        unsigned long long expected_fingerprint = 0;
        unsigned long long expected_size = 0;
        char encoded[8192];
        if (sscanf(line,
                   "C\t%llx\t%llx\t%llu\t%8191s",
                   &candidate,
                   &expected_fingerprint,
                   &expected_size,
                   encoded) == 4 &&
            (uint64_t)candidate == key &&
            !conditional_input_matches(
                encoded, (uint64_t)expected_fingerprint, (uint64_t)expected_size)) {
            fclose(input);
            return -1;
        }
    }
    fclose(input);
    return found ? status : -1;
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

static int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

static int conditional_input_matches(const char* encoded,
                                     uint64_t expected_fingerprint,
                                     uint64_t expected_size) {
    char relative[4096];
    const size_t encoded_size = strlen(encoded);
    if (encoded_size == 0 || encoded_size % 2 != 0 || encoded_size / 2 >= sizeof(relative)) {
        return 0;
    }
    for (size_t offset = 0; offset < encoded_size; offset += 2) {
        const int high = hex_value(encoded[offset]);
        const int low = hex_value(encoded[offset + 1]);
        if (high < 0 || low < 0) {
            return 0;
        }
        relative[offset / 2] = (char)((high << 4) | low);
    }
    relative[encoded_size / 2] = '\0';
    const char* root = getenv("LIQUID_WRAP_ROOT");
    if (root == NULL) {
        return 0;
    }
    char path[8192];
    const int path_size = snprintf(path, sizeof(path), "%s/%s", root, relative);
    if (path_size <= 0 || (size_t)path_size >= sizeof(path)) {
        return 0;
    }
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return 0;
    }
    char buffer[65536];
    uint64_t fingerprint = UINT64_C(1469598103934665603);
    uint64_t size = 0;
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0) {
            close(descriptor);
            return 0;
        }
        if (count == 0) {
            break;
        }
        fingerprint = fnv_update(fingerprint, buffer, (size_t)count);
        size += (uint64_t)count;
    }
    close(descriptor);
    return fingerprint == expected_fingerprint && size == expected_size;
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
    char line[16384];
    int found = 0;
    int status = -1;
    while (fgets(line, sizeof(line), input) != NULL) {
        unsigned long long candidate = 0;
        int candidate_status = 0;
        if (sscanf(line, "H\t%llx\t%d", &candidate, &candidate_status) == 2) {
            if ((uint64_t)candidate == key) {
                found = 1;
                status = candidate_status;
            }
            continue;
        }
        unsigned long long expected_fingerprint = 0;
        unsigned long long expected_size = 0;
        char encoded[8192];
        if (sscanf(line,
                   "C\t%llx\t%llx\t%llu\t%8191s",
                   &candidate,
                   &expected_fingerprint,
                   &expected_size,
                   encoded) == 4 &&
            (uint64_t)candidate == key &&
            !conditional_input_matches(
                encoded, (uint64_t)expected_fingerprint, (uint64_t)expected_size)) {
            fclose(input);
            return -1;
        }
    }
    fclose(input);
    return found ? status : -1;
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
