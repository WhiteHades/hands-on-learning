#include <stdio.h>

__attribute__((constructor)) static void visible_test(void) {
  fputs("Visible stdout test active.\n", stderr);
}
