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
  char catalog[4096];
  char installed[4096];
  hol_error error = {0};
  assert(hol_join_path(archive, sizeof(archive), root, "demo.holcourse.tar", &error) == 0);
  assert(hol_join_path(catalog, sizeof(catalog), root, "catalog.json", &error) == 0);
  assert(hol_join_path(installed, sizeof(installed), root, "data/courses", &error) == 0);

  char command[8192];
  (void)snprintf(command, sizeof(command),
                 "tar --format=ustar -cf '%s' -C courses demo.holcourse", archive);
  assert(system(command) == 0);
  struct stat status;
  assert(stat(archive, &status) == 0);
  char digest[65];
  assert(hol_sha256_file(archive, digest, &error) == 0);
  char manifest[8192];
  int length = snprintf(manifest, sizeof(manifest),
    "{\"schema_version\":1,\"updated_at\":\"2026-08-09T00:00:00Z\",\"courses\":[{"
    "\"id\":\"hol.demo-c\",\"version\":\"1.0.0\",\"title\":\"C in Three Small Steps\","
    "\"description\":\"Demo\",\"minimum_app_version\":\"0.1.0\",\"lesson_count\":3,"
    "\"license\":{\"spdx\":\"MIT\",\"attribution\":\"Hands-on Learning Contributors\"},"
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
  assert(strstr(listing, "hol.demo-c") != NULL);
  free(listing);

  assert(hol_catalog_install(catalog, "hol.demo-c", installed, &error) == 0);
  char course_path[4096];
  assert(hol_join_path(course_path, sizeof(course_path), installed,
                       "hol.demo-c.holcourse", &error) == 0);
  hol_course *course = NULL;
  assert(hol_course_load(course_path, &course, &error) == 0);
  assert(course->lesson_count == 3U);
  hol_course_free(course);

  (void)snprintf(command, sizeof(command), "rm -rf -- '%s'", root);
  assert(system(command) == 0);
  puts("catalog tests passed");
  return 0;
}
