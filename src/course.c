#include "hol.h"

#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char manifest_namespace[] =
  "http://www.imsglobal.org/xsd/imsccv1p3/imscp_v1p1";
static const char qti_namespace[] =
  "http://www.imsglobal.org/xsd/ims_qtiasiv1p2";
static const char lom_manifest_namespace[] =
  "http://ltsc.ieee.org/xsd/imsccv1p3/LOM/manifest";
static const char qti_assessment_type[] =
  "imsqti_xmlv1p2/imscc_xmlv1p3/assessment";
static const uint64_t maximum_expanded_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

static int copy_string(char *target, size_t capacity, const char *value,
                       const char *field, hol_error *error) {
  size_t length = strlen(value);
  if (length >= capacity) {
    hol_error_set(error, HOL_ERR_SCHEMA, "%s exceeds %zu bytes", field,
                  capacity - 1U);
    return -1;
  }
  memcpy(target, value, length + 1U);
  return 0;
}

static int make_directories(const char *path, hol_error *error) {
  char copy[4096];
  if (copy_string(copy, sizeof(copy), path, "path", error) < 0) return -1;
  for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(copy, 0700) < 0 && errno != EEXIST) return -1;
    *cursor = '/';
  }
  return 0;
}

static int remove_tree(const char *path) {
  DIR *directory = opendir(path);
  if (directory == NULL) return errno == ENOENT ? 0 : -1;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char child[4096];
    int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (length < 0 || (size_t)length >= sizeof(child)) {
      (void)closedir(directory);
      return -1;
    }
    struct stat status;
    if (lstat(child, &status) < 0) {
      (void)closedir(directory);
      return -1;
    }
    if (S_ISDIR(status.st_mode)) {
      if (remove_tree(child) < 0) {
        (void)closedir(directory);
        return -1;
      }
    } else if (unlink(child) < 0) {
      (void)closedir(directory);
      return -1;
    }
  }
  (void)closedir(directory);
  return rmdir(path);
}

static int extract_cartridge(const char *source, char root[4096], hol_error *error) {
  size_t source_length = strlen(source);
  if (source_length < 6U || strcmp(source + source_length - 6U, ".imscc") != 0) {
    hol_error_set(error, HOL_ERR_UNSUPPORTED, "course package must use the .imscc extension");
    return -1;
  }
  char canonical[4096];
  if (realpath(source, canonical) == NULL) {
    hol_error_set(error, HOL_ERR_PATH, "cannot resolve cartridge path");
    return -1;
  }
  (void)snprintf(root, 4096, "/tmp/hol-imscc-XXXXXX");
  if (mkdtemp(root) == NULL) return -1;
  struct archive *archive = archive_read_new();
  if (archive == NULL) goto failure;
  archive_read_support_format_zip(archive);
  if (archive_read_open_filename(archive, canonical, 64U * 1024U) != ARCHIVE_OK)
    goto archive_failure;
  size_t entries = 0U;
  uint64_t expanded = 0U;
  struct archive_entry *entry = NULL;
  int header_status = ARCHIVE_OK;
  while ((header_status = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
    const char *archive_path = archive_entry_pathname(entry);
    char relative[HOL_PATH_MAX + 1];
    if (archive_path == NULL ||
        copy_string(relative, sizeof(relative), archive_path, "archive path", error) < 0)
      goto archive_failure;
    size_t relative_length = strlen(relative);
    while (relative_length > 0U && relative[relative_length - 1U] == '/')
      relative[--relative_length] = '\0';
    la_int64_t entry_size = archive_entry_size(entry);
    if (!hol_safe_relative_path(relative) ||
        archive_entry_symlink(entry) != NULL || archive_entry_hardlink(entry) != NULL ||
        entry_size < 0 || ++entries > 100000U ||
        (uint64_t)entry_size > maximum_expanded_bytes - expanded) goto archive_failure;
    expanded += (uint64_t)entry_size;
    char destination[4096];
    if (hol_join_path(destination, sizeof(destination), root, relative, error) < 0)
      goto archive_failure;
    mode_t type = archive_entry_filetype(entry);
    if (type == AE_IFDIR) {
      if (make_directories(destination, error) < 0 ||
          (mkdir(destination, 0700) < 0 && errno != EEXIST)) goto archive_failure;
      continue;
    }
    if (type != AE_IFREG || make_directories(destination, error) < 0)
      goto archive_failure;
    int output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) goto archive_failure;
    char buffer[64U * 1024U];
    for (;;) {
      la_ssize_t count = archive_read_data(archive, buffer, sizeof(buffer));
      if (count < 0) {
        (void)close(output);
        goto archive_failure;
      }
      if (count == 0) break;
      size_t offset = 0U;
      while (offset < (size_t)count) {
        ssize_t written = write(output, buffer + offset, (size_t)count - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
          (void)close(output);
          goto archive_failure;
        }
        offset += (size_t)written;
      }
    }
    if (close(output) < 0) goto archive_failure;
  }
  if (header_status != ARCHIVE_EOF) goto archive_failure;
  int close_status = archive_read_close(archive);
  int free_status = archive_read_free(archive);
  if (close_status != ARCHIVE_OK || free_status != ARCHIVE_OK || entries == 0U)
    goto failure;
  char manifest[4096];
  if (hol_join_path(manifest, sizeof(manifest), root, "imsmanifest.xml", error) < 0 ||
      access(manifest, R_OK) < 0) goto failure;
  return 0;

archive_failure:
  (void)archive_read_close(archive);
  (void)archive_read_free(archive);
failure:
  (void)remove_tree(root);
  hol_error_set(error, HOL_ERR_SCHEMA, "unsafe or invalid Common Cartridge package");
  return -1;
}

static xmlNode *child(xmlNode *parent, const char *name) {
  for (xmlNode *node = parent != NULL ? parent->children : NULL;
       node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE && node->ns != NULL && parent->ns != NULL &&
        xmlStrEqual(node->ns->href, parent->ns->href) &&
        strcmp((const char *)node->name, name) == 0) return node;
  return NULL;
}

static xmlNode *descendant(xmlNode *parent, const char *name) {
  for (xmlNode *node = parent != NULL ? parent->children : NULL;
       node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE || node->ns == NULL || parent->ns == NULL ||
        !xmlStrEqual(node->ns->href, parent->ns->href)) continue;
    if (strcmp((const char *)node->name, name) == 0) return node;
    xmlNode *nested = descendant(node, name);
    if (nested != NULL) return nested;
  }
  return NULL;
}

static xmlNode *child_namespace(xmlNode *parent, const char *name,
                                const char *namespace) {
  for (xmlNode *node = parent != NULL ? parent->children : NULL;
       node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE && node->ns != NULL &&
        strcmp((const char *)node->name, name) == 0 &&
        strcmp((const char *)node->ns->href, namespace) == 0) return node;
  return NULL;
}

static bool bounded_regular_file(const char *path, uint64_t maximum) {
  struct stat status;
  return lstat(path, &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_nlink == 1 && status.st_size >= 0 &&
         (uint64_t)status.st_size <= maximum;
}

static int attribute(xmlNode *node, const char *name, char *target, size_t capacity,
                     hol_error *error);
static int resource_by_id(xmlNode *resources, const char *identifier,
                          xmlNode **found, hol_error *error);

static bool digest_string(const char *value) {
  if (strlen(value) != 64U) return false;
  for (size_t index = 0U; index < 64U; index++)
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) return false;
  return true;
}

static json_object *json_required(json_object *parent, const char *name,
                                  enum json_type type, hol_error *error) {
  json_object *value = NULL;
  if (!json_object_object_get_ex(parent, name, &value) ||
      !json_object_is_type(value, type)) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid exercise profile field: %s", name);
    return NULL;
  }
  return value;
}

static bool name_allowed(const char *name, const char *const names[], size_t count) {
  for (size_t index = 0U; index < count; index++)
    if (strcmp(name, names[index]) == 0) return true;
  return false;
}

static int json_exact_keys(json_object *object, const char *const names[], size_t count,
                           const char *context, hol_error *error) {
  if (!json_object_is_type(object, json_type_object)) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid exercise profile %s fields", context);
    return -1;
  }
  json_object_object_foreach(object, name, value) {
    (void)value;
    if (!name_allowed(name, names, count)) {
      hol_error_set(error, HOL_ERR_SCHEMA, "unsupported exercise profile field: %s", name);
      return -1;
    }
  }
  if ((size_t)json_object_object_length(object) != count) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid exercise profile %s fields", context);
    return -1;
  }
  return 0;
}

static int json_string_field(char *target, size_t capacity, json_object *parent,
                             const char *name, hol_error *error) {
  json_object *value = json_required(parent, name, json_type_string, error);
  if (value == NULL) return -1;
  const char *text = json_object_get_string(value);
  size_t length = strlen(text);
  if ((size_t)json_object_get_string_len(value) != length)
    return -1;
  return copy_string(target, capacity, text, name, error);
}

static hol_lesson *lesson_by_id(hol_course *course, const char *id) {
  hol_lesson *found = NULL;
  for (size_t chapter = 0U; chapter < course->chapter_count; chapter++)
    for (size_t lesson = 0U; lesson < course->chapters[chapter].lesson_count; lesson++)
      if (strcmp(course->chapters[chapter].lessons[lesson].id, id) == 0) {
        if (found != NULL) return NULL;
        found = &course->chapters[chapter].lessons[lesson];
      }
  return found;
}

static int resource_file_by_path(xmlNode *resource, const char *path,
                                 xmlNode **found, size_t *file_count,
                                 hol_error *error) {
  *found = NULL;
  if (file_count != NULL) *file_count = 0U;
  for (xmlNode *file = resource != NULL ? resource->children : NULL;
       file != NULL; file = file->next) {
    if (file->type != XML_ELEMENT_NODE ||
        strcmp((const char *)file->name, "file") != 0) continue;
    if (file->ns == NULL ||
        strcmp((const char *)file->ns->href, manifest_namespace) != 0) {
      hol_error_set(error, HOL_ERR_SCHEMA,
                    "Common Cartridge file uses a foreign namespace");
      return -1;
    }
    if (file_count != NULL) (*file_count)++;
    xmlChar *href = xmlGetProp(file, (const xmlChar *)"href");
    if (href == NULL) {
      hol_error_set(error, HOL_ERR_SCHEMA,
                    "Common Cartridge file is missing href");
      return -1;
    }
    bool matches = path == NULL || strcmp((const char *)href, path) == 0;
    xmlFree(href);
    if (matches) {
      if (path != NULL && *found != NULL) {
        hol_error_set(error, HOL_ERR_SCHEMA,
                      "duplicate Common Cartridge file path: %s", path);
        return -1;
      }
      *found = file;
    }
  }
  if (*found == NULL) {
    hol_error_set(error, HOL_ERR_SCHEMA,
                  "resource does not declare Common Cartridge file: %s",
                  path != NULL ? path : "<any>");
    return -1;
  }
  return 0;
}

static int find_item_by_id(xmlNode *node, const char *identifier,
                           xmlNode **found) {
  for (xmlNode *current = node != NULL ? node->children : NULL;
       current != NULL; current = current->next) {
    if (current->type != XML_ELEMENT_NODE || current->ns == NULL ||
        strcmp((const char *)current->ns->href, manifest_namespace) != 0) continue;
    if (strcmp((const char *)current->name, "item") == 0) {
      xmlChar *value = xmlGetProp(current, (const xmlChar *)"identifier");
      bool matches = value != NULL && strcmp((const char *)value, identifier) == 0;
      xmlFree(value);
      if (matches) {
        if (*found != NULL) return -1;
        *found = current;
      }
    }
    if (find_item_by_id(current, identifier, found) < 0) return -1;
  }
  return 0;
}

static int parse_profile_file(const hol_course *course, xmlNode *resource,
                              json_object *object, hol_course_file *file,
                              hol_error *error) {
  static const char *const keys[] = {
    "source", "target", "role", "syntax", "phase", "sha256",
  };
  char role[16];
  char phase[16];
  if (json_exact_keys(object, keys, sizeof(keys) / sizeof(keys[0]), "file", error) < 0 ||
      json_string_field(file->source, sizeof(file->source), object, "source", error) < 0 ||
      json_string_field(file->target, sizeof(file->target), object, "target", error) < 0 ||
      json_string_field(role, sizeof(role), object, "role", error) < 0 ||
      json_string_field(file->syntax, sizeof(file->syntax), object, "syntax", error) < 0 ||
      json_string_field(phase, sizeof(phase), object, "phase", error) < 0 ||
      json_string_field(file->sha256, sizeof(file->sha256), object, "sha256", error) < 0 ||
      !hol_safe_relative_path(file->source) || !hol_safe_relative_path(file->target) ||
      !digest_string(file->sha256)) {
    if (error != NULL && error->message[0] == '\0')
      hol_error_set(error, HOL_ERR_SCHEMA, "invalid exercise profile file declaration");
    return -1;
  }
  xmlNode *declared_file = NULL;
  if (resource_file_by_path(resource, file->source, &declared_file, NULL, error) < 0)
    return -1;
  if (strcmp(role, "editable") == 0) file->role = HOL_FILE_EDITABLE;
  else if (strcmp(role, "readonly") == 0) file->role = HOL_FILE_READONLY;
  else if (strcmp(role, "hidden") == 0) file->role = HOL_FILE_HIDDEN;
  else return -1;
  if (strcmp(phase, "run") == 0) file->phase = HOL_PHASE_RUN;
  else if (strcmp(phase, "check") == 0) file->phase = HOL_PHASE_CHECK;
  else if (strcmp(phase, "both") == 0) file->phase = HOL_PHASE_BOTH;
  else return -1;
  if (file->role == HOL_FILE_EDITABLE && file->phase != HOL_PHASE_BOTH) return -1;
  char path[4096];
  char actual[65];
  if (hol_join_path(path, sizeof(path), course->root, file->source, error) < 0 ||
      !bounded_regular_file(path, 16U * 1024U * 1024U) ||
      hol_sha256_file(path, actual, error) < 0) return -1;
  if (strcmp(actual, file->sha256) != 0) {
    hol_error_set(error, HOL_ERR_CHECKSUM,
                  "exercise resource checksum mismatch: %s", file->source);
    return -1;
  }
  return 0;
}

static int parse_profile_runner(json_object *object, hol_lesson *lesson,
                                hol_error *error) {
  static const char *const runner_keys[] = {"id", "profile", "check"};
  static const char *const tests_keys[] = {"kind"};
  static const char *const stdout_keys[] = {"kind", "expected"};
  json_object *check = NULL;
  char kind[16];
  if (json_exact_keys(object, runner_keys, 3U, "runner", error) < 0 ||
      json_string_field(lesson->runner.id, sizeof(lesson->runner.id),
                        object, "id", error) < 0 ||
      json_string_field(lesson->runner.profile, sizeof(lesson->runner.profile),
                        object, "profile", error) < 0 ||
      (check = json_required(object, "check", json_type_object, error)) == NULL ||
      json_string_field(kind, sizeof(kind), check, "kind", error) < 0) return -1;
  bool c_runner = strcmp(lesson->runner.id, "c") == 0 &&
    (strcmp(lesson->runner.profile, "c11") == 0 ||
     strcmp(lesson->runner.profile, "c11-32") == 0 ||
     strcmp(lesson->runner.profile, "c23") == 0);
  bool sql_runner = strcmp(lesson->runner.id, "sql") == 0 &&
                    strcmp(lesson->runner.profile, "sqlite3") == 0;
  if (!c_runner && !sql_runner) return -1;
  if (strcmp(kind, "tests") == 0) {
    if (json_exact_keys(check, tests_keys, 1U, "test check", error) < 0) return -1;
    lesson->runner.check_kind = HOL_CHECK_TESTS;
    return 0;
  }
  if (sql_runner) return -1;
  if (strcmp(kind, "stdout") != 0 ||
      json_exact_keys(check, stdout_keys, 2U, "stdout check", error) < 0)
    return -1;
  json_object *expected = json_required(check, "expected", json_type_string, error);
  if (expected == NULL || json_object_get_string_len(expected) < 0 ||
      (size_t)json_object_get_string_len(expected) > HOL_OUTPUT_MAX ||
      strlen(json_object_get_string(expected)) !=
        (size_t)json_object_get_string_len(expected)) return -1;
  lesson->runner.expected_output = strdup(json_object_get_string(expected));
  if (lesson->runner.expected_output == NULL) return -1;
  lesson->runner.check_kind = HOL_CHECK_STDOUT;
  return 0;
}

static bool ends_with_text(const char *value, const char *suffix) {
  size_t value_length = strlen(value);
  size_t suffix_length = strlen(suffix);
  return value_length >= suffix_length &&
         strcmp(value + value_length - suffix_length, suffix) == 0;
}

static int parse_profile_lesson(hol_course *course, xmlNode *organizations,
                                xmlNode *resources,
                                json_object *object, hol_error *error) {
  static const char *const lesson_keys[] = {"id", "workspace", "runner"};
  static const char *const workspace_keys[] = {"default_file", "files"};
  char lesson_id[HOL_ID_MAX + 1];
  json_object *workspace = NULL;
  json_object *files = NULL;
  json_object *runner = NULL;
  if (json_exact_keys(object, lesson_keys, 3U, "lesson", error) < 0 ||
      json_string_field(lesson_id, sizeof(lesson_id), object, "id", error) < 0 ||
      !hol_valid_id(lesson_id) ||
      (workspace = json_required(object, "workspace", json_type_object, error)) == NULL ||
      json_exact_keys(workspace, workspace_keys, 2U, "workspace", error) < 0 ||
      (files = json_required(workspace, "files", json_type_array, error)) == NULL ||
      (runner = json_required(object, "runner", json_type_object, error)) == NULL)
    return -1;
  hol_lesson *lesson = lesson_by_id(course, lesson_id);
  xmlNode *manifest_item = NULL;
  char resource_id[HOL_ID_MAX + 1];
  xmlNode *resource = NULL;
  size_t count = json_object_array_length(files);
  if (lesson == NULL || lesson->kind != HOL_LESSON_READING || count == 0U || count > 64U ||
      find_item_by_id(organizations, lesson_id, &manifest_item) < 0 ||
      manifest_item == NULL ||
      attribute(manifest_item, "identifierref", resource_id, sizeof(resource_id), error) < 0 ||
       resource_by_id(resources, resource_id, &resource, error) < 0 ||
      json_string_field(lesson->default_file, sizeof(lesson->default_file),
                        workspace, "default_file", error) < 0 ||
      !hol_safe_relative_path(lesson->default_file)) return -1;
  if (parse_profile_runner(runner, lesson, error) < 0) return -1;
  const char *syntax = strcmp(lesson->runner.id, "c") == 0 ? "c" : "sql";
  const char *translation_suffix = strcmp(lesson->runner.id, "c") == 0 ? ".c" : ".sql";
  lesson->files = calloc(count, sizeof(*lesson->files));
  if (lesson->files == NULL) return -1;
  lesson->file_count = count;
  bool default_found = false;
  bool run_tests = false;
  bool check_tests = false;
  for (size_t index = 0U; index < count; index++) {
    json_object *item = json_object_array_get_idx(files, index);
    if (item == NULL || parse_profile_file(course, resource, item,
                                            &lesson->files[index], error) < 0) return -1;
    if (strcmp(lesson->files[index].syntax, syntax) != 0) return -1;
    for (size_t previous = 0U; previous < index; previous++)
      if (strcmp(lesson->files[previous].source, lesson->files[index].source) == 0 ||
          strcmp(lesson->files[previous].target, lesson->files[index].target) == 0)
        return -1;
    if (strcmp(lesson->files[index].target, lesson->default_file) == 0 &&
        lesson->files[index].role == HOL_FILE_EDITABLE) default_found = true;
    if (lesson->files[index].role == HOL_FILE_HIDDEN &&
        ends_with_text(lesson->files[index].target, translation_suffix) &&
        (lesson->files[index].phase & HOL_PHASE_RUN) != 0) run_tests = true;
    if (lesson->files[index].role == HOL_FILE_HIDDEN &&
        ends_with_text(lesson->files[index].target, translation_suffix) &&
        (lesson->files[index].phase & HOL_PHASE_CHECK) != 0) check_tests = true;
  }
  if (!default_found || !run_tests ||
      (lesson->runner.check_kind == HOL_CHECK_TESTS && !check_tests)) return -1;
  lesson->kind = HOL_LESSON_EXERCISE;
  return 0;
}

static int load_exercise_profile(hol_course *course, xmlNode *organizations,
                                 xmlNode *resources, const char *profile_path,
                                 hol_error *error) {
  static const char *const root_keys[] = {
    "schema_version", "cartridge_sha256", "course_id", "course_version", "lessons",
  };
  if (profile_path == NULL) return 0;
  if (!bounded_regular_file(profile_path, 4U * 1024U * 1024U)) {
    hol_error_set(error, HOL_ERR_SCHEMA, "exercise profile is not a bounded regular file");
    return -1;
  }
  size_t text_length = 0U;
  char *text = hol_read_text(profile_path, 4U * 1024U * 1024U, &text_length, error);
  if (text == NULL || text_length > INT_MAX) {
    free(text);
    return -1;
  }
  json_tokener *tokener = json_tokener_new_ex(32);
  if (tokener == NULL) {
    free(text);
    return -1;
  }
  json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
  json_object *profile = json_tokener_parse_ex(tokener, text, (int)text_length);
  enum json_tokener_error status = json_tokener_get_error(tokener);
  size_t parsed = json_tokener_get_parse_end(tokener);
  json_tokener_free(tokener);
  while (parsed < text_length && isspace((unsigned char)text[parsed])) parsed++;
  free(text);
  if (profile == NULL || status != json_tokener_success || parsed != text_length ||
      !json_object_is_type(profile, json_type_object)) {
    if (profile != NULL) json_object_put(profile);
    hol_error_set(error, HOL_ERR_JSON, "exercise profile is not strict JSON");
    return -1;
  }
  json_object *schema = NULL;
  json_object *lessons = NULL;
  char cartridge_sha256[65];
  char course_id[HOL_ID_MAX + 1];
  char course_version[32];
  int result = -1;
  if (json_exact_keys(profile, root_keys, 5U, "root", error) < 0 ||
      (schema = json_required(profile, "schema_version", json_type_int, error)) == NULL ||
      json_object_get_int64(schema) != 1 ||
      json_string_field(cartridge_sha256, sizeof(cartridge_sha256), profile,
                        "cartridge_sha256", error) < 0 ||
      json_string_field(course_id, sizeof(course_id), profile, "course_id", error) < 0 ||
      json_string_field(course_version, sizeof(course_version), profile,
                        "course_version", error) < 0 ||
      (lessons = json_required(profile, "lessons", json_type_array, error)) == NULL ||
      !digest_string(cartridge_sha256) || strcmp(course_id, course->id) != 0 ||
      strcmp(course_version, course->version) != 0 ||
      json_object_array_length(lessons) == 0U ||
      json_object_array_length(lessons) > course->lesson_count) goto done;
  char actual[65];
  if (hol_sha256_file(course->source_path, actual, error) < 0) goto done;
  if (strcmp(actual, cartridge_sha256) != 0) {
    hol_error_set(error, HOL_ERR_CHECKSUM,
                  "exercise profile does not match the cartridge checksum");
    goto done;
  }
  for (size_t index = 0U; index < json_object_array_length(lessons); index++) {
    json_object *item = json_object_array_get_idx(lessons, index);
    if (item == NULL || parse_profile_lesson(course, organizations, resources,
                                             item, error) < 0) goto done;
  }
  course->has_exercise_profile = true;
  result = 0;

done:
  json_object_put(profile);
  if (result < 0 && error != NULL && error->message[0] == '\0')
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid exercise profile");
  return result;
}

static int node_text(xmlNode *node, char *target, size_t capacity,
                     const char *field, hol_error *error) {
  if (node == NULL) return -1;
  xmlChar *value = xmlNodeGetContent(node);
  if (value == NULL) return -1;
  int result = copy_string(target, capacity, (const char *)value, field, error);
  xmlFree(value);
  return result;
}

static int attribute(xmlNode *node, const char *name, char *target, size_t capacity,
                     hol_error *error) {
  xmlChar *value = node != NULL ? xmlGetProp(node, (const xmlChar *)name) : NULL;
  if (value == NULL) return -1;
  int result = copy_string(target, capacity, (const char *)value, name, error);
  xmlFree(value);
  return result;
}

static int resource_by_id(xmlNode *resources, const char *identifier,
                          xmlNode **found, hol_error *error) {
  *found = NULL;
  for (xmlNode *node = resources != NULL ? resources->children : NULL;
       node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE ||
        strcmp((const char *)node->name, "resource") != 0 || node->ns == NULL ||
        strcmp((const char *)node->ns->href, manifest_namespace) != 0) continue;
    xmlChar *value = xmlGetProp(node, (const xmlChar *)"identifier");
    if (value == NULL) {
      hol_error_set(error, HOL_ERR_SCHEMA,
                    "Common Cartridge resource is missing identifier");
      return -1;
    }
    for (xmlNode *later = node->next; later != NULL; later = later->next) {
      if (later->type != XML_ELEMENT_NODE ||
          strcmp((const char *)later->name, "resource") != 0 || later->ns == NULL ||
          strcmp((const char *)later->ns->href, manifest_namespace) != 0) continue;
      xmlChar *later_value = xmlGetProp(later, (const xmlChar *)"identifier");
      bool duplicate = later_value != NULL &&
                       strcmp((const char *)later_value, (const char *)value) == 0;
      xmlFree(later_value);
      if (duplicate) {
        hol_error_set(error, HOL_ERR_SCHEMA,
                      "duplicate Common Cartridge resource identifier: %s", value);
        xmlFree(value);
        return -1;
      }
    }
    bool matches = strcmp((const char *)value, identifier) == 0;
    xmlFree(value);
    if (matches) *found = node;
  }
  if (*found == NULL) {
    hol_error_set(error, HOL_ERR_SCHEMA,
                  "missing Common Cartridge resource: %s", identifier);
    return -1;
  }
  return 0;
}

static int write_reader_text(const hol_course *course, const char *id,
                             const char *text, char relative[HOL_PATH_MAX + 1],
                             hol_error *error) {
  int length = snprintf(relative, HOL_PATH_MAX + 1U, ".reader/%s.txt", id);
  if (length < 0 || length > HOL_PATH_MAX) return -1;
  char path[4096];
  if (hol_join_path(path, sizeof(path), course->root, relative, error) < 0) return -1;
  return hol_atomic_write(path, text, strlen(text), error);
}

static int parse_web_content(hol_course *course, hol_lesson *lesson,
                             xmlNode *resource, hol_error *error) {
  char href[HOL_PATH_MAX + 1];
  if (attribute(resource, "href", href, sizeof(href), error) < 0 ||
      !hol_safe_relative_path(href)) return -1;
  char path[4096];
  if (hol_join_path(path, sizeof(path), course->root, href, error) < 0) return -1;
  xmlNode *file = NULL;
  if (resource_file_by_path(resource, href, &file, NULL, error) < 0 ||
      !bounded_regular_file(path, 16U * 1024U * 1024U))
    return -1;
  xmlDoc *document = htmlReadFile(path, NULL, HTML_PARSE_NONET | HTML_PARSE_NOERROR |
                                              HTML_PARSE_NOWARNING);
  if (document == NULL) return -1;
  xmlChar *content = xmlNodeGetContent(xmlDocGetRootElement(document));
  if (content == NULL) {
    xmlFreeDoc(document);
    return -1;
  }
  lesson->kind = HOL_LESSON_READING;
  int result = write_reader_text(course, lesson->id, (const char *)content,
                                 lesson->content_path, error);
  xmlFree(content);
  xmlFreeDoc(document);
  return result;
}

static int parse_qti_item(xmlNode *item, hol_quiz_question *question,
                          hol_error *error) {
  xmlNode *metadata = descendant(item, "qtimetadata");
  bool profile_found = false;
  for (xmlNode *field = metadata != NULL ? metadata->children : NULL;
       field != NULL; field = field->next) {
    if (field->type != XML_ELEMENT_NODE ||
        strcmp((const char *)field->name, "qtimetadatafield") != 0) continue;
    char label[64];
    char entry[64];
    if (node_text(child(field, "fieldlabel"), label, sizeof(label), "profile", error) == 0 &&
        node_text(child(field, "fieldentry"), entry, sizeof(entry), "profile", error) == 0 &&
        strcmp(label, "cc_profile") == 0 &&
        strcmp(entry, "cc.multiple_choice.v0p1") == 0) profile_found = true;
  }
  if (attribute(item, "ident", question->id, sizeof(question->id), error) < 0 ||
      !hol_valid_id(question->id) || !profile_found ||
      node_text(descendant(descendant(item, "presentation"), "mattext"),
                question->prompt, sizeof(question->prompt), "prompt", error) < 0)
    return -1;
  xmlNode *render = descendant(item, "render_choice");
  xmlNode *response = descendant(item, "response_lid");
  char cardinality[16];
  if (attribute(response, "rcardinality", cardinality, sizeof(cardinality), error) < 0 ||
      strcmp(cardinality, "Single") != 0) return -1;
  size_t count = 0U;
  for (xmlNode *node = render != NULL ? render->children : NULL;
       node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE &&
        strcmp((const char *)node->name, "response_label") == 0) count++;
  if (count < 2U || count > 10U) return -1;
  question->choices = calloc(count, sizeof(*question->choices));
  if (question->choices == NULL) return -1;
  question->choice_count = count;
  size_t index = 0U;
  for (xmlNode *node = render->children; node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE ||
        strcmp((const char *)node->name, "response_label") != 0) continue;
    if (attribute(node, "ident", question->choices[index].id,
                  sizeof(question->choices[index].id), error) < 0 ||
        node_text(descendant(node, "mattext"), question->choices[index].text,
                  sizeof(question->choices[index].text), "choice", error) < 0)
      return -1;
    for (size_t previous = 0U; previous < index; previous++)
      if (strcmp(question->choices[previous].id, question->choices[index].id) == 0)
        return -1;
    index++;
  }
  if (node_text(descendant(item, "varequal"), question->answer,
                sizeof(question->answer), "answer", error) < 0) return -1;
  bool answer_found = false;
  for (size_t choice = 0U; choice < question->choice_count; choice++)
    if (strcmp(question->choices[choice].id, question->answer) == 0) answer_found = true;
  if (!answer_found) return -1;
  xmlNode *feedback = descendant(item, "itemfeedback");
  if (feedback != NULL)
    (void)node_text(descendant(feedback, "mattext"), question->explanation,
                    sizeof(question->explanation), "feedback", error);
  return 0;
}

static size_t find_qti_passing_score(xmlNode *node, size_t maximum) {
  for (xmlNode *current = node != NULL ? node->children : NULL;
       current != NULL; current = current->next) {
    if (current->type != XML_ELEMENT_NODE) continue;
    if (strcmp((const char *)current->name, "qtimetadatafield") == 0) {
      xmlChar *label = xmlNodeGetContent(child(current, "fieldlabel"));
      xmlChar *entry = xmlNodeGetContent(child(current, "fieldentry"));
      if (label != NULL && entry != NULL &&
          strcmp((const char *)label, "qmd_masteryscore") == 0) {
        char *end = NULL;
        unsigned long value = strtoul((const char *)entry, &end, 10);
        xmlFree(label);
        xmlFree(entry);
        if (end != NULL && *end == '\0' && value >= 1UL && value <= maximum)
          return (size_t)value;
        return 0U;
      }
      xmlFree(label);
      xmlFree(entry);
    }
    size_t nested = find_qti_passing_score(current, maximum);
    if (nested > 0U) return nested;
  }
  return 0U;
}

static int parse_qti(hol_course *course, hol_lesson *lesson, xmlNode *resource,
                     hol_error *error) {
  char href[HOL_PATH_MAX + 1];
  xmlNode *file = NULL;
  size_t file_count = 0U;
  if (resource_file_by_path(resource, NULL, &file, &file_count, error) < 0 ||
      file_count != 1U || attribute(file, "href", href, sizeof(href), error) < 0 ||
      !hol_safe_relative_path(href)) return -1;
  char path[4096];
  if (hol_join_path(path, sizeof(path), course->root, href, error) < 0) return -1;
  if (!bounded_regular_file(path, 4U * 1024U * 1024U)) return -1;
  xmlDoc *document = xmlReadFile(path, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
  if (document == NULL) return -1;
  xmlNode *root = xmlDocGetRootElement(document);
  if (root == NULL || root->ns == NULL ||
      strcmp((const char *)root->ns->href, qti_namespace) != 0 ||
      strcmp((const char *)root->name, "questestinterop") != 0) goto failure;
  xmlNode *assessment = child(root, "assessment");
  bool assessment_profile = false;
  xmlNode *assessment_metadata = child(assessment, "qtimetadata");
  for (xmlNode *field = assessment_metadata != NULL ? assessment_metadata->children : NULL;
       field != NULL; field = field->next) {
    if (field->type != XML_ELEMENT_NODE ||
        strcmp((const char *)field->name, "qtimetadatafield") != 0) continue;
    char label[64];
    char entry[64];
    if (node_text(child(field, "fieldlabel"), label, sizeof(label), "profile", error) == 0 &&
        node_text(child(field, "fieldentry"), entry, sizeof(entry), "profile", error) == 0 &&
        strcmp(label, "cc_profile") == 0 && strcmp(entry, "cc.exam.v0p1") == 0)
      assessment_profile = true;
  }
  xmlNode *section = child(assessment, "section");
  size_t count = 0U;
  for (xmlNode *node = section != NULL ? section->children : NULL;
       node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE && strcmp((const char *)node->name, "item") == 0)
      count++;
  if (!assessment_profile || count == 0U || count > 100U) goto failure;
  lesson->questions = calloc(count, sizeof(*lesson->questions));
  if (lesson->questions == NULL) goto failure;
  lesson->question_count = count;
  size_t passing_score = find_qti_passing_score(assessment, count);
  lesson->quiz_passing_score = passing_score > 0U ? passing_score : count;
  size_t index = 0U;
  for (xmlNode *node = section->children; node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE || strcmp((const char *)node->name, "item") != 0)
      continue;
    if (parse_qti_item(node, &lesson->questions[index++], error) < 0) goto failure;
  }
  lesson->kind = HOL_LESSON_QUIZ;
  char summary[HOL_TEXT_MAX + 64U];
  (void)snprintf(summary, sizeof(summary), "%s\n\nThis assessment contains %zu questions.\n",
                 lesson->title, count);
  if (write_reader_text(course, lesson->id, summary, lesson->content_path, error) < 0)
    goto failure;
  xmlFreeDoc(document);
  return 0;

failure:
  xmlFreeDoc(document);
  return -1;
}

static int parse_lesson(hol_course *course, hol_lesson *lesson, xmlNode *item,
                        xmlNode *resources, hol_error *error) {
  char reference[HOL_ID_MAX + 1];
  if (attribute(item, "identifier", lesson->id, sizeof(lesson->id), error) < 0 ||
      !hol_valid_id(lesson->id) ||
      attribute(item, "identifierref", reference, sizeof(reference), error) < 0 ||
      node_text(child(item, "title"), lesson->title, sizeof(lesson->title),
                "lesson title", error) < 0) return -1;
  xmlNode *resource = NULL;
  char type[128];
  if (resource_by_id(resources, reference, &resource, error) < 0 ||
      attribute(resource, "type", type, sizeof(type), error) < 0)
    return -1;
  if (strcmp(type, "webcontent") == 0)
    return parse_web_content(course, lesson, resource, error);
  if (strcmp(type, qti_assessment_type) == 0)
    return parse_qti(course, lesson, resource, error);
  hol_error_set(error, HOL_ERR_UNSUPPORTED, "unsupported Common Cartridge resource: %s", type);
  return -1;
}

static int parse_organization(hol_course *course, xmlNode *organizations,
                              xmlNode *resources, hol_error *error) {
  xmlNode *organization = child(organizations, "organization");
  xmlNode *root_item = child(organization, "item");
  char structure[32];
  if (organization == NULL || root_item == NULL ||
      attribute(organization, "structure", structure, sizeof(structure), error) < 0 ||
      strcmp(structure, "rooted-hierarchy") != 0) return -1;
  size_t chapter_count = 0U;
  for (xmlNode *node = root_item->children; node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE && strcmp((const char *)node->name, "item") == 0)
      chapter_count++;
  if (chapter_count == 0U || chapter_count > 256U) return -1;
  course->chapters = calloc(chapter_count, sizeof(*course->chapters));
  if (course->chapters == NULL) return -1;
  course->chapter_count = chapter_count;
  size_t chapter_index = 0U;
  for (xmlNode *node = root_item->children; node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE || strcmp((const char *)node->name, "item") != 0)
      continue;
    hol_chapter *chapter = &course->chapters[chapter_index++];
    if (attribute(node, "identifier", chapter->id, sizeof(chapter->id), error) < 0 ||
        !hol_valid_id(chapter->id) ||
        node_text(child(node, "title"), chapter->title, sizeof(chapter->title),
                  "chapter title", error) < 0) return -1;
    size_t lesson_count = 0U;
    for (xmlNode *lesson = node->children; lesson != NULL; lesson = lesson->next)
      if (lesson->type == XML_ELEMENT_NODE &&
          strcmp((const char *)lesson->name, "item") == 0) lesson_count++;
    if (lesson_count == 0U || course->lesson_count > 10000U - lesson_count) return -1;
    chapter->lessons = calloc(lesson_count, sizeof(*chapter->lessons));
    if (chapter->lessons == NULL) return -1;
    chapter->lesson_count = lesson_count;
    size_t lesson_index = 0U;
    for (xmlNode *lesson = node->children; lesson != NULL; lesson = lesson->next) {
      if (lesson->type != XML_ELEMENT_NODE ||
          strcmp((const char *)lesson->name, "item") != 0) continue;
      if (parse_lesson(course, &chapter->lessons[lesson_index++], lesson,
                       resources, error) < 0) return -1;
    }
    course->lesson_count += lesson_count;
  }
  return 0;
}

static int course_load(const char *source, const char *profile_path,
                       hol_course **output, hol_error *error) {
  if (source == NULL || output == NULL) return -1;
  *output = NULL;
  hol_course *course = calloc(1U, sizeof(*course));
  if (course == NULL) return -1;
  if (realpath(source, course->source_path) == NULL ||
      extract_cartridge(source, course->root, error) < 0) goto failure;
  course->owns_root = true;
  char manifest_path[4096];
  if (hol_join_path(manifest_path, sizeof(manifest_path), course->root,
                    "imsmanifest.xml", error) < 0) goto failure;
  if (!bounded_regular_file(manifest_path, 4U * 1024U * 1024U)) goto failure;
  xmlDoc *document = xmlReadFile(manifest_path, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
  if (document == NULL) goto failure;
  xmlNode *manifest = xmlDocGetRootElement(document);
  if (manifest == NULL || manifest->ns == NULL ||
      strcmp((const char *)manifest->name, "manifest") != 0 ||
      strcmp((const char *)manifest->ns->href, manifest_namespace) != 0) goto xml_failure;
  xmlNode *metadata = child(manifest, "metadata");
  char schema[64];
  char schema_version[32];
  if (node_text(child(metadata, "schema"), schema, sizeof(schema), "schema", error) < 0 ||
      node_text(child(metadata, "schemaversion"), schema_version,
                sizeof(schema_version), "schema version", error) < 0 ||
      strcmp(schema, "IMS Common Cartridge") != 0 ||
      strcmp(schema_version, "1.3.0") != 0) goto xml_failure;
  xmlNode *lom = child_namespace(metadata, "lom", lom_manifest_namespace);
  xmlNode *general = child(lom, "general");
  xmlNode *lifecycle = child(lom, "lifeCycle");
  xmlNode *rights = child(lom, "rights");
  if (node_text(child(child(general, "identifier"), "entry"), course->id,
                sizeof(course->id), "course id", error) < 0 ||
      !hol_valid_id(course->id) ||
      node_text(child(child(general, "title"), "string"), course->title,
                sizeof(course->title), "title", error) < 0 ||
      node_text(child(child(general, "description"), "string"), course->description,
                sizeof(course->description), "description", error) < 0 ||
      node_text(child(child(lifecycle, "version"), "string"), course->version,
                sizeof(course->version), "version", error) < 0 ||
      node_text(child(child(rights, "description"), "string"), course->attribution,
                sizeof(course->attribution), "rights", error) < 0) goto xml_failure;
  course->schema_version = 1U;
  (void)snprintf(course->minimum_app_version, sizeof(course->minimum_app_version), "%s",
                 HOL_APP_VERSION);
  const char *spdx = strstr(course->attribution, "SPDX-License-Identifier:");
  if (spdx != NULL) {
    spdx += strlen("SPDX-License-Identifier:");
    while (*spdx == ' ') spdx++;
    size_t length = strcspn(spdx, "\r\n");
    if (length >= sizeof(course->license_spdx)) goto xml_failure;
    memcpy(course->license_spdx, spdx, length);
    course->license_spdx[length] = '\0';
  } else (void)snprintf(course->license_spdx, sizeof(course->license_spdx), "NOASSERTION");
  (void)snprintf(course->license_file, sizeof(course->license_file), "LICENSE");
  if (parse_organization(course, child(manifest, "organizations"),
                         child(manifest, "resources"), error) < 0) goto xml_failure;
  if (load_exercise_profile(course, child(manifest, "organizations"),
                            child(manifest, "resources"), profile_path, error) < 0)
    goto xml_failure;
  xmlFreeDoc(document);
  *output = course;
  return 0;

xml_failure:
  xmlFreeDoc(document);
failure:
  hol_course_free(course);
  if (error != NULL && error->message[0] == '\0')
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid Common Cartridge package");
  return -1;
}

int hol_course_load(const char *source, hol_course **output, hol_error *error) {
  return course_load(source, NULL, output, error);
}

int hol_course_load_profile(const char *source, const char *profile_path,
                            hol_course **output, hol_error *error) {
  if (profile_path == NULL) {
    hol_error_set(error, HOL_ERR_ARGUMENT, "exercise profile path is required");
    return -1;
  }
  return course_load(source, profile_path, output, error);
}

const hol_lesson *hol_course_lesson(const hol_course *course, size_t index) {
  if (course == NULL) return NULL;
  for (size_t chapter = 0U; chapter < course->chapter_count; chapter++) {
    if (index < course->chapters[chapter].lesson_count)
      return &course->chapters[chapter].lessons[index];
    index -= course->chapters[chapter].lesson_count;
  }
  return NULL;
}

void hol_course_free(hol_course *course) {
  if (course == NULL) return;
  for (size_t chapter = 0U; chapter < course->chapter_count; chapter++) {
    for (size_t lesson_index = 0U;
         lesson_index < course->chapters[chapter].lesson_count; lesson_index++) {
      hol_lesson *lesson = &course->chapters[chapter].lessons[lesson_index];
      free(lesson->files);
      free(lesson->runner.expected_output);
      for (size_t question = 0U; question < lesson->question_count; question++)
        free(lesson->questions[question].choices);
      free(lesson->questions);
      for (size_t media = 0U; media < lesson->media_count; media++)
        free(lesson->media_paths[media]);
      free(lesson->media_paths);
    }
    free(course->chapters[chapter].lessons);
  }
  free(course->chapters);
  if (course->owns_root) (void)remove_tree(course->root);
  free(course);
}
