// SPDX-License-Identifier: MIT

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static int copy_file(const char* source, const char* destination) {
    char buffer[4096];
    const int input = open(source, O_RDONLY);
    const int output = open(destination, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (input < 0 || output < 0) {
        return 2;
    }
    ssize_t count;
    while ((count = read(input, buffer, sizeof(buffer))) > 0) {
        if (write(output, buffer, (size_t)count) != count) {
            return 3;
        }
    }
    close(input);
    close(output);
    return count < 0 ? 4 : 0;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        return 64;
    }
    char* end = NULL;
    const unsigned long milliseconds = strtoul(argv[3], &end, 10);
    if (end == argv[3] || *end != '\0') {
        return 64;
    }
    usleep(milliseconds * 1000);
    return copy_file(argv[1], argv[2]);
}
