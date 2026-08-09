#include "hol.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void assert_artifact(const char *path, off_t bytes, const char *digest) {
  struct stat status;
  hol_error error = {0};
  char actual[65];
  assert(stat(path, &status) == 0 && status.st_size == bytes);
  assert(hol_sha256_file(path, actual, &error) == 0);
  assert(strcmp(actual, digest) == 0);
}

int main(void) {
  char root[] = "/tmp/hol-catalog-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char archive[4096];
  assert(realpath("build/test-course.imscc", archive) != NULL);
  struct stat status;
  assert(stat(archive, &status) == 0);
  hol_error error = {0};
  char digest[65];
  assert(hol_sha256_file(archive, digest, &error) == 0);
  char profile[4096];
  assert(realpath("build/test-course.profile.json", profile) != NULL);
  struct stat profile_status;
  char profile_digest[65];
  assert(stat(profile, &profile_status) == 0);
  assert(hol_sha256_file(profile, profile_digest, &error) == 0);
  char catalog[4096];
  char installed[4096];
  assert(hol_join_path(catalog, sizeof(catalog), root, "catalog.json", &error) == 0);
  assert(hol_join_path(installed, sizeof(installed), root, "data/courses", &error) == 0);
  char manifest[8192];
  int length = snprintf(manifest, sizeof(manifest),
    "{\"schema_version\":1,\"updated_at\":\"2026-08-09T00:00:00Z\",\"courses\":[{"
    "\"id\":\"test.course\",\"version\":\"1.0.0\",\"title\":\"Test Course\","
    "\"description\":\"Fixture\",\"minimum_app_version\":\"0.1.0\",\"lesson_count\":5,"
    "\"license\":{\"spdx\":\"MIT\",\"attribution\":\"SPDX-License-Identifier: MIT\"},"
    "\"bundle\":{\"url\":\"file://%s\",\"bytes\":%lld,\"sha256\":\"%s\"},"
    "\"exercise_profile\":{\"url\":\"file://%s\",\"bytes\":%lld,\"sha256\":\"%s\"}}]}\n",
    archive, (long long)status.st_size, digest, profile,
    (long long)profile_status.st_size, profile_digest);
  assert(length > 0 && (size_t)length < sizeof(manifest));
  assert(hol_atomic_write(catalog, manifest, (size_t)length, &error) == 0);

  char *listing = NULL;
  size_t listing_length = 0U;
  FILE *stream = open_memstream(&listing, &listing_length);
  assert(stream != NULL);
  assert(hol_catalog_list(catalog, stream, &error) == 0);
  assert(fclose(stream) == 0);
  assert(strstr(listing, "test.course") != NULL);
  assert(strstr(listing, "SPDX-License-Identifier: MIT") != NULL);
  free(listing);

  char choice[] = "1\n";
  FILE *input = fmemopen(choice, strlen(choice), "r");
  char *menu = NULL;
  size_t menu_length = 0U;
  stream = open_memstream(&menu, &menu_length);
  char selected[HOL_ID_MAX + 1];
  assert(input != NULL && stream != NULL);
  assert(hol_catalog_select(catalog, input, stream, selected, &error) == 0);
  assert(fclose(input) == 0 && fclose(stream) == 0);
  assert(strcmp(selected, "test.course") == 0);
  assert(strstr(menu, "1. Test Course") != NULL);
  free(menu);

  assert(hol_catalog_install(catalog, "test.course", installed, &error) == 0);
  assert(hol_catalog_install(catalog, "test.course", installed, &error) == 0);
  char course_path[4096];
  assert(hol_join_path(course_path, sizeof(course_path), installed,
                         "test.course-1.0.0.imscc", &error) == 0);
  char profile_path[4096];
  assert(hol_join_path(profile_path, sizeof(profile_path), installed,
                       "test.course-1.0.0.profile.json", &error) == 0);
  assert(access(profile_path, R_OK) == 0);
  assert_artifact(course_path, status.st_size, digest);
  assert_artifact(profile_path, profile_status.st_size, profile_digest);

  static const char corrupt_profile[] = "corrupt\n";
  char corrupt_byte = 'X';
  int cached = -1;
  assert(hol_atomic_write(profile_path, corrupt_profile,
                          strlen(corrupt_profile), &error) == 0);
  assert(hol_catalog_install(catalog, "test.course", installed, &error) == 0);
  assert_artifact(profile_path, profile_status.st_size, profile_digest);

  cached = open(profile_path, O_WRONLY | O_CLOEXEC);
  assert(cached >= 0);
  assert(pwrite(cached, &corrupt_byte, 1U, 0) == 1);
  assert(close(cached) == 0);
  assert(hol_catalog_install(catalog, "test.course", installed, &error) == 0);
  assert_artifact(profile_path, profile_status.st_size, profile_digest);

  cached = open(course_path, O_WRONLY | O_CLOEXEC);
  assert(cached >= 0);
  assert(pwrite(cached, &corrupt_byte, 1U, 0) == 1);
  assert(close(cached) == 0);
  assert(hol_catalog_install(catalog, "test.course", installed, &error) == 0);
  assert_artifact(course_path, status.st_size, digest);

  cached = open(course_path, O_WRONLY | O_APPEND | O_CLOEXEC);
  assert(cached >= 0);
  assert(write(cached, &corrupt_byte, 1U) == 1);
  assert(close(cached) == 0);
  assert(hol_catalog_install(catalog, "test.course", installed, &error) == 0);
  assert_artifact(course_path, status.st_size, digest);

  hol_course *course = NULL;
  assert(hol_course_load_profile(course_path, profile_path, &course, &error) == 0);
  assert(course->lesson_count == 5U);
  assert(hol_course_lesson(course, 1U)->kind == HOL_LESSON_EXERCISE);
  hol_course_free(course);

  char plain_catalog[4096];
  char plain_installed[4096];
  assert(hol_join_path(plain_catalog, sizeof(plain_catalog), root,
                       "plain-catalog.json", &error) == 0);
  assert(hol_join_path(plain_installed, sizeof(plain_installed), root,
                       "plain/courses", &error) == 0);
  length = snprintf(manifest, sizeof(manifest),
    "{\"schema_version\":1,\"updated_at\":\"2026-08-09T00:00:00Z\",\"courses\":[{"
    "\"id\":\"test.course\",\"version\":\"1.0.0\",\"title\":\"Test Course\","
    "\"description\":\"Fixture\",\"minimum_app_version\":\"0.1.0\",\"lesson_count\":5,"
    "\"license\":{\"spdx\":\"MIT\",\"attribution\":\"SPDX-License-Identifier: MIT\"},"
    "\"bundle\":{\"url\":\"file://%s\",\"bytes\":%lld,\"sha256\":\"%s\"}}]}\n",
    archive, (long long)status.st_size, digest);
  assert(length > 0 && (size_t)length < sizeof(manifest));
  assert(hol_atomic_write(plain_catalog, manifest, (size_t)length, &error) == 0);
  assert(hol_catalog_install(plain_catalog, "test.course", plain_installed, &error) == 0);
  char stale_profile[4096];
  assert(hol_join_path(stale_profile, sizeof(stale_profile), plain_installed,
                       "test.course-1.0.0.profile.json", &error) == 0);
  assert(hol_atomic_write(stale_profile, corrupt_profile,
                          strlen(corrupt_profile), &error) == 0);
  assert(hol_catalog_install(plain_catalog, "test.course", plain_installed, &error) == 0);
  assert(access(stale_profile, F_OK) < 0);

  char command[8192];
  (void)snprintf(command, sizeof(command), "rm -rf -- '%s'", root);
  assert(system(command) == 0);
  puts("catalog tests passed");
  return 0;
}
