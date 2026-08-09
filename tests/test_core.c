#include "hol.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  assert(hol_safe_relative_path("web/lesson.html"));
  assert(!hol_safe_relative_path("../private"));
  assert(!hol_safe_relative_path("/absolute"));
  assert(!hol_safe_relative_path("double//segment"));
  assert(!hol_safe_relative_path("windows\\path"));
  assert(hol_valid_id("course.lesson-1"));
  assert(!hol_valid_id("Course Lesson"));
  assert(hol_version_supported("0.1.0"));
  assert(!hol_version_supported("0.2.0"));

  hol_error error = {0};
  hol_course *course = NULL;
  assert(hol_course_load("build/hol.demo-c-1.0.0.imscc", &course, &error) == 0);
  assert(course != NULL);
  assert(strcmp(course->id, "hol.demo-c") == 0);
  assert(strcmp(course->version, "1.0.0") == 0);
  assert(strcmp(course->license_spdx, "MIT") == 0);
  assert(course->chapter_count == 2U);
  assert(course->lesson_count == 3U);
  const hol_lesson *welcome = hol_course_lesson(course, 0U);
  assert(welcome != NULL && strcmp(welcome->id, "welcome") == 0);
  assert(welcome->kind == HOL_LESSON_READING);
  const hol_lesson *quiz = hol_course_lesson(course, 2U);
  assert(quiz != NULL && quiz->kind == HOL_LESSON_QUIZ);
  assert(quiz->question_count == 1U && quiz->quiz_passing_score == 1U);
  assert(strcmp(quiz->questions[0].answer, "b") == 0);

  char root[] = "/tmp/hol-state-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char state_path[4096];
  assert(hol_join_path(state_path, sizeof(state_path), root, "state.json", &error) == 0);
  hol_state state = {0};
  (void)strcpy(state.course_id, course->id);
  (void)strcpy(state.lesson_id, quiz->id);
  state.reader_scroll = 12U;
  assert(hol_state_mark_completed(&state, course->id, quiz->id, &error) == 0);
  assert(hol_state_save(state_path, &state, &error) == 0);
  hol_state loaded = {0};
  assert(hol_state_load(state_path, &loaded, &error) == 0);
  assert(strcmp(loaded.lesson_id, "header-quiz") == 0);
  assert(loaded.reader_scroll == 12U);
  assert(hol_state_completed(&loaded, course->id, quiz->id));
  hol_state_free(&loaded);
  hol_state_free(&state);
  hol_course_free(course);

  hol_course *invalid = NULL;
  memset(&error, 0, sizeof(error));
  assert(hol_course_load("courses/demo", &invalid, &error) == -1);
  assert(invalid == NULL && error.code == HOL_ERR_UNSUPPORTED);
  assert(unlink(state_path) == 0);
  assert(rmdir(root) == 0);
  puts("core tests passed");
  return 0;
}
