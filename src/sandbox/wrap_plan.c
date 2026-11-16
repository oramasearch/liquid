#if defined(__linux__)
/* open(O_NOFOLLOW | O_CLOEXEC), fstat and geteuid under -std=c99. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "sandbox/wrap_plan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "blake3.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static void hex_encode(const uint8_t* input, size_t size, char* output) {
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < size; ++index) {
        output[index * 2] = digits[input[index] >> 4U];
        output[index * 2 + 1] = digits[input[index] & 0x0fU];
    }
    output[size * 2] = '\0';
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

static int same_digest(const char* left, const char* right) {
    /* Both are fixed-length hex; compare every byte so the time does not depend on the match. */
    unsigned char difference = 0;
    for (size_t index = 0; index < LIQUID_WRAP_DIGEST_HEX_SIZE; ++index) {
        difference |= (unsigned char)(left[index] ^ right[index]);
    }
    return difference == 0;
}

static int is_digest_hex(const char* value) {
    if (value == NULL || strlen(value) != LIQUID_WRAP_DIGEST_HEX_SIZE) {
        return 0;
    }
    for (size_t index = 0; index < LIQUID_WRAP_DIGEST_HEX_SIZE; ++index) {
        if (hex_value(value[index]) < 0) {
            return 0;
        }
    }
    return 1;
}

void liquid_wrap_digest_hex(const void* data,
                            size_t size,
                            char output[LIQUID_WRAP_DIGEST_HEX_SIZE + 1]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, size);
    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));
    hex_encode(digest, sizeof(digest), output);
}

#ifndef _WIN32

int liquid_wrap_file_digest_hex(const char* path,
                                char output[LIQUID_WRAP_DIGEST_HEX_SIZE + 1],
                                uint64_t* size) {
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return -1;
    }
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    uint64_t total = 0;
    char buffer[65536];
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0) {
            close(descriptor);
            return -1;
        }
        if (count == 0) {
            break;
        }
        blake3_hasher_update(&hasher, buffer, (size_t)count);
        total += (uint64_t)count;
    }
    close(descriptor);
    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));
    hex_encode(digest, sizeof(digest), output);
    *size = total;
    return 0;
}

char* liquid_wrap_plan_load(const char* path,
                            const char* expected_hex,
                            int require_private,
                            size_t* size) {
    if (path == NULL || !is_digest_hex(expected_hex)) {
        return NULL;
    }
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return NULL;
    }
    struct stat info;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (require_private && (info.st_uid != geteuid() || (info.st_mode & 077) != 0))) {
        close(descriptor);
        return NULL;
    }
    const size_t capacity = (size_t)info.st_size;
    char* output = malloc(capacity + 1);
    if (output == NULL) {
        close(descriptor);
        return NULL;
    }
    size_t offset = 0;
    while (offset < capacity) {
        const ssize_t count = read(descriptor, output + offset, capacity - offset);
        if (count <= 0) {
            break;
        }
        offset += (size_t)count;
    }
    /* A plan that grew or shrank while it was read is not the plan liquid hashed. */
    char extra = 0;
    const int complete = offset == capacity && read(descriptor, &extra, 1) == 0;
    close(descriptor);
    output[offset] = '\0';
    char actual[LIQUID_WRAP_DIGEST_HEX_SIZE + 1];
    liquid_wrap_digest_hex(output, offset, actual);
    if (!complete || !same_digest(actual, expected_hex)) {
        free(output);
        return NULL;
    }
    *size = offset;
    return output;
}

#else

/* liquid wrap and its hook do not run on Windows (wrap returns errc::unsupported), so nothing
 * there reads a file condition or a plan. */
int liquid_wrap_file_digest_hex(const char* path,
                                char output[LIQUID_WRAP_DIGEST_HEX_SIZE + 1],
                                uint64_t* size) {
    (void)path;
    (void)output;
    (void)size;
    return -1;
}

char* liquid_wrap_plan_load(const char* path,
                            const char* expected_hex,
                            int require_private,
                            size_t* size) {
    (void)path;
    (void)expected_hex;
    (void)require_private;
    (void)size;
    return NULL;
}

#endif

static int decode_relative(const char* encoded, char* output, size_t capacity) {
    const size_t encoded_size = strlen(encoded);
    if (encoded_size == 0 || encoded_size % 2 != 0 || encoded_size / 2 >= capacity) {
        return 0;
    }
    for (size_t offset = 0; offset < encoded_size; offset += 2) {
        const int high = hex_value(encoded[offset]);
        const int low = hex_value(encoded[offset + 1]);
        if (high < 0 || low < 0) {
            return 0;
        }
        output[offset / 2] = (char)((high << 4) | low);
    }
    output[encoded_size / 2] = '\0';
    return strlen(output) == encoded_size / 2;
}

static int conditional_input_matches(const char* root,
                                     const char* encoded,
                                     const char* expected_hex,
                                     uint64_t expected_size) {
    char relative[4096];
    if (root == NULL || !is_digest_hex(expected_hex) ||
        !decode_relative(encoded, relative, sizeof(relative))) {
        return 0;
    }
    char path[8192];
    const int path_size = snprintf(path, sizeof(path), "%s/%s", root, relative);
    if (path_size <= 0 || (size_t)path_size >= sizeof(path)) {
        return 0;
    }
    char actual[LIQUID_WRAP_DIGEST_HEX_SIZE + 1];
    uint64_t size = 0;
    if (liquid_wrap_file_digest_hex(path, actual, &size) != 0) {
        return 0;
    }
    return size == expected_size && same_digest(actual, expected_hex);
}

/* Feeds a 64-bit little-endian length and then the bytes, so the parts of a key cannot run into
 * one another. */
static void hash_part(blake3_hasher* hasher, const void* data, size_t size) {
    uint8_t length[8];
    uint64_t value = (uint64_t)size;
    for (size_t index = 0; index < sizeof(length); ++index) {
        length[index] = (uint8_t)(value & 0xffU);
        value >>= 8U;
    }
    blake3_hasher_update(hasher, length, sizeof(length));
    blake3_hasher_update(hasher, data, size);
}

void liquid_wrap_process_key_hex(const char* executable,
                                 size_t executable_size,
                                 const char* directory,
                                 size_t directory_size,
                                 const char* arguments,
                                 size_t arguments_size,
                                 char output[LIQUID_WRAP_KEY_HEX_SIZE + 1]) {
    static const char label[] = "liquid wrap process key v2";
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    hash_part(&hasher, label, sizeof(label) - 1);
    hash_part(&hasher, executable, executable_size);
    hash_part(&hasher, directory, directory_size);
    hash_part(&hasher, arguments, arguments_size);
    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));
    hex_encode(digest, sizeof(digest), output);
}

/* Reads `<kind>\t<64 hex>\t` at the start of `line`. Returns the text after the second tab when
 * the row is of that kind and its key is exactly 64 lowercase hex digits equal to `key`;
 * otherwise NULL. */
static const char* row_for_key(const char* line, char kind, const char* key) {
    if (line[0] != kind || line[1] != '\t') {
        return NULL;
    }
    const char* candidate = line + 2;
    for (size_t index = 0; index < LIQUID_WRAP_KEY_HEX_SIZE; ++index) {
        if (hex_value(candidate[index]) < 0) {
            return NULL;
        }
    }
    if (candidate[LIQUID_WRAP_KEY_HEX_SIZE] != '\t' || !same_digest(candidate, key)) {
        return NULL;
    }
    return candidate + LIQUID_WRAP_KEY_HEX_SIZE + 1;
}

static int parse_status(const char* text, int* status) {
    if (*text < '0' || *text > '9') {
        return 0;
    }
    char* end = NULL;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > 255) {
        return 0;
    }
    *status = (int)value;
    return 1;
}

int liquid_wrap_plan_status(const char* plan, size_t size, const char* key, const char* root) {
    if (plan == NULL || !is_digest_hex(key)) {
        return -1;
    }
    int found = 0;
    int status = -1;
    size_t offset = 0;
    while (offset < size) {
        const char* start = plan + offset;
        const char* newline = memchr(start, '\n', size - offset);
        const size_t length = newline == NULL ? size - offset : (size_t)(newline - start);
        offset += length + 1;
        char line[16384];
        if (length >= sizeof(line)) {
            /* A row liquid would never write: refuse the whole plan rather than guess. */
            return -1;
        }
        memcpy(line, start, length);
        line[length] = '\0';
        const char* hit = row_for_key(line, 'H', key);
        if (hit != NULL) {
            int candidate_status = 0;
            if (!parse_status(hit, &candidate_status)) {
                return -1;
            }
            found = 1;
            status = candidate_status;
            continue;
        }
        const char* condition = row_for_key(line, 'C', key);
        if (condition == NULL) {
            continue;
        }
        /* A condition for this key that cannot be parsed or does not match is a miss. */
        char expected[LIQUID_WRAP_DIGEST_HEX_SIZE + 1];
        unsigned long long expected_size = 0;
        char encoded[8192];
        if (sscanf(condition, "%64s\t%llu\t%8191s", expected, &expected_size, encoded) != 3 ||
            !conditional_input_matches(root, encoded, expected, (uint64_t)expected_size)) {
            return -1;
        }
    }
    return found ? status : -1;
}
