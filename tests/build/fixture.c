#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

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

static int print_random(const char* destination) {
    unsigned char value[32];
    int output;
#if defined(__linux__)
    if (getrandom(&value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
        return 2;
    }
#else
    for (size_t index = 0; index != sizeof(value); ++index) {
        value[index] = (unsigned char)arc4random();
    }
#endif
    output = open(destination, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (output < 0) {
        return 3;
    }
    if (write(output, &value, sizeof(value)) != (ssize_t)sizeof(value)) {
        close(output);
        return 4;
    }
    close(output);
    return 0;
}

static int read_file(const char* source) {
    char buffer[4096];
    int input = open(source, O_RDONLY);
    ssize_t count;
    if (input < 0) {
        return 2;
    }
    while ((count = read(input, buffer, sizeof(buffer))) > 0) {
    }
    close(input);
    return count < 0 ? 3 : 0;
}

static int fail_recorded(const char* destination) {
    static const char output_text[] = "recorded failure output\n";
    static const char stdout_text[] = "recorded failure stdout\n";
    static const char stderr_text[] = "recorded failure stderr\n";
    int output = open(destination, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (output < 0) {
        return 2;
    }
    if (write(output, output_text, sizeof(output_text) - 1) != (ssize_t)(sizeof(output_text) - 1)) {
        close(output);
        return 3;
    }
    close(output);
    if (write(STDOUT_FILENO, stdout_text, sizeof(stdout_text) - 1) !=
        (ssize_t)(sizeof(stdout_text) - 1)) {
        return 4;
    }
    if (write(STDERR_FILENO, stderr_text, sizeof(stderr_text) - 1) !=
        (ssize_t)(sizeof(stderr_text) - 1)) {
        return 5;
    }
    return 37;
}

static int install_imported(void) {
    static const char contents[] = "imported package\n";
    int output;
    if (mkdir("out", 0755) != 0 && access("out", F_OK) != 0) {
        return 2;
    }
    if (mkdir("out/prefix", 0755) != 0 && access("out/prefix", F_OK) != 0) {
        return 3;
    }
    if (mkdir("out/prefix/lib", 0755) != 0 && access("out/prefix/lib", F_OK) != 0) {
        return 4;
    }
    output = open("out/prefix/lib/imported.txt", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (output < 0) {
        return 5;
    }
    if (write(output, contents, sizeof(contents) - 1) != (ssize_t)(sizeof(contents) - 1)) {
        close(output);
        return 6;
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
    if (strcmp(argv[1], "-c") == 0 && argc == 3) {
        return install_imported();
    }
    if (strcmp(argv[1], "copy-tagged") == 0 && argc == 4) {
        return copy_tagged(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "env") == 0 && argc == 3) {
        return print_environment(argv[2]);
    }
    if (strcmp(argv[1], "random") == 0 && argc == 3) {
        return print_random(argv[2]);
    }
    if (strcmp(argv[1], "read") == 0 && argc == 3) {
        return read_file(argv[2]);
    }
    if (strcmp(argv[1], "fail") == 0 && argc == 3) {
        return fail_recorded(argv[2]);
    }
    if (strcmp(argv[1], "outside-read") == 0) {
        return open("/etc/hostname", O_RDONLY) < 0 ? 0 : 1;
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
