#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *lesson_greeting(void);
const char *exercise_expected(void);
int exercise_network_is_private(void);

static int same_text(const char *left, const char *right) {
  while (*left != '\0' && *left == *right) {
    left++;
    right++;
  }
  return *left == *right;
}

int main(void) {
  if (!exercise_network_is_private() || getenv("HOL_HOST_SECRET") != NULL) {
    fputs("The visible test is not isolated.\n", stderr);
    return 1;
  }
  FILE *marker = fopen(".visible-stage-marker", "w");
  if (marker == NULL || fclose(marker) != 0) return 1;
  const char *actual = lesson_greeting();
  puts(actual);
  if (!same_text(actual, exercise_expected())) {
    fputs("Expected Hello, learner!\n", stderr);
    return 1;
  }
  return 0;
}
