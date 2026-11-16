// SPDX-License-Identifier: MIT

#include <stdio.h>

int left(void);
int right(void);

int main(void) {
    printf("%d\n", left() + right());
    return 0;
}
