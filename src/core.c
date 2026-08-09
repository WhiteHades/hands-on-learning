#include "hol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

void hol_error_set(hol_error *error, hol_errc code, const char *format, ...) {
  if (error == NULL) return;
  error->code = code;
  error->system_errno = errno;
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
  va_end(arguments);
}

bool hol_safe_relative_path(const char *path) {
  if (path == NULL || path[0] == '\0' || path[0] == '/' ||
      strlen(path) > HOL_PATH_MAX) {
    return false;
  }
  const char *segment = path;
  for (const char *cursor = path;; cursor++) {
    unsigned char byte = (unsigned char)*cursor;
    if (byte == '\\' || (byte != 0U && byte < 0x20U) || byte == 0x7fU)
      return false;
    if (byte == '/' || byte == '\0') {
      size_t length = (size_t)(cursor - segment);
      if (length == 0U || (length == 1U && segment[0] == '.') ||
          (length == 2U && segment[0] == '.' && segment[1] == '.')) {
        return false;
      }
      if (byte == '\0') break;
      segment = cursor + 1;
    }
  }
  return true;
}

int hol_join_path(char *output, size_t size, const char *root,
                  const char *relative, hol_error *error) {
  if (!hol_safe_relative_path(relative)) {
    hol_error_set(error, HOL_ERR_PATH, "unsafe relative path: %s",
                  relative != NULL ? relative : "(null)");
    return -1;
  }
  int written = snprintf(output, size, "%s/%s", root, relative);
  if (written < 0 || (size_t)written >= size) {
    hol_error_set(error, HOL_ERR_PATH, "joined path is too long");
    return -1;
  }
  return 0;
}

static int make_parent_directories(const char *path, hol_error *error) {
  char copy[4096];
  if (copy_string(copy, sizeof(copy), path, "path", error) < 0) return -1;
  for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(copy, 0700) < 0 && errno != EEXIST) {
      hol_error_set(error, HOL_ERR_IO, "cannot create directory %s", copy);
      return -1;
    }
    *cursor = '/';
  }
  return 0;
}

int hol_atomic_write(const char *path, const char *data, size_t length,
                     hol_error *error) {
  if (make_parent_directories(path, error) < 0) return -1;
  char temporary[4096];
  int written = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
  if (written < 0 || (size_t)written >= sizeof(temporary)) {
    hol_error_set(error, HOL_ERR_PATH, "temporary path is too long");
    return -1;
  }
  int descriptor = mkstemp(temporary);
  if (descriptor < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot create temporary state file");
    return -1;
  }
  size_t offset = 0U;
  while (offset < length) {
    ssize_t count = write(descriptor, data + offset, length - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      hol_error_set(error, HOL_ERR_IO, "cannot write temporary state file");
      (void)close(descriptor);
      (void)unlink(temporary);
      return -1;
    }
    offset += (size_t)count;
  }
  if (fsync(descriptor) < 0 || close(descriptor) < 0 ||
      rename(temporary, path) < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot publish atomic state file");
    (void)unlink(temporary);
    return -1;
  }
  return 0;
}

char *hol_read_text(const char *path, size_t maximum, size_t *length,
                    hol_error *error) {
  int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot open %s", path);
    return NULL;
  }
  struct stat status;
  if (fstat(descriptor, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 || (uintmax_t)status.st_size > maximum) {
    hol_error_set(error, HOL_ERR_IO, "invalid or oversized file: %s", path);
    (void)close(descriptor);
    return NULL;
  }
  size_t size = (size_t)status.st_size;
  char *data = calloc(size + 1U, 1U);
  if (data == NULL) {
    hol_error_set(error, HOL_ERR_IO, "out of memory reading %s", path);
    (void)close(descriptor);
    return NULL;
  }
  size_t offset = 0U;
  while (offset < size) {
    ssize_t count = read(descriptor, data + offset, size - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      hol_error_set(error, HOL_ERR_IO, "short read from %s", path);
      free(data);
      (void)close(descriptor);
      return NULL;
    }
    offset += (size_t)count;
  }
  (void)close(descriptor);
  if (length != NULL) *length = size;
  return data;
}

int hol_copy_file_if_missing(const char *source, const char *target,
                             hol_error *error) {
  struct stat existing;
  if (lstat(target, &existing) == 0) {
    if (!S_ISREG(existing.st_mode)) {
      hol_error_set(error, HOL_ERR_PATH, "workspace target is not a file");
      return -1;
    }
    return 0;
  }
  if (errno != ENOENT || make_parent_directories(target, error) < 0) return -1;
  size_t length = 0U;
  char *content = hol_read_text(source, HOL_OUTPUT_MAX, &length, error);
  if (content == NULL) return -1;
  int descriptor = open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    if (errno == EEXIST) {
      free(content);
      return 0;
    }
    hol_error_set(error, HOL_ERR_IO, "cannot create workspace file");
    free(content);
    return -1;
  }
  size_t offset = 0U;
  while (offset < length) {
    ssize_t count = write(descriptor, content + offset, length - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      hol_error_set(error, HOL_ERR_IO, "cannot copy workspace file");
      (void)close(descriptor);
      (void)unlink(target);
      free(content);
      return -1;
    }
    offset += (size_t)count;
  }
  free(content);
  if (close(descriptor) < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot close workspace file");
    return -1;
  }
  return 0;
}

static bool valid_id(const char *id) {
  size_t length = strlen(id);
  if (length == 0U || length > HOL_ID_MAX) return false;
  for (size_t index = 0U; index < length; index++) {
    char value = id[index];
    bool allowed = (value >= 'a' && value <= 'z') ||
                   (value >= '0' && value <= '9') || value == '.' ||
                   value == '_' || value == '-';
    if (!allowed || (index == 0U && (value == '.' || value == '_' || value == '-'))) {
      return false;
    }
  }
  return true;
}

static json_object *required(json_object *parent, const char *name,
                             enum json_type type, hol_error *error) {
  json_object *value = NULL;
  if (!json_object_object_get_ex(parent, name, &value) ||
      !json_object_is_type(value, type)) {
    hol_error_set(error, HOL_ERR_SCHEMA, "missing or invalid field: %s", name);
    return NULL;
  }
  return value;
}

static const char *required_string(json_object *parent, const char *name,
                                   hol_error *error) {
  json_object *value = required(parent, name, json_type_string, error);
  return value != NULL ? json_object_get_string(value) : NULL;
}

static int parse_file(json_object *object, hol_course_file *file,
                      hol_error *error) {
  const char *source = required_string(object, "source", error);
  const char *target = required_string(object, "target", error);
  const char *role = required_string(object, "role", error);
  const char *syntax = required_string(object, "syntax", error);
  if (source == NULL || target == NULL || role == NULL || syntax == NULL) return -1;
  if (!hol_safe_relative_path(source) || !hol_safe_relative_path(target)) {
    hol_error_set(error, HOL_ERR_PATH, "workspace file has an unsafe path");
    return -1;
  }
  if (strcmp(role, "editable") == 0) file->role = HOL_FILE_EDITABLE;
  else if (strcmp(role, "readonly") == 0) file->role = HOL_FILE_READONLY;
  else if (strcmp(role, "hidden") == 0) file->role = HOL_FILE_HIDDEN;
  else {
    hol_error_set(error, HOL_ERR_SCHEMA, "unknown workspace role: %s", role);
    return -1;
  }
  return copy_string(file->source, sizeof(file->source), source, "source", error) |
         copy_string(file->target, sizeof(file->target), target, "target", error) |
         copy_string(file->syntax, sizeof(file->syntax), syntax, "syntax", error);
}

static int parse_quiz(json_object *quiz, hol_lesson *lesson, hol_error *error) {
  json_object *questions = required(quiz, "questions", json_type_array, error);
  if (questions == NULL) return -1;
  size_t count = json_object_array_length(questions);
  if (count == 0U || count > 100U) {
    hol_error_set(error, HOL_ERR_SCHEMA, "quiz question count is invalid");
    return -1;
  }
  lesson->questions = calloc(count, sizeof(*lesson->questions));
  if (lesson->questions == NULL) return -1;
  lesson->question_count = count;
  for (size_t index = 0U; index < count; index++) {
    json_object *question = json_object_array_get_idx(questions, index);
    const char *id = required_string(question, "id", error);
    const char *prompt = required_string(question, "prompt", error);
    const char *answer = required_string(question, "answer", error);
    json_object *choices = required(question, "choices", json_type_array, error);
    if (id == NULL || prompt == NULL || answer == NULL || choices == NULL ||
        !valid_id(id)) return -1;
    hol_quiz_question *target = &lesson->questions[index];
    if (copy_string(target->id, sizeof(target->id), id, "question id", error) < 0 ||
        copy_string(target->prompt, sizeof(target->prompt), prompt, "prompt", error) < 0 ||
        copy_string(target->answer, sizeof(target->answer), answer, "answer", error) < 0) {
      return -1;
    }
    json_object *explanation = NULL;
    if (json_object_object_get_ex(question, "explanation", &explanation) &&
        json_object_is_type(explanation, json_type_string) &&
        copy_string(target->explanation, sizeof(target->explanation),
                    json_object_get_string(explanation), "explanation", error) < 0) {
      return -1;
    }
    size_t choice_count = json_object_array_length(choices);
    if (choice_count < 2U || choice_count > 10U) return -1;
    target->choices = calloc(choice_count, sizeof(*target->choices));
    if (target->choices == NULL) return -1;
    target->choice_count = choice_count;
    bool answer_found = false;
    for (size_t choice_index = 0U; choice_index < choice_count; choice_index++) {
      json_object *choice = json_object_array_get_idx(choices, choice_index);
      const char *choice_id = required_string(choice, "id", error);
      const char *text = required_string(choice, "text", error);
      if (choice_id == NULL || text == NULL || !valid_id(choice_id)) return -1;
      if (strcmp(choice_id, answer) == 0) answer_found = true;
      if (copy_string(target->choices[choice_index].id,
                      sizeof(target->choices[choice_index].id), choice_id,
                      "choice id", error) < 0 ||
          copy_string(target->choices[choice_index].text,
                      sizeof(target->choices[choice_index].text), text,
                      "choice text", error) < 0) return -1;
    }
    if (!answer_found) {
      hol_error_set(error, HOL_ERR_SCHEMA, "quiz answer does not name a choice");
      return -1;
    }
  }
  return 0;
}

static int validate_bundle_file(const hol_course *course, const char *relative,
                                hol_error *error) {
  char path[4096];
  if (hol_join_path(path, sizeof(path), course->root, relative, error) < 0) return -1;
  struct stat status;
  if (lstat(path, &status) < 0 || !S_ISREG(status.st_mode)) {
    hol_error_set(error, HOL_ERR_PATH, "bundle file is missing or unsafe: %s", relative);
    return -1;
  }
  return 0;
}

static int parse_lesson(json_object *object, hol_course *course,
                        hol_lesson *lesson, hol_error *error) {
  const char *id = required_string(object, "id", error);
  const char *title = required_string(object, "title", error);
  const char *kind = required_string(object, "kind", error);
  const char *content = required_string(object, "content", error);
  if (id == NULL || title == NULL || kind == NULL || content == NULL || !valid_id(id) ||
      !hol_safe_relative_path(content)) return -1;
  if (strcmp(kind, "reading") == 0) lesson->kind = HOL_LESSON_READING;
  else if (strcmp(kind, "exercise") == 0) lesson->kind = HOL_LESSON_EXERCISE;
  else if (strcmp(kind, "quiz") == 0) lesson->kind = HOL_LESSON_QUIZ;
  else return -1;
  if (copy_string(lesson->id, sizeof(lesson->id), id, "lesson id", error) < 0 ||
      copy_string(lesson->title, sizeof(lesson->title), title, "lesson title", error) < 0 ||
      copy_string(lesson->content_path, sizeof(lesson->content_path), content,
                  "lesson content", error) < 0 ||
      validate_bundle_file(course, content, error) < 0) return -1;

  json_object *workspace = NULL;
  if (json_object_object_get_ex(object, "workspace", &workspace) &&
      !json_object_is_type(workspace, json_type_null)) {
    const char *default_file = required_string(workspace, "default_file", error);
    json_object *files = required(workspace, "files", json_type_array, error);
    if (default_file == NULL || files == NULL || !hol_safe_relative_path(default_file)) return -1;
    size_t file_count = json_object_array_length(files);
    if (file_count == 0U || file_count > 256U) return -1;
    lesson->files = calloc(file_count, sizeof(*lesson->files));
    if (lesson->files == NULL) return -1;
    lesson->file_count = file_count;
    if (copy_string(lesson->default_file, sizeof(lesson->default_file), default_file,
                    "default file", error) < 0) return -1;
    bool default_found = false;
    for (size_t index = 0U; index < file_count; index++) {
      if (parse_file(json_object_array_get_idx(files, index), &lesson->files[index],
                     error) < 0 ||
          validate_bundle_file(course, lesson->files[index].source, error) < 0) return -1;
      if (strcmp(lesson->files[index].target, default_file) == 0 &&
          lesson->files[index].role != HOL_FILE_HIDDEN) default_found = true;
    }
    if (!default_found) return -1;
  }

  json_object *runner = NULL;
  if (json_object_object_get_ex(object, "runner", &runner) &&
      !json_object_is_type(runner, json_type_null)) {
    const char *runner_id = required_string(runner, "id", error);
    const char *profile = required_string(runner, "profile", error);
    json_object *check = required(runner, "check", json_type_object, error);
    const char *check_kind = check != NULL ? required_string(check, "kind", error) : NULL;
    if (runner_id == NULL || profile == NULL || check_kind == NULL ||
        strcmp(runner_id, "c") != 0 ||
        (strcmp(profile, "c11") != 0 && strcmp(profile, "c11-32") != 0 &&
         strcmp(profile, "c23") != 0)) {
      hol_error_set(error, HOL_ERR_UNSUPPORTED, "unsupported runner profile");
      return -1;
    }
    (void)copy_string(lesson->runner.id, sizeof(lesson->runner.id), runner_id,
                      "runner", error);
    (void)copy_string(lesson->runner.profile, sizeof(lesson->runner.profile), profile,
                      "runner profile", error);
    if (strcmp(check_kind, "stdout") == 0) {
      const char *expected = required_string(check, "expected", error);
      if (expected == NULL || strlen(expected) > HOL_OUTPUT_MAX) return -1;
      lesson->runner.check_kind = HOL_CHECK_STDOUT;
      lesson->runner.expected_output = strdup(expected);
      if (lesson->runner.expected_output == NULL) return -1;
    } else if (strcmp(check_kind, "tests") == 0) {
      lesson->runner.check_kind = HOL_CHECK_TESTS;
    } else return -1;
  }

  json_object *quiz = NULL;
  if (json_object_object_get_ex(object, "quiz", &quiz) &&
      !json_object_is_type(quiz, json_type_null) && parse_quiz(quiz, lesson, error) < 0) {
    return -1;
  }
  if ((lesson->kind == HOL_LESSON_EXERCISE && lesson->file_count == 0U) ||
      (lesson->kind == HOL_LESSON_QUIZ && lesson->question_count == 0U)) return -1;
  return 0;
}

int hol_course_load(const char *root, hol_course **output, hol_error *error) {
  if (root == NULL || output == NULL) return -1;
  *output = NULL;
  char canonical[4096];
  if (realpath(root, canonical) == NULL) {
    hol_error_set(error, HOL_ERR_PATH, "cannot resolve course root");
    return -1;
  }
  char manifest_path[4096];
  int written = snprintf(manifest_path, sizeof(manifest_path), "%s/course.json", canonical);
  if (written < 0 || (size_t)written >= sizeof(manifest_path)) return -1;
  size_t manifest_length = 0U;
  char *manifest = hol_read_text(manifest_path, 1024U * 1024U, &manifest_length, error);
  if (manifest == NULL) return -1;
  json_tokener *tokener = json_tokener_new_ex(32);
  json_object *root_object = json_tokener_parse_ex(tokener, manifest, (int)manifest_length);
  enum json_tokener_error json_error = json_tokener_get_error(tokener);
  free(manifest);
  json_tokener_free(tokener);
  if (root_object == NULL || json_error != json_tokener_success ||
      !json_object_is_type(root_object, json_type_object)) {
    if (root_object != NULL) json_object_put(root_object);
    hol_error_set(error, HOL_ERR_JSON, "course.json is not valid JSON");
    return -1;
  }
  hol_course *course = calloc(1U, sizeof(*course));
  if (course == NULL) {
    json_object_put(root_object);
    return -1;
  }
  (void)copy_string(course->root, sizeof(course->root), canonical, "course root", error);
  json_object *schema = required(root_object, "schema_version", json_type_int, error);
  const char *id = required_string(root_object, "id", error);
  const char *version = required_string(root_object, "version", error);
  const char *minimum = required_string(root_object, "minimum_app_version", error);
  const char *title = required_string(root_object, "title", error);
  const char *description = required_string(root_object, "description", error);
  json_object *license = required(root_object, "license", json_type_object, error);
  json_object *chapters = required(root_object, "chapters", json_type_array, error);
  if (schema == NULL || id == NULL || version == NULL || minimum == NULL || title == NULL ||
      description == NULL || license == NULL || chapters == NULL ||
      json_object_get_int(schema) != 1 || !valid_id(id)) goto failure;
  const char *spdx = required_string(license, "spdx", error);
  const char *license_file = required_string(license, "file", error);
  const char *attribution = required_string(license, "attribution", error);
  if (spdx == NULL || license_file == NULL || attribution == NULL ||
      !hol_safe_relative_path(license_file)) goto failure;
  course->schema_version = 1U;
  if (copy_string(course->id, sizeof(course->id), id, "course id", error) < 0 ||
      copy_string(course->version, sizeof(course->version), version, "version", error) < 0 ||
      copy_string(course->minimum_app_version, sizeof(course->minimum_app_version), minimum,
                  "minimum app version", error) < 0 ||
      copy_string(course->title, sizeof(course->title), title, "title", error) < 0 ||
      copy_string(course->description, sizeof(course->description), description,
                  "description", error) < 0 ||
      copy_string(course->license_spdx, sizeof(course->license_spdx), spdx, "license", error) < 0 ||
      copy_string(course->license_file, sizeof(course->license_file), license_file,
                  "license file", error) < 0 ||
      copy_string(course->attribution, sizeof(course->attribution), attribution,
                  "attribution", error) < 0 ||
      validate_bundle_file(course, license_file, error) < 0) goto failure;
  size_t chapter_count = json_object_array_length(chapters);
  if (chapter_count == 0U || chapter_count > 256U) goto failure;
  course->chapters = calloc(chapter_count, sizeof(*course->chapters));
  if (course->chapters == NULL) goto failure;
  course->chapter_count = chapter_count;
  for (size_t chapter_index = 0U; chapter_index < chapter_count; chapter_index++) {
    json_object *chapter = json_object_array_get_idx(chapters, chapter_index);
    const char *chapter_id = required_string(chapter, "id", error);
    const char *chapter_title = required_string(chapter, "title", error);
    json_object *lessons = required(chapter, "lessons", json_type_array, error);
    if (chapter_id == NULL || chapter_title == NULL || lessons == NULL ||
        !valid_id(chapter_id)) goto failure;
    hol_chapter *target = &course->chapters[chapter_index];
    if (copy_string(target->id, sizeof(target->id), chapter_id, "chapter id", error) < 0 ||
        copy_string(target->title, sizeof(target->title), chapter_title,
                    "chapter title", error) < 0) goto failure;
    size_t lesson_count = json_object_array_length(lessons);
    if (lesson_count == 0U || lesson_count > 10000U) goto failure;
    target->lessons = calloc(lesson_count, sizeof(*target->lessons));
    if (target->lessons == NULL) goto failure;
    target->lesson_count = lesson_count;
    course->lesson_count += lesson_count;
    for (size_t lesson_index = 0U; lesson_index < lesson_count; lesson_index++) {
      if (parse_lesson(json_object_array_get_idx(lessons, lesson_index), course,
                       &target->lessons[lesson_index], error) < 0) goto failure;
    }
  }
  json_object_put(root_object);
  *output = course;
  return 0;

failure:
  json_object_put(root_object);
  hol_course_free(course);
  if (error != NULL && error->message[0] == '\0')
    hol_error_set(error, HOL_ERR_SCHEMA, "course manifest is invalid");
  return -1;
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
  free(course);
}

int hol_state_load(const char *path, hol_state *state, hol_error *error) {
  memset(state, 0, sizeof(*state));
  size_t length = 0U;
  char *text = hol_read_text(path, 64U * 1024U, &length, error);
  if (text == NULL) {
    if (errno == ENOENT) {
      if (error != NULL) memset(error, 0, sizeof(*error));
      return 0;
    }
    return -1;
  }
  json_object *object = json_tokener_parse(text);
  free(text);
  if (object == NULL) return -1;
  json_object *value = NULL;
#define LOAD_STRING(NAME, FIELD)                                               \
  if (json_object_object_get_ex(object, NAME, &value) &&                       \
      json_object_is_type(value, json_type_string))                            \
    (void)copy_string(state->FIELD, sizeof(state->FIELD),                      \
                      json_object_get_string(value), NAME, error)
  LOAD_STRING("course_id", course_id);
  LOAD_STRING("lesson_id", lesson_id);
  LOAD_STRING("file_path", file_path);
#undef LOAD_STRING
  if (json_object_object_get_ex(object, "reader_scroll", &value))
    state->reader_scroll = (size_t)json_object_get_uint64(value);
  if (json_object_object_get_ex(object, "preview_scroll", &value))
    state->preview_scroll = (size_t)json_object_get_uint64(value);
  if (json_object_object_get_ex(object, "output_scroll", &value))
    state->output_scroll = (size_t)json_object_get_uint64(value);
  if (json_object_object_get_ex(object, "pane", &value))
    state->pane = json_object_get_int(value);
  json_object_put(object);
  return 0;
}

int hol_state_save(const char *path, const hol_state *state, hol_error *error) {
  json_object *object = json_object_new_object();
  json_object_object_add(object, "schema_version", json_object_new_int(1));
  json_object_object_add(object, "course_id", json_object_new_string(state->course_id));
  json_object_object_add(object, "lesson_id", json_object_new_string(state->lesson_id));
  json_object_object_add(object, "file_path", json_object_new_string(state->file_path));
  json_object_object_add(object, "reader_scroll",
                         json_object_new_uint64(state->reader_scroll));
  json_object_object_add(object, "preview_scroll",
                         json_object_new_uint64(state->preview_scroll));
  json_object_object_add(object, "output_scroll",
                         json_object_new_uint64(state->output_scroll));
  json_object_object_add(object, "pane", json_object_new_int(state->pane));
  const char *json = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PRETTY);
  int result = hol_atomic_write(path, json, strlen(json), error);
  json_object_put(object);
  return result;
}

int hol_workspace_ensure(const hol_course *course, const hol_lesson *lesson,
                         const char *workspace, hol_error *error) {
  if (mkdir(workspace, 0700) < 0 && errno != EEXIST) {
    hol_error_set(error, HOL_ERR_IO, "cannot create lesson workspace");
    return -1;
  }
  for (size_t index = 0U; index < lesson->file_count; index++) {
    char source[4096];
    char target[4096];
    if (hol_join_path(source, sizeof(source), course->root, lesson->files[index].source,
                      error) < 0 ||
        hol_join_path(target, sizeof(target), workspace, lesson->files[index].target,
                      error) < 0 ||
        hol_copy_file_if_missing(source, target, error) < 0) return -1;
  }
  return 0;
}

static int remove_tree(const char *path, hol_error *error) {
  DIR *directory = opendir(path);
  if (directory == NULL) {
    if (errno == ENOENT) return 0;
    hol_error_set(error, HOL_ERR_IO, "cannot open workspace for reset");
    return -1;
  }
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char child[4096];
    int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(child)) {
      (void)closedir(directory);
      return -1;
    }
    struct stat status;
    if (lstat(child, &status) < 0) {
      (void)closedir(directory);
      return -1;
    }
    if (S_ISDIR(status.st_mode)) {
      if (remove_tree(child, error) < 0) {
        (void)closedir(directory);
        return -1;
      }
    } else if (unlink(child) < 0) {
      (void)closedir(directory);
      return -1;
    }
  }
  (void)closedir(directory);
  if (rmdir(path) < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot remove lesson workspace");
    return -1;
  }
  return 0;
}

int hol_workspace_reset(const hol_course *course, const hol_lesson *lesson,
                        const char *workspace, hol_error *error) {
  if (remove_tree(workspace, error) < 0) return -1;
  return hol_workspace_ensure(course, lesson, workspace, error);
}
