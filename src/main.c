#include "hol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream) {
  (void)fprintf(stream,
    "%s %s\n\n"
    "Usage:\n"
    "  hands-on-learning [--course PATH [--exercise-profile PATH]]\n"
    "  hands-on-learning validate PATH\n"
    "  hands-on-learning catalog list [CATALOG]\n"
    "  hands-on-learning catalog install ID [CATALOG] [DESTINATION]\n"
    "  hands-on-learning --version\n\n"
    "Local cartridges need --exercise-profile PATH to enable exercises.\n",
    HOL_APP_NAME, HOL_APP_VERSION);
}

static const char *default_catalog(void) {
  if (access("courses/catalog.json", R_OK) == 0) return "courses/catalog.json";
  static char user_catalog[4096];
  const char *base = getenv("XDG_DATA_HOME");
  char fallback[4096];
  if (base == NULL || base[0] == '\0') {
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
      int length = snprintf(fallback, sizeof(fallback), "%s/.local/share", home);
      if (length >= 0 && (size_t)length < sizeof(fallback)) base = fallback;
    }
  }
  if (base != NULL && base[0] != '\0') {
    int length = snprintf(user_catalog, sizeof(user_catalog),
                          "%s/hands-on-learning/courses/catalog.json", base);
    if (length >= 0 && (size_t)length < sizeof(user_catalog) &&
        access(user_catalog, R_OK) == 0) return user_catalog;
  }
  return "/usr/local/share/hands-on-learning/courses/catalog.json";
}

static int default_install_destination(char output[4096]) {
  const char *base = getenv("XDG_DATA_HOME");
  char fallback[4096];
  if (base == NULL || base[0] == '\0') {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') return -1;
    int length = snprintf(fallback, sizeof(fallback), "%s/.local/share", home);
    if (length < 0 || (size_t)length >= sizeof(fallback)) return -1;
    base = fallback;
  }
  int length = snprintf(output, 4096, "%s/hands-on-learning/courses", base);
  return length >= 0 && length < 4096 ? 0 : -1;
}

static int choose_course(char course_path[4096], hol_error *error) {
  char course_id[HOL_ID_MAX + 1];
  char destination[4096];
  const char *catalog = default_catalog();
  if (default_install_destination(destination) < 0) {
    hol_error_set(error, HOL_ERR_PATH,
                  "cannot determine the course installation directory");
    return -1;
  }
  if (hol_catalog_select(catalog, stdin, stdout, course_id, error) < 0) return -1;
  (void)printf("\nPreparing the course. The first download can take a few minutes.\n");
  (void)fflush(stdout);
  if (hol_catalog_install_path(catalog, course_id, destination, course_path, 4096U,
                               error) < 0) return -1;
  return 0;
}

static int sibling_profile(const char *course, char profile[4096]) {
  size_t length = strlen(course);
  if (length < 6U || strcmp(course + length - 6U, ".imscc") != 0) return -1;
  int written = snprintf(profile, 4096, "%.*s.profile.json", (int)(length - 6U), course);
  return written >= 0 && written < 4096 && access(profile, R_OK) == 0 ? 0 : -1;
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
  if (argc >= 3 && strcmp(argv[1], "catalog") == 0 &&
      strcmp(argv[2], "list") == 0) {
    if (argc > 4 || hol_catalog_list(argc == 4 ? argv[3] : default_catalog(),
                                     stdout, &error) < 0) {
      (void)fprintf(stderr, "catalog list failed: %s\n", error.message);
      return 1;
    }
    return 0;
  }
  if (argc >= 4 && argc <= 6 && strcmp(argv[1], "catalog") == 0 &&
      strcmp(argv[2], "install") == 0) {
    char destination[4096];
    if (argc == 6) (void)snprintf(destination, sizeof(destination), "%s", argv[5]);
    else if (default_install_destination(destination) < 0) {
      (void)fprintf(stderr, "cannot determine the course installation directory\n");
      return 1;
    }
    const char *catalog = argc >= 5 ? argv[4] : default_catalog();
    if (hol_catalog_install(catalog, argv[3], destination, &error) < 0) {
      (void)fprintf(stderr, "catalog install failed: %s\n", error.message);
      return 1;
    }
    (void)printf("installed %s in %s\n", argv[3], destination);
    return 0;
  }
  char selected_course[4096];
  char selected_profile[4096];
  const char *course = getenv("HOL_COURSE");
  const char *profile = NULL;
  if (argc == 3 && strcmp(argv[1], "--course") == 0) course = argv[2];
  else if (argc == 5 && strcmp(argv[1], "--course") == 0 &&
           strcmp(argv[3], "--exercise-profile") == 0) {
    course = argv[2];
    profile = argv[4];
  }
  else if (argc != 1) {
    usage(stderr);
    return 2;
  }
  if (course == NULL || course[0] == '\0') {
    if (choose_course(selected_course, &error) < 0) {
      (void)fprintf(stderr, "cannot open course: %s\n", error.message);
      return 1;
    }
    course = selected_course;
    if (sibling_profile(course, selected_profile) == 0) profile = selected_profile;
  }
  if (hol_ui_run(course, profile, &error) < 0) {
    (void)fprintf(stderr, "%s: %s\n", HOL_APP_NAME,
                  error.message[0] != '\0' ? error.message : strerror(errno));
    return 1;
  }
  return 0;
}
