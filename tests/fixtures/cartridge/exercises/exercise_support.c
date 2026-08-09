#include <stdio.h>
#include <string.h>

static int same_text(const char *left, const char *right) {
  while (*left != '\0' && *left == *right) {
    left++;
    right++;
  }
  return *left == *right;
}

const char *exercise_expected(void) {
  return "Hello, learner!";
}

int exercise_network_is_private(void) {
  FILE *devices = fopen("/proc/net/dev", "r");
  if (devices == NULL) return 0;
  char line[256];
  while (fgets(line, sizeof(line), devices) != NULL) {
    char *name = line;
    while (*name == ' ') name++;
    char *colon = strchr(name, ':');
    if (colon == NULL) continue;
    *colon = '\0';
    if (!same_text(name, "lo")) {
      (void)fclose(devices);
      return 0;
    }
  }
  return fclose(devices) == 0;
}
