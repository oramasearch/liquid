// SPDX-License-Identifier: MIT

#include <stdio.h>

int main(int argc, char** argv) {
    if (argc != 6) {
        return 2;
    }
    FILE* output = fopen(argv[1], "w");
    if (output == NULL) {
        return 1;
    }
    fprintf(output, "outcome=%s old=%s new=%s tier=%s\n", argv[2], argv[3], argv[4], argv[5]);
    return fclose(output) == 0 ? 0 : 1;
}
