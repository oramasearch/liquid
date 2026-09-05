#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 4 || argv[2][0] != '-' || argv[2][1] != 'o' || argv[2][2] != '\0') {
        return 64;
    }
    sleep(3);
    const int input = open(argv[1], O_RDONLY);
    if (input < 0) {
        return 2;
    }
    const int output = open(argv[3], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (output < 0) {
        close(input);
        return 3;
    }
    char buffer[4096];
    ssize_t count;
    while ((count = read(input, buffer, sizeof(buffer))) > 0) {
        if (write(output, buffer, (size_t)count) != count) {
            close(input);
            close(output);
            return 4;
        }
    }
    close(input);
    close(output);
    return count < 0 ? 5 : 0;
}
