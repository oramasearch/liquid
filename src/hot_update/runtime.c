#include "liquid/hot_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

struct registered_entry {
    char* symbol;
    void** slot;
    void* original;
};

struct pending_entry {
    void** slot;
    void* replacement;
};

static struct registered_entry* registered_entries;
static size_t registered_count;
static struct pending_entry* pending_entries;
static size_t pending_count;
static void** library_handles;
static size_t library_count;
static char last_error[256];

#if !defined(_WIN32)
static int listener = -1;
static char* listener_path;

static void close_listener(void) {
    if (listener >= 0) {
        close(listener);
        listener = -1;
    }
    if (listener_path != NULL) {
        unlink(listener_path);
        free(listener_path);
        listener_path = NULL;
    }
}
#endif

static liquid_hot_status set_error(liquid_hot_status status, const char* message) {
    if (message == NULL) {
        last_error[0] = '\0';
        return status;
    }
    snprintf(last_error, sizeof(last_error), "%s", message);
    return status;
}

static char* copy_text(const char* begin, size_t size) {
    char* output = (char*)malloc(size + 1);
    if (output == NULL) {
        return NULL;
    }
    memcpy(output, begin, size);
    output[size] = '\0';
    return output;
}

static struct registered_entry* find_entry(const char* symbol) {
    size_t index;
    for (index = 0; index < registered_count; ++index) {
        if (strcmp(registered_entries[index].symbol, symbol) == 0) {
            return &registered_entries[index];
        }
    }
    return NULL;
}

uint32_t liquid_hot_abi_version(void) {
    return LIQUID_HOT_ABI_VERSION;
}

liquid_hot_status liquid_hot_register(const char* symbol, void** entry_slot) {
    struct registered_entry* resized;
    char* name;
    if (symbol == NULL || symbol[0] == '\0' || entry_slot == NULL || *entry_slot == NULL) {
        return set_error(LIQUID_HOT_INVALID, "invalid hot-update entry registration");
    }
    if (find_entry(symbol) != NULL) {
        return set_error(LIQUID_HOT_INVALID, "hot-update entry is already registered");
    }
    name = copy_text(symbol, strlen(symbol));
    if (name == NULL) {
        return set_error(LIQUID_HOT_IO, "cannot allocate hot-update entry");
    }
    resized = (struct registered_entry*)realloc(
        registered_entries, (registered_count + 1) * sizeof(*registered_entries));
    if (resized == NULL) {
        free(name);
        return set_error(LIQUID_HOT_IO, "cannot allocate hot-update registry");
    }
    registered_entries = resized;
    registered_entries[registered_count].symbol = name;
    registered_entries[registered_count].slot = entry_slot;
    registered_entries[registered_count].original = *entry_slot;
    ++registered_count;
    return set_error(LIQUID_HOT_OK, NULL);
}

#if !defined(_WIN32)

static const char* next_line(char** cursor) {
    char* begin = *cursor;
    char* end;
    if (begin == NULL || *begin == '\0') {
        return NULL;
    }
    end = strchr(begin, '\n');
    if (end == NULL) {
        return NULL;
    }
    *end = '\0';
    *cursor = end + 1;
    return begin;
}

static liquid_hot_status queue_patch(char* contents) {
    const char* magic;
    const char* library;
    const char* tier;
    const char* mapping;
    void* handle;
    struct pending_entry* additions = NULL;
    size_t additions_count = 0;
    size_t resolved_count = 0;
    char* cursor = contents;
    magic = next_line(&cursor);
    library = next_line(&cursor);
    tier = next_line(&cursor);
    if (magic == NULL || strcmp(magic, "liquid-hot/1") != 0 || library == NULL ||
        library[0] == '\0' || tier == NULL ||
        (strcmp(tier, "tier=1") != 0 && strcmp(tier, "tier=2") != 0)) {
        return set_error(LIQUID_HOT_INVALID, "invalid hot-update payload");
    }
    handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        return set_error(LIQUID_HOT_IO, dlerror());
    }
    while ((mapping = next_line(&cursor)) != NULL) {
        char* separator;
        struct registered_entry* entry;
        void* replacement;
        struct pending_entry* resized;
        if (mapping[0] == '\0') {
            continue;
        }
        separator = strchr((char*)mapping, '=');
        if (separator == NULL || separator == mapping || separator[1] == '\0') {
            free(additions);
            dlclose(handle);
            return set_error(LIQUID_HOT_INVALID, "invalid hot-update symbol mapping");
        }
        *separator = '\0';
        dlerror();
        replacement = dlsym(handle, separator + 1);
        if (replacement == NULL || dlerror() != NULL) {
            free(additions);
            dlclose(handle);
            return set_error(LIQUID_HOT_SYMBOL, "replacement symbol is not exported");
        }
        ++resolved_count;
        if (strcmp(tier, "tier=2") == 0) {
            continue;
        }
        entry = find_entry(mapping);
        if (entry == NULL) {
            free(additions);
            dlclose(handle);
            return set_error(LIQUID_HOT_SYMBOL, "hot-update entry is not registered");
        }
        resized =
            (struct pending_entry*)realloc(additions, (additions_count + 1) * sizeof(*additions));
        if (resized == NULL) {
            free(additions);
            dlclose(handle);
            return set_error(LIQUID_HOT_IO, "cannot allocate pending hot update");
        }
        additions = resized;
        additions[additions_count].slot = entry->slot;
        additions[additions_count].replacement = replacement;
        ++additions_count;
    }
    if (resolved_count == 0) {
        free(additions);
        dlclose(handle);
        return set_error(LIQUID_HOT_INVALID, "hot-update payload has no symbols");
    }
    {
        void** handles;
        if (additions_count != 0) {
            struct pending_entry* resized = (struct pending_entry*)realloc(
                pending_entries, (pending_count + additions_count) * sizeof(*pending_entries));
            if (resized == NULL) {
                free(additions);
                dlclose(handle);
                return set_error(LIQUID_HOT_IO, "cannot allocate pending hot updates");
            }
            pending_entries = resized;
            memcpy(
                pending_entries + pending_count, additions, additions_count * sizeof(*additions));
            pending_count += additions_count;
        }
        free(additions);
        handles = (void**)realloc(library_handles, (library_count + 1) * sizeof(*library_handles));
        if (handles == NULL) {
            pending_count -= additions_count;
            dlclose(handle);
            return set_error(LIQUID_HOT_IO, "cannot retain hot-update library");
        }
        library_handles = handles;
        library_handles[library_count] = handle;
        ++library_count;
    }
    return set_error(LIQUID_HOT_OK, NULL);
}

static liquid_hot_status read_all(int descriptor, void* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = read(descriptor, (char*)data + offset, size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return set_error(LIQUID_HOT_IO, "incomplete hot-update socket payload");
        }
        offset += (size_t)count;
    }
    return LIQUID_HOT_OK;
}

static liquid_hot_status receive_patch(int descriptor) {
    unsigned char size_bytes[4];
    uint32_t size;
    char* contents;
    liquid_hot_status status;
    status = read_all(descriptor, size_bytes, sizeof(size_bytes));
    if (status != LIQUID_HOT_OK) {
        return status;
    }
    size = ((uint32_t)size_bytes[0] << 24u) | ((uint32_t)size_bytes[1] << 16u) |
           ((uint32_t)size_bytes[2] << 8u) | (uint32_t)size_bytes[3];
    if (size == 0 || size > 64u * 1024u * 1024u) {
        return set_error(LIQUID_HOT_INVALID, "invalid hot-update socket payload size");
    }
    contents = (char*)malloc((size_t)size + 1u);
    if (contents == NULL) {
        return set_error(LIQUID_HOT_IO, "cannot allocate hot-update socket payload");
    }
    status = read_all(descriptor, contents, size);
    if (status == LIQUID_HOT_OK) {
        contents[size] = '\0';
        status = queue_patch(contents);
    }
    free(contents);
    return status;
}

#endif

liquid_hot_status liquid_hot_listen(const char* endpoint) {
#if defined(_WIN32)
    (void)endpoint;
    return set_error(LIQUID_HOT_UNSUPPORTED, "hot update is unsupported on Windows");
#else
    struct sockaddr_un address;
    int descriptor;
    int flags;
    size_t required_size;
    if (endpoint == NULL || endpoint[0] == '\0') {
        return set_error(LIQUID_HOT_INVALID, "invalid hot-update endpoint");
    }
    required_size = strlen(endpoint) + 1;
    if (required_size > sizeof(address.sun_path)) {
        snprintf(last_error,
                 sizeof(last_error),
                 "socket path exceeds the platform limit (%zu > %zu bytes including NUL): '%s'",
                 required_size,
                 sizeof(address.sun_path),
                 endpoint);
        return LIQUID_HOT_INVALID;
    }
    close_listener();
    descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return set_error(LIQUID_HOT_IO, strerror(errno));
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", endpoint);
    unlink(endpoint);
    if (bind(descriptor, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        chmod(endpoint, S_IRUSR | S_IWUSR) != 0 || listen(descriptor, 4) != 0) {
        close(descriptor);
        unlink(endpoint);
        return set_error(LIQUID_HOT_IO, strerror(errno));
    }
    flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(descriptor);
        unlink(endpoint);
        return set_error(LIQUID_HOT_IO, strerror(errno));
    }
    listener_path = copy_text(endpoint, strlen(endpoint));
    if (listener_path == NULL) {
        close(descriptor);
        unlink(endpoint);
        return set_error(LIQUID_HOT_IO, "cannot retain hot-update endpoint");
    }
    listener = descriptor;
    return set_error(LIQUID_HOT_OK, NULL);
#endif
}

liquid_hot_status liquid_hot_apply(const void* patch_bytes, size_t patch_size) {
#if defined(_WIN32)
    (void)patch_bytes;
    (void)patch_size;
    return set_error(LIQUID_HOT_UNSUPPORTED, "hot update is unsupported on Windows");
#else
    char* contents;
    liquid_hot_status status;
    if (patch_bytes == NULL || patch_size == 0 || memchr(patch_bytes, '\0', patch_size) != NULL) {
        return set_error(LIQUID_HOT_INVALID, "invalid hot-update payload bytes");
    }
    contents = copy_text((const char*)patch_bytes, patch_size);
    if (contents == NULL) {
        return set_error(LIQUID_HOT_IO, "cannot allocate hot-update payload");
    }
    status = queue_patch(contents);
    free(contents);
    return status;
#endif
}

liquid_hot_status liquid_hot_quiescent(void) {
#if !defined(_WIN32)
    if (listener >= 0) {
        for (;;) {
            int connection = accept(listener, NULL, NULL);
            liquid_hot_status status;
            if (connection < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (connection < 0 && errno == EINTR) {
                continue;
            }
            if (connection < 0) {
                return set_error(LIQUID_HOT_IO, strerror(errno));
            }
            status = receive_patch(connection);
            {
                const unsigned char response = (unsigned char)status;
                ssize_t written;
                do {
                    written = write(connection, &response, sizeof(response));
                } while (written < 0 && errno == EINTR);
            }
            close(connection);
            if (status != LIQUID_HOT_OK) {
                return status;
            }
        }
    }
#endif
    {
        size_t index;
        for (index = 0; index < pending_count; ++index) {
            *pending_entries[index].slot = pending_entries[index].replacement;
        }
        free(pending_entries);
        pending_entries = NULL;
        pending_count = 0;
    }
    return set_error(LIQUID_HOT_OK, NULL);
}

void liquid_hot_close(void) {
#if !defined(_WIN32)
    size_t index;
    close_listener();
    for (index = 0; index < registered_count; ++index) {
        *registered_entries[index].slot = registered_entries[index].original;
    }
    for (index = 0; index < library_count; ++index) {
        dlclose(library_handles[index]);
    }
    free(library_handles);
    library_handles = NULL;
    library_count = 0;
#endif
    {
        size_t index;
        for (index = 0; index < registered_count; ++index) {
            free(registered_entries[index].symbol);
        }
        free(registered_entries);
        registered_entries = NULL;
        registered_count = 0;
        free(pending_entries);
        pending_entries = NULL;
        pending_count = 0;
    }
}

const char* liquid_hot_last_error(void) {
    return last_error;
}
