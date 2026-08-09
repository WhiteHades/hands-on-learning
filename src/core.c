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
  if (value == NULL) return NULL;
  const char *text = json_object_get_string(value);
  if ((size_t)json_object_get_string_len(value) != strlen(text)) {
    hol_error_set(error, HOL_ERR_SCHEMA, "field contains an embedded NUL: %s", name);
    return NULL;
  }
  return text;
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
  json_object *passing_score = required(quiz, "passing_score", json_type_int, error);
  if (questions == NULL || passing_score == NULL) return -1;
  size_t count = json_object_array_length(questions);
  int64_t score = json_object_get_int64(passing_score);
  if (count == 0U || count > 100U || score < 1 || (uint64_t)score > count) {
    hol_error_set(error, HOL_ERR_SCHEMA, "quiz question count is invalid");
    return -1;
  }
  lesson->questions = calloc(count, sizeof(*lesson->questions));
  if (lesson->questions == NULL) return -1;
  lesson->question_count = count;
  lesson->quiz_passing_score = (size_t)score;
  for (size_t index = 0U; index < count; index++) {
    json_object *question = json_object_array_get_idx(questions, index);
    const char *id = required_string(question, "id", error);
    const char *prompt = required_string(question, "prompt", error);
    const char *answer = required_string(question, "answer", error);
    json_object *choices = required(question, "choices", json_type_array, error);
    if (id == NULL || prompt == NULL || answer == NULL || choices == NULL ||
        !hol_valid_id(id)) return -1;
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
      if (choice_id == NULL || text == NULL || !hol_valid_id(choice_id)) return -1;
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
  if (lstat(path, &status) < 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1) {
    hol_error_set(error, HOL_ERR_PATH, "bundle file is missing or unsafe: %s", relative);
    return -1;
  }
  return 0;
}

static bool valid_sha256(const char *value) {
  if (strlen(value) != 64U) return false;
  for (size_t index = 0U; index < 64U; index++)
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) return false;
  return true;
}

static json_object *inventory_entry(json_object *files, const char *path) {
  size_t count = json_object_array_length(files);
  for (size_t index = 0U; index < count; index++) {
    json_object *entry = json_object_array_get_idx(files, index);
    json_object *value = NULL;
    if (json_object_object_get_ex(entry, "path", &value) &&
        json_object_is_type(value, json_type_string) &&
        strcmp(json_object_get_string(value), path) == 0) return entry;
  }
  return NULL;
}

static int audit_directory(const hol_course *course, json_object *files,
                           const char *relative, size_t *count,
                           hol_error *error) {
  char directory_path[4096];
  if (relative[0] == '\0') {
    if (copy_string(directory_path, sizeof(directory_path), course->root,
                    "course root", error) < 0) return -1;
  } else if (hol_join_path(directory_path, sizeof(directory_path), course->root,
                           relative, error) < 0) return -1;
  DIR *directory = opendir(directory_path);
  if (directory == NULL) return -1;
  struct dirent *item;
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
    char child_relative[HOL_PATH_MAX + 1];
    int written = relative[0] == '\0'
      ? snprintf(child_relative, sizeof(child_relative), "%s", item->d_name)
      : snprintf(child_relative, sizeof(child_relative), "%s/%s", relative, item->d_name);
    if (written < 0 || (size_t)written >= sizeof(child_relative) ||
        !hol_safe_relative_path(child_relative)) {
      (void)closedir(directory);
      return -1;
    }
    char child_path[4096];
    if (hol_join_path(child_path, sizeof(child_path), course->root, child_relative,
                      error) < 0) {
      (void)closedir(directory);
      return -1;
    }
    struct stat status;
    if (lstat(child_path, &status) < 0) {
      (void)closedir(directory);
      return -1;
    }
    if (S_ISDIR(status.st_mode)) {
      if (audit_directory(course, files, child_relative, count, error) < 0) {
        (void)closedir(directory);
        return -1;
      }
    } else if (S_ISREG(status.st_mode) && status.st_nlink == 1) {
      if (strcmp(child_relative, "course.json") == 0) continue;
      if (inventory_entry(files, child_relative) == NULL) {
        hol_error_set(error, HOL_ERR_CHECKSUM, "undeclared bundle file: %s",
                      child_relative);
        (void)closedir(directory);
        return -1;
      }
      (*count)++;
    } else {
      hol_error_set(error, HOL_ERR_PATH, "bundle contains a link or special file");
      (void)closedir(directory);
      return -1;
    }
  }
  (void)closedir(directory);
  return 0;
}

static int validate_inventory(const hol_course *course, json_object *files,
                              hol_error *error) {
  size_t file_count = json_object_array_length(files);
  if (file_count == 0U || file_count > 100000U) return -1;
  for (size_t index = 0U; index < file_count; index++) {
    json_object *entry = json_object_array_get_idx(files, index);
    const char *path = required_string(entry, "path", error);
    const char *expected = required_string(entry, "sha256", error);
    json_object *bytes = required(entry, "bytes", json_type_int, error);
    if (path == NULL || expected == NULL || bytes == NULL ||
        !hol_safe_relative_path(path) || !valid_sha256(expected) ||
        json_object_get_int64(bytes) < 0) return -1;
    for (size_t previous = 0U; previous < index; previous++) {
      json_object *other = json_object_array_get_idx(files, previous);
      json_object *other_path = NULL;
      (void)json_object_object_get_ex(other, "path", &other_path);
      if (other_path != NULL && strcmp(json_object_get_string(other_path), path) == 0)
        return -1;
    }
    char absolute[4096];
    if (hol_join_path(absolute, sizeof(absolute), course->root, path, error) < 0) return -1;
    struct stat status;
    if (lstat(absolute, &status) < 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1 ||
        (uint64_t)status.st_size != json_object_get_uint64(bytes)) {
      hol_error_set(error, HOL_ERR_CHECKSUM, "bundle size mismatch: %s", path);
      return -1;
    }
    char actual[65];
    if (hol_sha256_file(absolute, actual, error) < 0 || strcmp(actual, expected) != 0) {
      hol_error_set(error, HOL_ERR_CHECKSUM, "bundle checksum mismatch: %s", path);
      return -1;
    }
  }
  size_t discovered = 0U;
  if (audit_directory(course, files, "", &discovered, error) < 0 ||
      discovered != file_count) return -1;
  return 0;
}

static int parse_lesson(json_object *object, hol_course *course,
                        hol_lesson *lesson, hol_error *error) {
  const char *id = required_string(object, "id", error);
  const char *title = required_string(object, "title", error);
  const char *kind = required_string(object, "kind", error);
  const char *content = required_string(object, "content", error);
  if (id == NULL || title == NULL || kind == NULL || content == NULL || !hol_valid_id(id) ||
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
    bool c_runner = runner_id != NULL && profile != NULL &&
      strcmp(runner_id, "c") == 0 &&
      (strcmp(profile, "c11") == 0 || strcmp(profile, "c11-32") == 0 ||
       strcmp(profile, "c23") == 0);
    bool sql_runner = runner_id != NULL && profile != NULL &&
      strcmp(runner_id, "sql") == 0 && strcmp(profile, "sqlite3") == 0;
    if (runner_id == NULL || profile == NULL || check_kind == NULL ||
        (!c_runner && !sql_runner)) {
      hol_error_set(error, HOL_ERR_UNSUPPORTED, "unsupported runner profile");
      return -1;
    }
    (void)copy_string(lesson->runner.id, sizeof(lesson->runner.id), runner_id,
                      "runner", error);
    (void)copy_string(lesson->runner.profile, sizeof(lesson->runner.profile), profile,
                      "runner profile", error);
    if (strcmp(check_kind, "stdout") == 0 && c_runner) {
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
  json_object *media = NULL;
  if (json_object_object_get_ex(object, "media", &media)) {
    if (!json_object_is_type(media, json_type_array) ||
        json_object_array_length(media) > 64U) return -1;
    lesson->media_count = json_object_array_length(media);
    if (lesson->media_count > 0U) {
      lesson->media_paths = calloc(lesson->media_count, sizeof(*lesson->media_paths));
      if (lesson->media_paths == NULL) return -1;
    }
    for (size_t index = 0U; index < lesson->media_count; index++) {
      json_object *item = json_object_array_get_idx(media, index);
      if (!json_object_is_type(item, json_type_string)) return -1;
      const char *media_path = json_object_get_string(item);
      if (!hol_safe_relative_path(media_path) ||
          validate_bundle_file(course, media_path, error) < 0) return -1;
      lesson->media_paths[index] = strdup(media_path);
      if (lesson->media_paths[index] == NULL) return -1;
    }
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
  json_object *files = required(root_object, "files", json_type_array, error);
  json_object *chapters = required(root_object, "chapters", json_type_array, error);
  if (schema == NULL || id == NULL || version == NULL || minimum == NULL || title == NULL ||
      description == NULL || license == NULL || files == NULL || chapters == NULL ||
      json_object_get_int(schema) != 1 || !hol_valid_id(id)) goto failure;
  if (!hol_version_supported(minimum)) {
    hol_error_set(error, HOL_ERR_UNSUPPORTED, "course requires a newer application version");
    goto failure;
  }
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
      validate_inventory(course, files, error) < 0 ||
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
        !hol_valid_id(chapter_id)) goto failure;
    hol_chapter *target = &course->chapters[chapter_index];
    if (copy_string(target->id, sizeof(target->id), chapter_id, "chapter id", error) < 0 ||
        copy_string(target->title, sizeof(target->title), chapter_title,
                    "chapter title", error) < 0) goto failure;
    size_t lesson_count = json_object_array_length(lessons);
    if (lesson_count == 0U || lesson_count > 10000U ||
        course->lesson_count > 10000U - lesson_count) goto failure;
    target->lessons = calloc(lesson_count, sizeof(*target->lessons));
    if (target->lessons == NULL) goto failure;
    target->lesson_count = lesson_count;
    course->lesson_count += lesson_count;
    for (size_t lesson_index = 0U; lesson_index < lesson_count; lesson_index++) {
      if (parse_lesson(json_object_array_get_idx(lessons, lesson_index), course,
                       &target->lessons[lesson_index], error) < 0) goto failure;
    }
  }
  for (size_t chapter = 0U; chapter < course->chapter_count; chapter++) {
    for (size_t previous = 0U; previous < chapter; previous++)
      if (strcmp(course->chapters[chapter].id, course->chapters[previous].id) == 0) {
        hol_error_set(error, HOL_ERR_SCHEMA, "duplicate chapter id");
        goto failure;
      }
    for (size_t lesson_index = 0U;
         lesson_index < course->chapters[chapter].lesson_count; lesson_index++) {
      const char *lesson_id = course->chapters[chapter].lessons[lesson_index].id;
      size_t seen = 0U;
      for (size_t candidate = 0U; candidate < course->lesson_count; candidate++) {
        const hol_lesson *other = hol_course_lesson(course, candidate);
        if (other != NULL && strcmp(other->id, lesson_id) == 0) seen++;
      }
      if (seen != 1U) {
        hol_error_set(error, HOL_ERR_SCHEMA, "duplicate lesson id");
        goto failure;
      }
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
