#include "hol.h"

#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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

static json_object *catalog_required(json_object *parent, const char *name,
                                     enum json_type type, hol_error *error) {
  json_object *value = NULL;
  if (!json_object_object_get_ex(parent, name, &value) ||
      !json_object_is_type(value, type)) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid catalog field: %s", name);
    return NULL;
  }
  return value;
}

static int catalog_copy(char *target, size_t size, json_object *parent,
                        const char *name, hol_error *error) {
  json_object *value = catalog_required(parent, name, json_type_string, error);
  if (value == NULL) return -1;
  const char *text = json_object_get_string(value);
  size_t length = strlen(text);
  if ((size_t)json_object_get_string_len(value) != length || length >= size) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid catalog field: %s", name);
    return -1;
  }
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

static int parse_entry(json_object *object, catalog_entry *entry,
                       hol_error *error) {
  json_object *license = catalog_required(object, "license", json_type_object, error);
  json_object *bundle = catalog_required(object, "bundle", json_type_object, error);
  json_object *lessons = catalog_required(object, "lesson_count", json_type_int, error);
  json_object *bytes = bundle != NULL
    ? catalog_required(bundle, "bytes", json_type_int, error) : NULL;
  if (license == NULL || bundle == NULL || lessons == NULL || bytes == NULL ||
      catalog_copy(entry->id, sizeof(entry->id), object, "id", error) < 0 ||
      catalog_copy(entry->version, sizeof(entry->version), object, "version", error) < 0 ||
      catalog_copy(entry->title, sizeof(entry->title), object, "title", error) < 0 ||
      catalog_copy(entry->description, sizeof(entry->description), object, "description", error) < 0 ||
      catalog_copy(entry->minimum_app_version, sizeof(entry->minimum_app_version), object,
                   "minimum_app_version", error) < 0 ||
      catalog_copy(entry->license_spdx, sizeof(entry->license_spdx), license, "spdx", error) < 0 ||
      catalog_copy(entry->attribution, sizeof(entry->attribution), license, "attribution", error) < 0 ||
      catalog_copy(entry->url, sizeof(entry->url), bundle, "url", error) < 0 ||
      catalog_copy(entry->sha256, sizeof(entry->sha256), bundle, "sha256", error) < 0)
    return -1;
  int64_t lesson_count = json_object_get_int64(lessons);
  int64_t byte_count = json_object_get_int64(bytes);
  if (lesson_count < 1 || lesson_count > 10000 || byte_count < 1 ||
      (uint64_t)byte_count > maximum_bundle_bytes || !hol_valid_id(entry->id) ||
      !hol_version_supported(entry->minimum_app_version) ||
      !digest_string(entry->sha256) ||
      !(strncmp(entry->url, "https://", 8U) == 0 ||
        strncmp(entry->url, "file://", 7U) == 0)) return -1;
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
  json_object *schema = catalog_required(catalog, "schema_version", json_type_int, error);
  json_object *courses = catalog_required(catalog, "courses", json_type_array, error);
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
  size_t count = json_object_array_length(courses);
  for (size_t index = 0U; index < count; index++) {
    catalog_entry entry = {0};
    if (parse_entry(json_object_array_get_idx(courses, index), &entry, error) < 0) {
      json_object_put(catalog);
      return -1;
    }
    (void)fprintf(stream, "%s\t%s\t%zu lessons\t%s\t%s\t%s\n", entry.id,
                   entry.version, entry.lesson_count, entry.license_spdx,
                   entry.title, entry.attribution);
  }
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
  download_target target = {
    .descriptor = descriptor,
    .limit = entry->bytes,
  };
  (void)curl_easy_setopt(curl, CURLOPT_URL, entry->url);
  (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,
                         strncmp(entry->url, "file://", 7U) == 0 ? "file" : "https");
  (void)curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                         strncmp(entry->url, "file://", 7U) == 0 ? "file" : "https");
  (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
  (void)curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &target);
  CURLcode result = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  if (fsync(descriptor) < 0 || close(descriptor) < 0 || result != CURLE_OK ||
      target.failed || target.written != entry->bytes) {
    hol_error_set(error, HOL_ERR_IO, "course download failed");
    (void)unlink(path);
    return -1;
  }
  char actual[65];
  if (hol_sha256_file(path, actual, error) < 0 || strcmp(actual, entry->sha256) != 0) {
    hol_error_set(error, HOL_ERR_CHECKSUM, "download checksum mismatch");
    (void)unlink(path);
    return -1;
  }
  return 0;
}

static uint64_t octal_value(const unsigned char *field, size_t length, bool *valid) {
  uint64_t value = 0U;
  *valid = true;
  size_t index = 0U;
  while (index < length && (field[index] == ' ' || field[index] == '\0')) index++;
  for (; index < length && field[index] != '\0' && field[index] != ' '; index++) {
    if (field[index] < '0' || field[index] > '7') {
      *valid = false;
      return 0U;
    }
    value = value * 8U + (uint64_t)(field[index] - '0');
  }
  return value;
}

static bool zero_block(const unsigned char block[512]) {
  for (size_t index = 0U; index < 512U; index++) if (block[index] != 0U) return false;
  return true;
}

static int ensure_directories(const char *path) {
  char copy[4096];
  int written = snprintf(copy, sizeof(copy), "%s", path);
  if (written < 0 || (size_t)written >= sizeof(copy)) return -1;
  for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(copy, 0700) < 0 && errno != EEXIST) return -1;
    *cursor = '/';
  }
  return 0;
}

static int read_exact(int descriptor, void *buffer, size_t length) {
  size_t offset = 0U;
  while (offset < length) {
    ssize_t count = read(descriptor, (char *)buffer + offset, length - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return -1;
    offset += (size_t)count;
  }
  return 0;
}

static int extract_tar(const char *archive, const char *staging, char top[256],
                       hol_error *error) {
  int descriptor = open(archive, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return -1;
  size_t entries = 0U;
  uint64_t expanded = 0U;
  for (;;) {
    unsigned char header[512];
    if (read_exact(descriptor, header, sizeof(header)) < 0) break;
    if (zero_block(header)) {
      (void)close(descriptor);
      return entries > 0U ? 0 : -1;
    }
    bool checksum_valid = false;
    bool size_valid = false;
    uint64_t stored_checksum = octal_value(header + 148U, 8U, &checksum_valid);
    uint64_t checksum = 0U;
    for (size_t index = 0U; index < sizeof(header); index++)
      checksum += index >= 148U && index < 156U ? 0x20U : header[index];
    uint64_t size = octal_value(header + 124U, 12U, &size_valid);
    if (!checksum_valid || !size_valid || checksum != stored_checksum ||
        ++entries > 100000U ||
        size > maximum_bundle_bytes - expanded) goto invalid;
    expanded += size;
    char name[256];
    size_t name_length = strnlen((const char *)header, 100U);
    size_t prefix_length = strnlen((const char *)header + 345U, 155U);
    int written = prefix_length > 0U
      ? snprintf(name, sizeof(name), "%.*s/%.*s", (int)prefix_length, header + 345U,
                 (int)name_length, header)
      : snprintf(name, sizeof(name), "%.*s", (int)name_length, header);
    if (written < 0 || (size_t)written >= sizeof(name)) goto invalid;
    while (written > 0 && name[written - 1] == '/') name[--written] = '\0';
    if (!hol_safe_relative_path(name)) goto invalid;
    const char *slash = strchr(name, '/');
    size_t top_length = slash != NULL ? (size_t)(slash - name) : strlen(name);
    if (top[0] == '\0') {
      if (top_length >= 256U) goto invalid;
      memcpy(top, name, top_length);
      top[top_length] = '\0';
    } else if (strlen(top) != top_length || strncmp(top, name, top_length) != 0) goto invalid;
    char destination[4096];
    written = snprintf(destination, sizeof(destination), "%s/%s", staging, name);
    if (written < 0 || (size_t)written >= sizeof(destination)) goto invalid;
    unsigned char type = header[156U];
    if (type == '5') {
      if (ensure_directories(destination) < 0 ||
          (mkdir(destination, 0700) < 0 && errno != EEXIST)) goto invalid;
    } else if (type == '0' || type == '\0') {
      if (ensure_directories(destination) < 0) goto invalid;
      int output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (output < 0) goto invalid;
      uint64_t remaining = size;
      unsigned char block[512];
      while (remaining > 0U) {
        if (read_exact(descriptor, block, sizeof(block)) < 0) {
          (void)close(output);
          goto invalid;
        }
        size_t amount = remaining < sizeof(block) ? (size_t)remaining : sizeof(block);
        size_t offset = 0U;
        while (offset < amount) {
          ssize_t count = write(output, block + offset, amount - offset);
          if (count < 0 && errno == EINTR) continue;
          if (count <= 0) {
            (void)close(output);
            goto invalid;
          }
          offset += (size_t)count;
        }
        remaining -= amount;
      }
      if (close(output) < 0) goto invalid;
    } else goto invalid;
    if ((type == '5') && size > 0U) goto invalid;
  }

invalid:
  (void)close(descriptor);
  hol_error_set(error, HOL_ERR_SCHEMA, "unsafe or invalid course archive");
  return -1;
}

static int remove_tree(const char *path) {
  DIR *directory = opendir(path);
  if (directory == NULL) return errno == ENOENT ? 0 : -1;
  struct dirent *item;
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
    char child[4096];
    int written = snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
    if (written < 0 || (size_t)written >= sizeof(child)) return -1;
    struct stat status;
    if (lstat(child, &status) < 0) return -1;
    if (S_ISDIR(status.st_mode)) {
      if (remove_tree(child) < 0) return -1;
    } else if (unlink(child) < 0) return -1;
  }
  (void)closedir(directory);
  return rmdir(path);
}

int hol_catalog_install(const char *catalog_path, const char *course_id,
                        const char *destination, hol_error *error) {
  json_object *catalog = NULL;
  if (load_catalog(catalog_path, &catalog, error) < 0) return -1;
  json_object *courses = NULL;
  (void)json_object_object_get_ex(catalog, "courses", &courses);
  catalog_entry selected = {0};
  bool found = false;
  bool valid = true;
  for (size_t index = 0U; index < json_object_array_length(courses); index++) {
    catalog_entry entry = {0};
    if (parse_entry(json_object_array_get_idx(courses, index), &entry, error) < 0) {
      valid = false;
      break;
    }
    if (strcmp(entry.id, course_id) == 0) {
      selected = entry;
      found = true;
      break;
    }
  }
  json_object_put(catalog);
  if (!valid) return -1;
  if (!found) {
    hol_error_set(error, HOL_ERR_ARGUMENT, "course is not in the catalog: %s", course_id);
    return -1;
  }
  if (ensure_directories(destination) < 0 ||
      (mkdir(destination, 0700) < 0 && errno != EEXIST)) return -1;
  struct stat destination_status;
  if (lstat(destination, &destination_status) < 0 ||
      !S_ISDIR(destination_status.st_mode)) {
    hol_error_set(error, HOL_ERR_PATH, "course destination is not a directory");
    return -1;
  }
  char archive[4096];
  char staging[4096];
  int written = snprintf(archive, sizeof(archive), "%s/.download.XXXXXX", destination);
  if (written < 0 || (size_t)written >= sizeof(archive)) return -1;
  int placeholder = mkstemp(archive);
  if (placeholder < 0) return -1;
  (void)close(placeholder);
  (void)unlink(archive);
  written = snprintf(staging, sizeof(staging), "%s/.install.XXXXXX", destination);
  if (written < 0 || (size_t)written >= sizeof(staging) || mkdtemp(staging) == NULL) return -1;
  if (download_bundle(&selected, archive, error) < 0) goto failure;
  char top[256] = {0};
  if (extract_tar(archive, staging, top, error) < 0) goto failure;
  char extracted[4096];
  char final[4096];
  written = snprintf(extracted, sizeof(extracted), "%s/%s", staging, top);
  if (written < 0 || (size_t)written >= sizeof(extracted)) goto failure;
  written = snprintf(final, sizeof(final), "%s/%s.holcourse", destination, selected.id);
  if (written < 0 || (size_t)written >= sizeof(final)) goto failure;
  hol_course *course = NULL;
  if (hol_course_load(extracted, &course, error) < 0) goto failure;
  bool matches = strcmp(course->id, selected.id) == 0 &&
                  strcmp(course->version, selected.version) == 0 &&
                  course->lesson_count == selected.lesson_count &&
                  strcmp(course->license_spdx, selected.license_spdx) == 0 &&
                  strcmp(course->attribution, selected.attribution) == 0 &&
                  strcmp(course->minimum_app_version,
                         selected.minimum_app_version) == 0;
  hol_course_free(course);
  if (!matches || access(final, F_OK) == 0 || rename(extracted, final) < 0) {
    hol_error_set(error, HOL_ERR_SCHEMA, "installed course does not match catalog metadata");
    goto failure;
  }
  (void)unlink(archive);
  (void)rmdir(staging);
  return 0;

failure:
  (void)unlink(archive);
  (void)remove_tree(staging);
  return -1;
}
