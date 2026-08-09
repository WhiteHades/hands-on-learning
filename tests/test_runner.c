#include "hol.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_source(const char *workspace, const char *source) {
  char path[4096];
  hol_error error = {0};
  assert(hol_join_path(path, sizeof(path), workspace, "main.c", &error) == 0);
  assert(hol_atomic_write(path, source, strlen(source), &error) == 0);
}

int main(void) {
  char workspace[] = "/tmp/hol-runner-XXXXXX";
  assert(mkdtemp(workspace) != NULL);
  hol_course_file file = {.role = HOL_FILE_EDITABLE};
  (void)strcpy(file.target, "main.c");
  hol_lesson lesson = {
    .kind = HOL_LESSON_EXERCISE,
    .files = &file,
    .file_count = 1U,
    .runner = {
      .id = "c",
      .profile = "c11",
      .check_kind = HOL_CHECK_STDOUT,
      .expected_output = "Hello, learner!\n",
    },
  };
  hol_course course = {0};
  hol_error error = {0};
  hol_run_result result = {0};

  write_source(workspace,
               "#include <stdio.h>\nint main(void) { puts(\"Hello, learner!\"); }\n");
  assert(hol_runner_execute(&course, &lesson, workspace, HOL_CHECK, &result, &error) == 0);
  if (!result.passed)
    (void)fprintf(stderr, "runner failed: exit=%d timeout=%d stdout=%s stderr=%s\n",
                  result.exit_code, result.timed_out, result.stdout_data,
                  result.stderr_data);
  assert(result.passed);
  assert(strcmp(result.stdout_data, "Hello, learner!") == 0);
  hol_run_result_free(&result);

  write_source(workspace,
               "#include <stdio.h>\nint main(void) { puts(\"Wrong\"); }\n");
  assert(hol_runner_execute(&course, &lesson, workspace, HOL_RUN, &result, &error) == 0);
  assert(result.passed);
  hol_run_result_free(&result);
  assert(hol_runner_execute(&course, &lesson, workspace, HOL_CHECK, &result, &error) == 0);
  assert(!result.passed);
  hol_run_result_free(&result);

  assert(setenv("HOL_TEST_TIMEOUT_MS", "100", 1) == 0);
  write_source(workspace, "int main(void) { for (;;) {} }\n");
  assert(hol_runner_execute(&course, &lesson, workspace, HOL_RUN, &result, &error) == 0);
  assert(result.timed_out);
  assert(!result.passed);
  hol_run_result_free(&result);

  char command[8192];
  (void)snprintf(command, sizeof(command), "rm -rf -- '%s'", workspace);
  assert(system(command) == 0);
  puts("runner tests passed");
  return 0;
}
