#include "hol.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void write_source(const char *workspace, const char *source) {
  char path[4096];
  hol_error error = {0};
  assert(hol_join_path(path, sizeof(path), workspace, "main.c", &error) == 0);
  assert(hol_atomic_write(path, source, strlen(source), &error) == 0);
}

static void write_named(const char *workspace, const char *name, const char *source) {
  char path[4096];
  hol_error error = {0};
  assert(hol_join_path(path, sizeof(path), workspace, name, &error) == 0);
  assert(hol_atomic_write(path, source, strlen(source), &error) == 0);
}

static int64_t monotonic_ms(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
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
               "#include <signal.h>\n#include <unistd.h>\n"
               "int main(void) { if (fork() == 0) { signal(SIGTERM, SIG_IGN); "
               "for (;;) pause(); } return 0; }\n");
  int64_t started = monotonic_ms();
  assert(hol_runner_execute(&course, &lesson, workspace, HOL_RUN, &result, &error) == 0);
  assert(monotonic_ms() - started < 2000);
  hol_run_result_free(&result);

  (void)strcpy(file.target, "-main.c");
  write_named(workspace, "-main.c", "int main(void) { return 0; }\n");
  assert(hol_runner_execute(&course, &lesson, workspace, HOL_RUN, &result, &error) == 0);
  assert(result.passed);
  hol_run_result_free(&result);
  (void)strcpy(file.target, "main.c");

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

  hol_course_file sql_files[3] = {0};
  (void)strcpy(sql_files[0].target, "001_up.sql");
  sql_files[0].role = HOL_FILE_READONLY;
  (void)strcpy(sql_files[1].target, "002_main.sql");
  sql_files[1].role = HOL_FILE_EDITABLE;
  (void)strcpy(sql_files[2].target, "003_test.sql");
  sql_files[2].role = HOL_FILE_READONLY;
  hol_lesson sql_lesson = {
    .kind = HOL_LESSON_EXERCISE,
    .files = sql_files,
    .file_count = 3U,
    .runner = {
      .id = "sql",
      .profile = "sqlite3",
      .check_kind = HOL_CHECK_TESTS,
    },
  };
  write_named(workspace, "001_up.sql",
              "CREATE TABLE users(name TEXT); INSERT INTO users VALUES ('Ada');\n");
  write_named(workspace, "002_main.sql", "SELECT name FROM users;\n");
  write_named(workspace, "003_test.sql", "SELECT count(*) AS total FROM users;\n");
  assert(unsetenv("HOL_TEST_TIMEOUT_MS") == 0);
  assert(hol_runner_execute(&course, &sql_lesson, workspace, HOL_RUN,
                            &result, &error) == 0);
  assert(result.passed && strstr(result.stdout_data, "Ada") != NULL);
  assert(strstr(result.stdout_data, "total") == NULL);
  hol_run_result_free(&result);
  assert(hol_runner_execute(&course, &sql_lesson, workspace, HOL_CHECK,
                            &result, &error) == 0);
  assert(result.passed && strstr(result.stdout_data, "total") != NULL);
  hol_run_result_free(&result);

  const size_t maximum_sql = 16U * HOL_OUTPUT_MAX;
  char *large_sql = malloc(maximum_sql - 1U);
  assert(large_sql != NULL);
  memset(large_sql, ' ', maximum_sql - 2U);
  large_sql[maximum_sql - 2U] = '\0';
  write_named(workspace, "001_up.sql", large_sql);
  write_named(workspace, "002_main.sql", "x");
  free(large_sql);
  sql_lesson.file_count = 2U;
  assert(hol_runner_execute(&course, &sql_lesson, workspace, HOL_RUN,
                            &result, &error) == -1);

  char command[8192];
  (void)snprintf(command, sizeof(command), "rm -rf -- '%s'", workspace);
  assert(system(command) == 0);
  puts("runner tests passed");
  return 0;
}
