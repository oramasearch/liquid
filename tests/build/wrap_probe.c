// SPDX-License-Identifier: MIT

/* A tool for the wrap hook tests: writes "ran" to the file named by its argument, so a test can
 * tell a real run from one the hook answered from the plan. */
#include <stdio.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    FILE* output = fopen(argv[1], "w");
    if (output == NULL) {
        return 3;
    }
    fputs("ran\n", output);
    fclose(output);
    return 0;
}
