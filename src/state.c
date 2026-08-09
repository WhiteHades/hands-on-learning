#include "hol.h"

#include <errno.h>
#include <json-c/json.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int copy_state_string(char *target, size_t capacity, json_object *value,
                             const char *field, hol_error *error) {
  if (!json_object_is_type(value, json_type_string)) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid state field: %s", field);
    return -1;
  }
  const char *text = json_object_get_string(value);
  size_t length = strlen(text);
  if ((size_t)json_object_get_string_len(value) != length || length >= capacity) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid state field: %s", field);
    return -1;
  }
  memcpy(target, text, length + 1U);
  return 0;
}

static int load_size(json_object *object, const char *field, size_t *target,
                     hol_error *error) {
  json_object *value = NULL;
  if (!json_object_object_get_ex(object, field, &value) ||
      !json_object_is_type(value, json_type_int)) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid state field: %s", field);
    return -1;
  }
  int64_t number = json_object_get_int64(value);
  if (number < 0 || (uint64_t)number > SIZE_MAX) {
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid state field: %s", field);
    return -1;
  }
  *target = (size_t)number;
  return 0;
}

static bool optional_valid_id(const char *value) {
  return value[0] == '\0' || hol_valid_id(value);
}

int hol_state_load(const char *path, hol_state *state, hol_error *error) {
  size_t length = 0U;
  char *text = hol_read_text(path, 64U * 1024U, &length, error);
  if (text == NULL) {
    if (errno == ENOENT) {
      if (error != NULL) memset(error, 0, sizeof(*error));
      return 0;
    }
    return -1;
  }
  json_tokener *tokener = json_tokener_new_ex(16);
  json_object *object = json_tokener_parse_ex(tokener, text, (int)length);
  enum json_tokener_error status = json_tokener_get_error(tokener);
  json_tokener_free(tokener);
  free(text);
  if (object == NULL || status != json_tokener_success ||
      !json_object_is_type(object, json_type_object)) {
    if (object != NULL) json_object_put(object);
    hol_error_set(error, HOL_ERR_JSON, "state file is not valid JSON");
    return -1;
  }

  hol_state loaded = {0};
  json_object *schema = NULL;
  json_object *course_id = NULL;
  json_object *lesson_id = NULL;
  json_object *file_path = NULL;
  json_object *pane = NULL;
  json_object *progress = NULL;
  if (!json_object_object_get_ex(object, "schema_version", &schema) ||
      !json_object_is_type(schema, json_type_int) || json_object_get_int(schema) != 1 ||
      !json_object_object_get_ex(object, "course_id", &course_id) ||
      !json_object_object_get_ex(object, "lesson_id", &lesson_id) ||
      !json_object_object_get_ex(object, "file_path", &file_path) ||
      !json_object_object_get_ex(object, "pane", &pane) ||
      !json_object_is_type(pane, json_type_int) ||
      !json_object_object_get_ex(object, "progress", &progress) ||
      !json_object_is_type(progress, json_type_array) ||
      copy_state_string(loaded.course_id, sizeof(loaded.course_id), course_id,
                        "course_id", error) < 0 ||
      copy_state_string(loaded.lesson_id, sizeof(loaded.lesson_id), lesson_id,
                        "lesson_id", error) < 0 ||
      copy_state_string(loaded.file_path, sizeof(loaded.file_path), file_path,
                        "file_path", error) < 0 ||
      load_size(object, "reader_scroll", &loaded.reader_scroll, error) < 0 ||
      load_size(object, "preview_scroll", &loaded.preview_scroll, error) < 0 ||
      load_size(object, "output_scroll", &loaded.output_scroll, error) < 0 ||
      !optional_valid_id(loaded.course_id) || !optional_valid_id(loaded.lesson_id) ||
      (loaded.file_path[0] != '\0' && !hol_safe_relative_path(loaded.file_path))) {
    goto failure;
  }
  loaded.pane = json_object_get_int(pane);
  if (loaded.pane < 0 || loaded.pane > 2) goto failure;

  size_t count = json_object_array_length(progress);
  if (count > 10000U) goto failure;
  if (count > 0U) {
    loaded.progress = calloc(count, sizeof(*loaded.progress));
    if (loaded.progress == NULL) goto failure;
  }
  loaded.progress_count = count;
  for (size_t index = 0U; index < count; index++) {
    json_object *entry = json_object_array_get_idx(progress, index);
    json_object *entry_course = NULL;
    json_object *entry_lesson = NULL;
    json_object *completed = NULL;
    if (!json_object_is_type(entry, json_type_object) ||
        !json_object_object_get_ex(entry, "course_id", &entry_course) ||
        !json_object_object_get_ex(entry, "lesson_id", &entry_lesson) ||
        !json_object_object_get_ex(entry, "completed", &completed) ||
        !json_object_is_type(completed, json_type_boolean) ||
        copy_state_string(loaded.progress[index].course_id,
                          sizeof(loaded.progress[index].course_id), entry_course,
                          "progress course_id", error) < 0 ||
        copy_state_string(loaded.progress[index].lesson_id,
                          sizeof(loaded.progress[index].lesson_id), entry_lesson,
                          "progress lesson_id", error) < 0 ||
        !hol_valid_id(loaded.progress[index].course_id) ||
        !hol_valid_id(loaded.progress[index].lesson_id)) goto failure;
    loaded.progress[index].completed = json_object_get_boolean(completed) != 0;
  }

  json_object_put(object);
  hol_state_free(state);
  *state = loaded;
  return 0;

failure:
  json_object_put(object);
  hol_state_free(&loaded);
  if (error != NULL && error->message[0] == '\0')
    hol_error_set(error, HOL_ERR_SCHEMA, "state file has invalid fields");
  return -1;
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
  json_object *progress = json_object_new_array_ext((int)state->progress_count);
  for (size_t index = 0U; index < state->progress_count; index++) {
    json_object *entry = json_object_new_object();
    json_object_object_add(entry, "course_id",
                           json_object_new_string(state->progress[index].course_id));
    json_object_object_add(entry, "lesson_id",
                           json_object_new_string(state->progress[index].lesson_id));
    json_object_object_add(entry, "completed",
                           json_object_new_boolean(state->progress[index].completed));
    json_object_array_add(progress, entry);
  }
  json_object_object_add(object, "progress", progress);
  const char *json = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PRETTY);
  int result = hol_atomic_write(path, json, strlen(json), error);
  json_object_put(object);
  return result;
}

bool hol_state_completed(const hol_state *state, const char *course_id,
                         const char *lesson_id) {
  for (size_t index = 0U; index < state->progress_count; index++)
    if (state->progress[index].completed &&
        strcmp(state->progress[index].course_id, course_id) == 0 &&
        strcmp(state->progress[index].lesson_id, lesson_id) == 0) return true;
  return false;
}

int hol_state_mark_completed(hol_state *state, const char *course_id,
                             const char *lesson_id, hol_error *error) {
  if (!hol_valid_id(course_id) || !hol_valid_id(lesson_id)) return -1;
  for (size_t index = 0U; index < state->progress_count; index++) {
    if (strcmp(state->progress[index].course_id, course_id) == 0 &&
        strcmp(state->progress[index].lesson_id, lesson_id) == 0) {
      state->progress[index].completed = true;
      return 0;
    }
  }
  if (state->progress_count >= 10000U) {
    hol_error_set(error, HOL_ERR_SCHEMA, "progress entry limit reached");
    return -1;
  }
  size_t next_count = state->progress_count + 1U;
  hol_progress_entry *next = realloc(state->progress, next_count * sizeof(*next));
  if (next == NULL) return -1;
  state->progress = next;
  hol_progress_entry *entry = &state->progress[state->progress_count];
  memset(entry, 0, sizeof(*entry));
  (void)memcpy(entry->course_id, course_id, strlen(course_id) + 1U);
  (void)memcpy(entry->lesson_id, lesson_id, strlen(lesson_id) + 1U);
  entry->completed = true;
  state->progress_count = next_count;
  return 0;
}

void hol_state_free(hol_state *state) {
  if (state == NULL) return;
  free(state->progress);
  memset(state, 0, sizeof(*state));
}
