#include "hol.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char id[HOL_ID_MAX + 1];
  char version[32];
  char title[HOL_TEXT_MAX + 1];
  char description[1025];
  char license_spdx[32];
  char attribution[1025];
  char minimum_app_version[32];
  char url[2049];
  char sha256[65];
  uint64_t bytes;
  size_t lesson_count;
} catalog_entry;

typedef struct {
  int descriptor;
  uint64_t limit;
  uint64_t written;
  bool failed;
} download_target;

static const uint64_t maximum_bundle_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

static json_object *required(json_object *parent, const char *name,
                             enum json_type type, hol_error *error) {
  json_object *value = NULL;
  if (!json_object_object_get_ex(parent, name, &value) ||
      !json_object_is_type(value, type)) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid catalog field: %s", name);
    return NULL;
  }
  return value;
}

static int copy_field(char *target, size_t size, json_object *parent,
                      const char *name, hol_error *error) {
  json_object *value = required(parent, name, json_type_string, error);
  if (value == NULL) return -1;
  const char *text = json_object_get_string(value);
  size_t length = strlen(text);
  if ((size_t)json_object_get_string_len(value) != length || length >= size) return -1;
  memcpy(target, text, length + 1U);
  return 0;
}

static bool digest_string(const char *value) {
  if (strlen(value) != 64U) return false;
  for (size_t index = 0U; index < 64U; index++)
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) return false;
  return true;
}

static bool cartridge_url(const char *url) {
  size_t length = strlen(url);
  return length > 6U && strcmp(url + length - 6U, ".imscc") == 0 &&
         (strncmp(url, "https://", 8U) == 0 || strncmp(url, "file://", 7U) == 0);
}

static int parse_entry(json_object *object, catalog_entry *entry,
                       hol_error *error) {
  json_object *license = required(object, "license", json_type_object, error);
  json_object *bundle = required(object, "bundle", json_type_object, error);
  json_object *lessons = required(object, "lesson_count", json_type_int, error);
  json_object *bytes = bundle != NULL ? required(bundle, "bytes", json_type_int, error) : NULL;
  if (license == NULL || bundle == NULL || lessons == NULL || bytes == NULL ||
      copy_field(entry->id, sizeof(entry->id), object, "id", error) < 0 ||
      copy_field(entry->version, sizeof(entry->version), object, "version", error) < 0 ||
      copy_field(entry->title, sizeof(entry->title), object, "title", error) < 0 ||
      copy_field(entry->description, sizeof(entry->description), object,
                 "description", error) < 0 ||
      copy_field(entry->minimum_app_version, sizeof(entry->minimum_app_version), object,
                 "minimum_app_version", error) < 0 ||
      copy_field(entry->license_spdx, sizeof(entry->license_spdx), license,
                 "spdx", error) < 0 ||
      copy_field(entry->attribution, sizeof(entry->attribution), license,
                 "attribution", error) < 0 ||
      copy_field(entry->url, sizeof(entry->url), bundle, "url", error) < 0 ||
      copy_field(entry->sha256, sizeof(entry->sha256), bundle, "sha256", error) < 0)
    return -1;
  int64_t lesson_count = json_object_get_int64(lessons);
  int64_t byte_count = json_object_get_int64(bytes);
  if (lesson_count < 1 || lesson_count > 10000 || byte_count < 1 ||
      (uint64_t)byte_count > maximum_bundle_bytes || !hol_valid_id(entry->id) ||
      !hol_version_supported(entry->minimum_app_version) ||
      !digest_string(entry->sha256) || !cartridge_url(entry->url)) return -1;
  entry->lesson_count = (size_t)lesson_count;
  entry->bytes = (uint64_t)byte_count;
  return 0;
}

static int load_catalog(const char *path, json_object **output, hol_error *error) {
  size_t length = 0U;
  char *text = hol_read_text(path, 4U * 1024U * 1024U, &length, error);
  if (text == NULL) return -1;
  json_tokener *tokener = json_tokener_new_ex(32);
  json_object *catalog = json_tokener_parse_ex(tokener, text, (int)length);
  enum json_tokener_error status = json_tokener_get_error(tokener);
  json_tokener_free(tokener);
  free(text);
  if (catalog == NULL || status != json_tokener_success ||
      !json_object_is_type(catalog, json_type_object)) {
    if (catalog != NULL) json_object_put(catalog);
    hol_error_set(error, HOL_ERR_JSON, "catalog is not valid JSON");
    return -1;
  }
  json_object *schema = required(catalog, "schema_version", json_type_int, error);
  json_object *courses = required(catalog, "courses", json_type_array, error);
  if (schema == NULL || courses == NULL || json_object_get_int(schema) != 1 ||
      json_object_array_length(courses) > 10000U) {
    json_object_put(catalog);
    return -1;
  }
  *output = catalog;
  return 0;
}

int hol_catalog_list(const char *catalog_path, FILE *stream, hol_error *error) {
  json_object *catalog = NULL;
  if (load_catalog(catalog_path, &catalog, error) < 0) return -1;
  json_object *courses = NULL;
  (void)json_object_object_get_ex(catalog, "courses", &courses);
  for (size_t index = 0U; index < json_object_array_length(courses); index++) {
    catalog_entry entry = {0};
    if (parse_entry(json_object_array_get_idx(courses, index), &entry, error) < 0) {
      json_object_put(catalog);
      return -1;
    }
    for (char *cursor = entry.attribution; *cursor != '\0'; cursor++)
      if (*cursor == '\r' || *cursor == '\n') *cursor = ' ';
    (void)fprintf(stream, "%s\t%s\t%zu lessons\t%s\t%s\t%s\n", entry.id,
                  entry.version, entry.lesson_count, entry.license_spdx,
                  entry.title, entry.attribution);
  }
  json_object_put(catalog);
  return 0;
}

int hol_catalog_select(const char *catalog_path, FILE *input, FILE *output,
                       char course_id[HOL_ID_MAX + 1], hol_error *error) {
  json_object *catalog = NULL;
  if (load_catalog(catalog_path, &catalog, error) < 0) return -1;
  json_object *courses = NULL;
  (void)json_object_object_get_ex(catalog, "courses", &courses);
  size_t count = json_object_array_length(courses);
  if (count == 0U) {
    json_object_put(catalog);
    hol_error_set(error, HOL_ERR_SCHEMA, "the course catalog is empty");
    return -1;
  }
  (void)fprintf(output, "Choose a course:\n\n");
  for (size_t index = 0U; index < count; index++) {
    catalog_entry entry = {0};
    if (parse_entry(json_object_array_get_idx(courses, index), &entry, error) < 0) {
      json_object_put(catalog);
      return -1;
    }
    (void)fprintf(output, "  %zu. %s\n     %s\n", index + 1U, entry.title,
                  entry.description);
  }
  (void)fprintf(output, "\nEnter 1 to %zu: ", count);
  (void)fflush(output);
  char response[32];
  if (fgets(response, sizeof(response), input) == NULL) {
    json_object_put(catalog);
    hol_error_set(error, HOL_ERR_ARGUMENT, "no course was selected");
    return -1;
  }
  char *end = NULL;
  errno = 0;
  unsigned long selection = strtoul(response, &end, 10);
  while (end != NULL && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
    end++;
  if (errno != 0 || end == response || end == NULL || *end != '\0' ||
      selection < 1UL || selection > count) {
    json_object_put(catalog);
    hol_error_set(error, HOL_ERR_ARGUMENT, "invalid course selection");
    return -1;
  }
  catalog_entry selected = {0};
  if (parse_entry(json_object_array_get_idx(courses, selection - 1UL),
                  &selected, error) < 0) {
    json_object_put(catalog);
    return -1;
  }
  (void)snprintf(course_id, HOL_ID_MAX + 1U, "%s", selected.id);
  json_object_put(catalog);
  return 0;
}

static size_t write_download(char *data, size_t size, size_t count, void *user_data) {
  download_target *target = user_data;
  if (size != 0U && count > SIZE_MAX / size) {
    target->failed = true;
    return 0U;
  }
  size_t length = size * count;
  if (target->written > target->limit || length > target->limit - target->written) {
    target->failed = true;
    return 0U;
  }
  size_t offset = 0U;
  while (offset < length) {
    ssize_t written = write(target->descriptor, data + offset, length - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      target->failed = true;
      return 0U;
    }
    offset += (size_t)written;
  }
  target->written += length;
  return length;
}

static int download_bundle(const catalog_entry *entry, const char *path,
                           hol_error *error) {
  int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) return -1;
  CURL *curl = curl_easy_init();
  if (curl == NULL) {
    (void)close(descriptor);
    return -1;
  }
  download_target target = {.descriptor = descriptor, .limit = entry->bytes};
  const char *protocol = strncmp(entry->url, "file://", 7U) == 0 ? "file" : "https";
  (void)curl_easy_setopt(curl, CURLOPT_URL, entry->url);
  (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocol);
  (void)curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocol);
  (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
  (void)curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &target);
  CURLcode result = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  int sync_status = fsync(descriptor);
  int close_status = close(descriptor);
  if (sync_status < 0 || close_status < 0 || result != CURLE_OK ||
      target.failed || target.written != entry->bytes) goto failure;
  char actual[65];
  if (hol_sha256_file(path, actual, error) < 0 || strcmp(actual, entry->sha256) != 0) {
    hol_error_set(error, HOL_ERR_CHECKSUM, "download checksum mismatch");
    goto failure;
  }
  return 0;

failure:
  (void)unlink(path);
  if (error != NULL && error->message[0] == '\0')
    hol_error_set(error, HOL_ERR_IO, "course download failed");
  return -1;
}

static int ensure_directories(const char *path) {
  char copy[4096];
  int length = snprintf(copy, sizeof(copy), "%s", path);
  if (length < 0 || (size_t)length >= sizeof(copy)) return -1;
  for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(copy, 0700) < 0 && errno != EEXIST) return -1;
    *cursor = '/';
  }
  return 0;
}

int hol_catalog_install_path(const char *catalog_path, const char *course_id,
                             const char *destination, char *course_path,
                             size_t course_path_size, hol_error *error) {
  json_object *catalog = NULL;
  if (load_catalog(catalog_path, &catalog, error) < 0) return -1;
  json_object *courses = NULL;
  (void)json_object_object_get_ex(catalog, "courses", &courses);
  catalog_entry selected = {0};
  bool found = false;
  for (size_t index = 0U; index < json_object_array_length(courses); index++) {
    catalog_entry entry = {0};
    if (parse_entry(json_object_array_get_idx(courses, index), &entry, error) < 0) {
      json_object_put(catalog);
      return -1;
    }
    if (strcmp(entry.id, course_id) == 0) {
      selected = entry;
      found = true;
      break;
    }
  }
  json_object_put(catalog);
  if (!found) {
    hol_error_set(error, HOL_ERR_ARGUMENT, "course is not in the catalog: %s", course_id);
    return -1;
  }
  if (ensure_directories(destination) < 0 ||
      (mkdir(destination, 0700) < 0 && errno != EEXIST)) return -1;
  struct stat status;
  if (lstat(destination, &status) < 0 || !S_ISDIR(status.st_mode)) return -1;
  char final[4096];
  int length = snprintf(final, sizeof(final), "%s/%s-%s.imscc", destination,
                        selected.id, selected.version);
  if (length < 0 || (size_t)length >= sizeof(final) ||
      (course_path != NULL && (size_t)length >= course_path_size)) return -1;
  if (access(final, R_OK) == 0) {
    if (course_path != NULL) (void)snprintf(course_path, course_path_size, "%s", final);
    return 0;
  }
  char temporary[4096];
  length = snprintf(temporary, sizeof(temporary), "%s/.download.XXXXXX.imscc",
                    destination);
  if (length < 0 || (size_t)length >= sizeof(temporary)) return -1;
  int placeholder = mkstemps(temporary, 6);
  if (placeholder < 0) return -1;
  (void)close(placeholder);
  (void)unlink(temporary);
  if (download_bundle(&selected, temporary, error) < 0) return -1;
  hol_course *course = NULL;
  if (hol_course_load(temporary, &course, error) < 0) goto failure;
  bool matches = strcmp(course->id, selected.id) == 0 &&
                 strcmp(course->version, selected.version) == 0 &&
                 strcmp(course->title, selected.title) == 0 &&
                 course->lesson_count == selected.lesson_count &&
                 strcmp(course->license_spdx, selected.license_spdx) == 0 &&
                 strcmp(course->attribution, selected.attribution) == 0;
  hol_course_free(course);
  if (!matches || rename(temporary, final) < 0) {
    hol_error_set(error, HOL_ERR_SCHEMA, "installed cartridge does not match catalog metadata");
    goto failure;
  }
  if (course_path != NULL) (void)snprintf(course_path, course_path_size, "%s", final);
  return 0;

failure:
  (void)unlink(temporary);
  return -1;
}

int hol_catalog_install(const char *catalog_path, const char *course_id,
                        const char *destination, hol_error *error) {
  return hol_catalog_install_path(catalog_path, course_id, destination, NULL, 0U,
                                  error);
}
