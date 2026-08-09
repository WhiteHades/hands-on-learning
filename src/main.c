#include "hol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream) {
  (void)fprintf(stream,
    "%s %s\n\n"
    "Usage:\n"
    "  hands-on-learning [--course PATH]\n"
    "  hands-on-learning validate PATH\n"
    "  hands-on-learning --version\n\n"
    "The default course is the bundled original demo. Set HOL_COURSE to use\n"
    "another installed .holcourse directory.\n",
    HOL_APP_NAME, HOL_APP_VERSION);
}

static const char *default_course(void) {
  const char *configured = getenv("HOL_COURSE");
  if (configured != NULL && configured[0] != '\0') return configured;
  if (access("courses/demo.holcourse/course.json", R_OK) == 0)
    return "courses/demo.holcourse";
  return "/usr/local/share/hands-on-learning/courses/demo.holcourse";
}

int main(int argc, char **argv) {
  hol_error error = {0};
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    (void)printf("%s %s\n", HOL_APP_NAME, HOL_APP_VERSION);
    return 0;
  }
  if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    usage(stdout);
    return 0;
  }
  if (argc == 3 && strcmp(argv[1], "validate") == 0) {
    hol_course *course = NULL;
    if (hol_course_load(argv[2], &course, &error) < 0) {
      (void)fprintf(stderr, "validation failed: %s\n", error.message);
      return 1;
    }
    (void)printf("valid: %s %s (%zu lessons, %s)\n", course->title,
                 course->version, course->lesson_count, course->license_spdx);
    hol_course_free(course);
    return 0;
  }
  const char *course = default_course();
  if (argc == 3 && strcmp(argv[1], "--course") == 0) course = argv[2];
  else if (argc != 1) {
    usage(stderr);
    return 2;
  }
  if (hol_ui_run(course, &error) < 0) {
    (void)fprintf(stderr, "%s: %s\n", HOL_APP_NAME,
                  error.message[0] != '\0' ? error.message : strerror(errno));
    return 1;
  }
  return 0;
}
