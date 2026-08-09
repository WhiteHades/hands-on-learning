#ifndef HOL_H
#define HOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HOL_APP_NAME "Hands-on Learning"
#define HOL_APP_VERSION "0.1.0"
#define HOL_PATH_MAX 240
#define HOL_ID_MAX 64
#define HOL_TEXT_MAX 256
#define HOL_ERROR_MAX 384
#define HOL_OUTPUT_MAX (1024U * 1024U)
#define HOL_PROCESS_TIMEOUT_MS 8000

typedef enum {
  HOL_OK = 0,
  HOL_ERR_ARGUMENT,
  HOL_ERR_IO,
  HOL_ERR_JSON,
  HOL_ERR_SCHEMA,
  HOL_ERR_PATH,
  HOL_ERR_CHECKSUM,
  HOL_ERR_PROCESS,
  HOL_ERR_TIMEOUT,
  HOL_ERR_UNSUPPORTED
} hol_errc;

typedef struct {
  hol_errc code;
  int system_errno;
  char message[HOL_ERROR_MAX];
} hol_error;

typedef enum {
  HOL_LESSON_READING,
  HOL_LESSON_EXERCISE,
  HOL_LESSON_QUIZ
} hol_lesson_kind;

typedef enum {
  HOL_FILE_EDITABLE,
  HOL_FILE_READONLY,
  HOL_FILE_HIDDEN
} hol_file_role;

typedef enum {
  HOL_CHECK_NONE,
  HOL_CHECK_STDOUT,
  HOL_CHECK_TESTS
} hol_check_kind;

typedef struct {
  char source[HOL_PATH_MAX + 1];
  char target[HOL_PATH_MAX + 1];
  char syntax[32];
  hol_file_role role;
} hol_course_file;

typedef struct {
  char id[HOL_ID_MAX + 1];
  char text[HOL_TEXT_MAX + 1];
} hol_quiz_choice;

typedef struct {
  char id[HOL_ID_MAX + 1];
  char prompt[1025];
  char answer[HOL_ID_MAX + 1];
  char explanation[1025];
  hol_quiz_choice *choices;
  size_t choice_count;
} hol_quiz_question;

typedef struct {
  char id[16];
  char profile[16];
  hol_check_kind check_kind;
  char *expected_output;
} hol_runner_spec;

typedef struct {
  char id[HOL_ID_MAX + 1];
  char title[HOL_TEXT_MAX + 1];
  char content_path[HOL_PATH_MAX + 1];
  hol_lesson_kind kind;
  hol_course_file *files;
  size_t file_count;
  char default_file[HOL_PATH_MAX + 1];
  hol_runner_spec runner;
  hol_quiz_question *questions;
  size_t question_count;
  size_t quiz_passing_score;
  char **media_paths;
  size_t media_count;
} hol_lesson;

typedef struct {
  char id[HOL_ID_MAX + 1];
  char title[HOL_TEXT_MAX + 1];
  hol_lesson *lessons;
  size_t lesson_count;
} hol_chapter;

typedef struct {
  unsigned schema_version;
  char id[HOL_ID_MAX + 1];
  char version[32];
  char minimum_app_version[32];
  char title[HOL_TEXT_MAX + 1];
  char description[1025];
  char license_spdx[32];
  char license_file[HOL_PATH_MAX + 1];
  char attribution[1025];
  char root[4096];
  char source_path[4096];
  bool owns_root;
  hol_chapter *chapters;
  size_t chapter_count;
  size_t lesson_count;
} hol_course;

typedef enum {
  HOL_RUN,
  HOL_CHECK
} hol_run_mode;

typedef struct {
  char *stdout_data;
  char *stderr_data;
  int exit_code;
  int term_signal;
  bool timed_out;
  bool stdout_truncated;
  bool stderr_truncated;
  bool passed;
} hol_run_result;

typedef enum {
  HOL_MODE_NORMAL,
  HOL_MODE_SEARCH,
  HOL_MODE_CONFIRM
} hol_input_mode;

typedef enum {
  HOL_ACTION_NONE,
  HOL_ACTION_QUIT,
  HOL_ACTION_CANCEL,
  HOL_ACTION_UP,
  HOL_ACTION_DOWN,
  HOL_ACTION_LEFT,
  HOL_ACTION_RIGHT,
  HOL_ACTION_TOP,
  HOL_ACTION_BOTTOM,
  HOL_ACTION_HALF_UP,
  HOL_ACTION_HALF_DOWN,
  HOL_ACTION_SEARCH,
  HOL_ACTION_NEXT_MATCH,
  HOL_ACTION_PREV_MATCH,
  HOL_ACTION_SELECT,
  HOL_ACTION_EDIT,
  HOL_ACTION_RUN,
  HOL_ACTION_CHECK,
  HOL_ACTION_RESET,
  HOL_ACTION_MEDIA,
  HOL_ACTION_LESSON_PICKER,
  HOL_ACTION_COURSE_PICKER,
  HOL_ACTION_NEXT_BUFFER,
  HOL_ACTION_PREV_BUFFER,
  HOL_ACTION_HELP
} hol_action;

typedef struct {
  hol_input_mode mode;
  char pending[8];
  size_t pending_length;
  int64_t pending_since_ms;
} hol_key_state;

typedef struct {
  char course_id[HOL_ID_MAX + 1];
  char lesson_id[HOL_ID_MAX + 1];
  bool completed;
} hol_progress_entry;

typedef struct {
  char course_id[HOL_ID_MAX + 1];
  char lesson_id[HOL_ID_MAX + 1];
  char file_path[HOL_PATH_MAX + 1];
  size_t reader_scroll;
  size_t preview_scroll;
  size_t output_scroll;
  int pane;
  hol_progress_entry *progress;
  size_t progress_count;
} hol_state;

void hol_error_set(hol_error *error, hol_errc code, const char *format, ...)
  __attribute__((format(printf, 3, 4)));
bool hol_safe_relative_path(const char *path);
bool hol_valid_id(const char *id);
int hol_join_path(char *output, size_t size, const char *root,
                  const char *relative, hol_error *error);
int hol_atomic_write(const char *path, const char *data, size_t length,
                     hol_error *error);
int hol_copy_file_if_missing(const char *source, const char *target,
                             hol_error *error);
char *hol_read_text(const char *path, size_t maximum, size_t *length,
                    hol_error *error);
int hol_sha256_file(const char *path, char hexadecimal[65],
                    hol_error *error);
bool hol_version_supported(const char *minimum_version);

int hol_course_load(const char *root, hol_course **output, hol_error *error);
const hol_lesson *hol_course_lesson(const hol_course *course, size_t index);
void hol_course_free(hol_course *course);

int hol_state_load(const char *path, hol_state *state, hol_error *error);
int hol_state_save(const char *path, const hol_state *state, hol_error *error);
bool hol_state_completed(const hol_state *state, const char *course_id,
                         const char *lesson_id);
int hol_state_mark_completed(hol_state *state, const char *course_id,
                             const char *lesson_id, hol_error *error);
void hol_state_free(hol_state *state);

int hol_workspace_ensure(const hol_course *course, const hol_lesson *lesson,
                         const char *workspace, hol_error *error);
int hol_workspace_reset(const hol_course *course, const hol_lesson *lesson,
                        const char *workspace, hol_error *error);

int hol_runner_execute(const hol_course *course, const hol_lesson *lesson,
                       const char *workspace, hol_run_mode mode,
                       hol_run_result *result, hol_error *error);
void hol_run_result_free(hol_run_result *result);

void hol_keys_init(hol_key_state *state);
hol_action hol_keys_feed(hol_key_state *state, int key, int64_t now_ms);
const char *hol_action_name(hol_action action);

int hol_launch_editor(const char *path, hol_error *error);
int hol_launch_media(const char *path, hol_error *error);

int hol_catalog_list(const char *catalog_path, FILE *stream, hol_error *error);
int hol_catalog_install(const char *catalog_path, const char *course_id,
                        const char *destination, hol_error *error);

int hol_ui_run(const char *course_root, hol_error *error);

#endif
