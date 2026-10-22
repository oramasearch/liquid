/* Wrap test tool: writes "ran" to a marker, or prints the Linux process identity with --identity.
 */
#include <stdio.h>
#if defined(__linux__)
#include "sandbox/process_identity.h"
#endif

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
#if defined(__linux__)
    if (strcmp(argv[1], "--identity") == 0) {
        char identity[128];
        if (liquid_process_identity_at("self", identity, sizeof(identity), NULL) != 0)
            return 4;
        puts(identity);
        return 0;
    }
#endif
    FILE* output = fopen(argv[1], "w");
    if (output == NULL) {
        return 3;
    }
    fputs("ran\n", output);
    fclose(output);
    return 0;
}
