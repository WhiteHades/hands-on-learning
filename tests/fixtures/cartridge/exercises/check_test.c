#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  const char *submit = getenv("HOL_SUBMIT");
  const char *home = getenv("HOME");
  const char *temporary = getenv("TMPDIR");
  if (submit == NULL || !same_text(submit, "1")) {
    fputs("Check did not set HOL_SUBMIT=1.\n", stderr);
    return 1;
  }
  if (!exercise_network_is_private() || getenv("HOL_HOST_SECRET") != NULL ||
      home == NULL || temporary == NULL ||
      !same_text(home, "/home/learner") || !same_text(temporary, "/tmp") ||
      access(".visible-stage-marker", F_OK) == 0) {
    fputs("Check environment or stage was not private.\n", stderr);
    return 1;
  }
  FILE *escape = fopen("/stage-escape", "w");
  if (escape != NULL) {
    (void)fclose(escape);
    fputs("The sandbox root is writable.\n", stderr);
    return 1;
  }
  if (!same_text(lesson_greeting(), exercise_expected())) {
    fputs("lesson_greeting returned the wrong text.\n", stderr);
    return 1;
  }
  puts("All supplied tests passed.");
  const char *token = getenv("HOL_TEST_TOKEN");
  if (token == NULL) return 1;
  printf("HOL_TEST_COMPLETE:%s\n", token);
  return 0;
}
