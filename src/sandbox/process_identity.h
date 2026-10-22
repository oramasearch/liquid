#ifndef LIQUID_PROCESS_IDENTITY_H
#define LIQUID_PROCESS_IDENTITY_H

#if defined(__linux__)
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LIQUID_WRAP_LAUNCH_SEQUENCE "LIQUID_WRAP_LAUNCH_SEQUENCE"

/* Read the exec-time environment, which the parent observer can read too. Zero names a task
 * outside a wrap launch; oversized unrelated entries are skipped without retaining their data. */
static inline int liquid_process_scope(const char* process, unsigned long long* sequence) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/%s/environ", process);
    FILE* input = fopen(path, "re");
    if (input == NULL)
        return -1;
    char entry[128];
    size_t size = 0;
    int oversized = 0;
    int result = 0;
    int character;
    *sequence = 0;
    while ((character = fgetc(input)) != EOF) {
        if (character != '\0') {
            if (size + 1 < sizeof(entry))
                entry[size++] = (char)character;
            else
                oversized = 1;
            continue;
        }
        entry[size] = '\0';
        const char prefix[] = LIQUID_WRAP_LAUNCH_SEQUENCE "=";
        if (strncmp(entry, prefix, sizeof(prefix) - 1) == 0) {
            if (oversized) {
                result = -1;
                break;
            }
            const char* value = entry + sizeof(prefix) - 1;
            char* end;
            errno = 0;
            *sequence = strtoull(value, &end, 10);
            if (*value < '0' || *value > '9' || *end != '\0' || errno != 0 || *sequence == 0)
                result = -1;
            break;
        }
        size = 0;
        oversized = 0;
    }
    if (ferror(input))
        result = -1;
    fclose(input);
    return result;
}

/* The hook and the FUSE observer must name the same task, even across PID namespaces.
 * Read this while the task is blocked in its FUSE request, before it can exit. */
static inline int
liquid_process_identity_at(const char* entry, char* output, size_t capacity, long* parent) {
    char path[128];
    struct stat namespace_stat;
    snprintf(path, sizeof(path), "/proc/%s/ns/pid", entry);
    if (stat(path, &namespace_stat) != 0)
        return -1;
    snprintf(path, sizeof(path), "/proc/%s/status", entry);
    FILE* input = fopen(path, "re");
    if (input == NULL)
        return -1;
    char line[8192];
    long local_pid = 0;
    while (fgets(line, sizeof(line), input) != NULL) {
        if (parent != NULL && strncmp(line, "PPid:", 5) == 0)
            *parent = strtol(line + 5, NULL, 10);
        if (strncmp(line, "NSpid:", 6) != 0)
            continue;
        char* cursor = line + 6;
        for (;;) {
            char* end;
            const long value = strtol(cursor, &end, 10);
            if (end == cursor)
                break;
            local_pid = value;
            cursor = end;
        }
    }
    fclose(input);
    if (local_pid <= 0)
        return -1;
    unsigned long long sequence;
    if (liquid_process_scope(entry, &sequence) != 0)
        return -1;
    const int size = snprintf(output,
                              capacity,
                              "%llu:%llu:%ld",
                              sequence,
                              (unsigned long long)namespace_stat.st_ino,
                              local_pid);
    return size > 0 && (size_t)size < capacity ? 0 : -1;
}

static inline int liquid_process_identity(long pid, char* output, size_t capacity) {
    char entry[32];
    snprintf(entry, sizeof(entry), "%ld", pid);
    return liquid_process_identity_at(entry, output, capacity, NULL);
}
#endif
#endif
