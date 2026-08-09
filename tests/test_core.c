#include "hol.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char manifest[] =
  "{\n"
  "  \"schema_version\": 1,\n"
  "  \"id\": \"hol.test\",\n"
  "  \"version\": \"1.0.0\",\n"
  "  \"minimum_app_version\": \"0.1.0\",\n"
  "  \"title\": \"Test Course\",\n"
  "  \"description\": \"A local fixture.\",\n"
  "  \"license\": {\"spdx\": \"MIT\", \"file\": \"LICENSE\", "
  "\"attribution\": \"Test\"},\n"
  "  \"chapters\": [{\n"
  "    \"id\": \"start\", \"title\": \"Start\", \"lessons\": [{\n"
  "      \"id\": \"hello\", \"title\": \"Hello\", \"kind\": \"exercise\",\n"
  "      \"content\": \"lessons/hello.md\",\n"
  "      \"workspace\": {\"default_file\": \"main.c\", \"files\": [{\n"
  "        \"source\": \"lessons/main.c\", \"target\": \"main.c\",\n"
  "        \"role\": \"editable\", \"syntax\": \"c\"\n"
  "      }]},\n"
  "      \"runner\": {\"id\": \"c\", \"profile\": \"c11\",\n"
  "        \"check\": {\"kind\": \"stdout\", \"expected\": \"hello\\n\"}},\n"
  "      \"quiz\": null\n"
  "    }]\n"
  "  }]\n"
  "}\n";

static void write_file(const char *path, const char *content) {
  hol_error error = {0};
  assert(hol_atomic_write(path, content, strlen(content), &error) == 0);
}

int main(void) {
  assert(hol_safe_relative_path("lessons/start/main.c"));
  assert(!hol_safe_relative_path("../private"));
  assert(!hol_safe_relative_path("/absolute"));
  assert(!hol_safe_relative_path("double//segment"));
  assert(!hol_safe_relative_path("windows\\path"));

  char root[] = "/tmp/hol-core-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char path[4096];
  (void)snprintf(path, sizeof(path), "%s/course.json", root);
  write_file(path, manifest);
  (void)snprintf(path, sizeof(path), "%s/LICENSE", root);
  write_file(path, "MIT\n");
  (void)snprintf(path, sizeof(path), "%s/lessons/hello.md", root);
  write_file(path, "# Hello\n");
  (void)snprintf(path, sizeof(path), "%s/lessons/main.c", root);
  write_file(path, "int main(void) { return 0; }\n");

  hol_error error = {0};
  hol_course *course = NULL;
  assert(hol_course_load(root, &course, &error) == 0);
  assert(course != NULL);
  assert(course->lesson_count == 1U);
  const hol_lesson *lesson = hol_course_lesson(course, 0U);
  assert(lesson != NULL && strcmp(lesson->id, "hello") == 0);

  char workspace[4096];
  (void)snprintf(workspace, sizeof(workspace), "%s/workspace", root);
  assert(hol_workspace_ensure(course, lesson, workspace, &error) == 0);
  assert(hol_join_path(path, sizeof(path), workspace, "main.c", &error) == 0);
  write_file(path, "learner edit\n");
  assert(hol_workspace_ensure(course, lesson, workspace, &error) == 0);
  size_t length = 0U;
  char *text = hol_read_text(path, 1024U, &length, &error);
  assert(text != NULL && strcmp(text, "learner edit\n") == 0);
  free(text);
  assert(hol_workspace_reset(course, lesson, workspace, &error) == 0);
  text = hol_read_text(path, 1024U, &length, &error);
  assert(text != NULL && strstr(text, "return 0") != NULL);
  free(text);

  hol_state state = {0};
  (void)strcpy(state.course_id, "hol.test");
  (void)strcpy(state.lesson_id, "hello");
  state.reader_scroll = 12U;
  (void)snprintf(path, sizeof(path), "%s/state/progress.json", root);
  assert(hol_state_save(path, &state, &error) == 0);
  hol_state loaded = {0};
  assert(hol_state_load(path, &loaded, &error) == 0);
  assert(strcmp(loaded.lesson_id, "hello") == 0);
  assert(loaded.reader_scroll == 12U);

  hol_course_free(course);
  char command[8192];
  (void)snprintf(command, sizeof(command), "rm -rf -- '%s'", root);
  assert(system(command) == 0);
  puts("core tests passed");
  return 0;
}
