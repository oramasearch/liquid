// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>

int main(void) {
    char value[3];
    memset(value, 0, sizeof(value));
    value[0] = '4';
    value[1] = '2';
    puts(value);
    return 0;
}
