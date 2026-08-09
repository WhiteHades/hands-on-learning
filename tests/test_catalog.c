#include "hol.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
  char catalog[4096];
  char installed[4096];
  assert(hol_join_path(catalog, sizeof(catalog), root, "catalog.json", &error) == 0);
  assert(hol_join_path(installed, sizeof(installed), root, "data/courses", &error) == 0);
  char manifest[8192];
  int length = snprintf(manifest, sizeof(manifest),
    "{\"schema_version\":1,\"updated_at\":\"2026-08-09T00:00:00Z\",\"courses\":[{"
    "\"id\":\"test.course\",\"version\":\"1.0.0\",\"title\":\"Test Course\","
    "\"description\":\"Fixture\",\"minimum_app_version\":\"0.1.0\",\"lesson_count\":2,"
    "\"license\":{\"spdx\":\"MIT\",\"attribution\":\"SPDX-License-Identifier: MIT\"},"
    "\"bundle\":{\"url\":\"file://%s\",\"bytes\":%lld,\"sha256\":\"%s\"}}]}\n",
    archive, (long long)status.st_size, digest);
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
  hol_course *course = NULL;
  assert(hol_course_load(course_path, &course, &error) == 0);
  assert(course->lesson_count == 2U);
  hol_course_free(course);

  char command[8192];
  (void)snprintf(command, sizeof(command), "rm -rf -- '%s'", root);
  assert(system(command) == 0);
  puts("catalog tests passed");
  return 0;
}
