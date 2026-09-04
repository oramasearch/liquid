#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char** environ;

static int copy_file(const char* source, const char* destination) {
    char buffer[4096];
    int input = open(source, O_RDONLY);
    int output = open(destination, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    ssize_t count;
    if (input < 0 || output < 0) {
        return 2;
    }
    while ((count = read(input, buffer, sizeof(buffer))) > 0) {
        if (write(output, buffer, (size_t)count) != count) {
            return 3;
        }
    }
    close(input);
    close(output);
    return count < 0 ? 4 : 0;
}

static int copy_tagged(const char* source, const char* destination) {
    int copied = copy_file(source, destination);
    int output;
    if (copied != 0) {
        return copied;
    }
    output = open(destination, O_APPEND | O_WRONLY);
    if (output < 0) {
        return 5;
    }
    if (write(output, "-derived", 8) != 8) {
        close(output);
        return 6;
    }
    close(output);
    return 0;
}

static int print_environment(const char* destination) {
    int output = open(destination, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    char** entry = environ;
    if (output < 0) {
        return 2;
    }
    while (*entry != NULL) {
        size_t length = strlen(*entry);
        if (write(output, *entry, length) != (ssize_t)length || write(output, "\n", 1) != 1) {
            return 3;
        }
        ++entry;
    }
    close(output);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        return 64;
    }
    if (strcmp(argv[1], "copy") == 0 && argc == 4) {
        return copy_file(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "copy-tagged") == 0 && argc == 4) {
        return copy_tagged(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "env") == 0 && argc == 3) {
        return print_environment(argv[2]);
    }
    if (strcmp(argv[1], "outside-read") == 0) {
        return open("/etc/passwd", O_RDONLY) < 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "outside-write") == 0) {
        return copy_file("input.txt", "undeclared.txt");
    }
    if (strcmp(argv[1], "sleep") == 0) {
        if (fork() == 0) {
            sleep(30);
            _exit(0);
        }
        sleep(30);
        return 0;
    }
    return 64;
}
