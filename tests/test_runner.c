#include "hol.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void write_source(const char *workspace, const char *source) {
  char path[4096];
  hol_error error = {0};
  assert(hol_join_path(path, sizeof(path), workspace, "greeting.c", &error) == 0);
  assert(hol_atomic_write(path, source, strlen(source), &error) == 0);
}

static void write_named(const char *workspace, const char *name,
                        const char *source) {
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
  hol_error error = {0};
  hol_course *course = NULL;
  assert(hol_course_load_profile("build/test-course.imscc",
                                 "build/test-course.profile.json",
                                 &course, &error) == 0);
  const hol_lesson *lesson = hol_course_lesson(course, 1U);
  assert(lesson != NULL && lesson->kind == HOL_LESSON_EXERCISE);

  char workspace[] = "/tmp/hol-runner-XXXXXX";
  assert(mkdtemp(workspace) != NULL);
  assert(hol_workspace_ensure(course, lesson, workspace, &error) == 0);
  char path[4096];
  assert(hol_join_path(path, sizeof(path), workspace, "greeting.c", &error) == 0);
  assert(access(path, R_OK) == 0);
  assert(hol_join_path(path, sizeof(path), workspace, "visible_test.c", &error) == 0);
  assert(access(path, F_OK) < 0);
  assert(hol_join_path(path, sizeof(path), workspace, "exercise_support.c", &error) == 0);
  assert(access(path, F_OK) < 0);
  assert(hol_join_path(path, sizeof(path), workspace, "check_test.c", &error) == 0);
  assert(access(path, F_OK) < 0);

  hol_run_result result = {0};
  assert(setenv("HOL_HOST_SECRET", "must-not-enter-sandbox", 1) == 0);
  assert(hol_runner_execute(course, lesson, workspace, HOL_RUN, &result, &error) == 0);
  if (result.stderr_data == NULL ||
      strstr(result.stderr_data, "Expected Hello, learner!") == NULL)
    (void)fprintf(stderr, "run failed: exit=%d stdout=%s stderr=%s\n",
                  result.exit_code, result.stdout_data != NULL ? result.stdout_data : "(null)",
                  result.stderr_data != NULL ? result.stderr_data : "(null)");
  assert(!result.passed);
  assert(strstr(result.stderr_data, "Expected Hello, learner!") != NULL);
  hol_run_result_free(&result);

  assert(hol_join_path(path, sizeof(path), workspace, "check_test.c", &error) == 0);
  static const char forged_test[] =
    "#include <stdio.h>\nint main(void) { puts(\"forged\"); return 0; }\n";
  assert(hol_atomic_write(path, forged_test, strlen(forged_test), &error) == 0);
  write_source(workspace,
               "const char *lesson_greeting(void) { return \"Hello, learner!\"; }\n");
  assert(hol_runner_execute(course, lesson, workspace, HOL_RUN, &result, &error) == 0);
  assert(result.passed && strstr(result.stdout_data, "Hello, learner!") != NULL);
  assert(strstr(result.stdout_data, "forged") == NULL);
  hol_run_result_free(&result);
  write_source(workspace,
               "#include <stdlib.h>\nconst char *lesson_greeting(void) { _Exit(0); }\n");
  assert(hol_runner_execute(course, lesson, workspace, HOL_CHECK,
                            &result, &error) == 0);
  assert(result.exit_code == 0 && !result.passed);
  hol_run_result_free(&result);
  write_source(workspace,
               "const char *lesson_greeting(void) { return \"Hello, learner!\"; }\n");
  assert(hol_runner_execute(course, lesson, workspace, HOL_CHECK, &result, &error) == 0);
  assert(result.passed && strstr(result.stdout_data, "All supplied tests passed.") != NULL);
  assert(strstr(result.stdout_data, "forged") == NULL);
  hol_run_result_free(&result);
  assert(unsetenv("HOL_HOST_SECRET") == 0);

  write_source(workspace, "const char *lesson_greeting(void) { return \"Wrong\"; }\n");
  assert(hol_runner_execute(course, lesson, workspace, HOL_CHECK, &result, &error) == 0);
  assert(!result.passed);
  hol_run_result_free(&result);

  assert(hol_workspace_reset(course, lesson, workspace, &error) == 0);
  assert(hol_join_path(path, sizeof(path), workspace, "greeting.c", &error) == 0);
  char *starter = hol_read_text(path, HOL_OUTPUT_MAX, NULL, &error);
  assert(starter != NULL && strstr(starter, "TODO") != NULL);
  free(starter);
  assert(hol_join_path(path, sizeof(path), workspace, "check_test.c", &error) == 0);
  assert(access(path, F_OK) < 0);

  write_source(workspace,
               "#include <signal.h>\n#include <unistd.h>\n"
               "const char *lesson_greeting(void) { if (fork() == 0) { "
               "signal(SIGTERM, SIG_IGN); for (;;) pause(); } "
               "return \"Hello, learner!\"; }\n");
  int64_t descendant_started = monotonic_ms();
  assert(hol_runner_execute(course, lesson, workspace, HOL_RUN,
                            &result, &error) == 0);
  assert(monotonic_ms() - descendant_started < 2000);
  hol_run_result_free(&result);

  write_source(workspace,
               "#include <sys/wait.h>\n#include <unistd.h>\n"
               "const char *lesson_greeting(void) { int children = 0; "
               "for (int i = 0; i < 64; i++) { pid_t child = fork(); "
               "if (child == 0) _exit(0); if (child < 0) break; children++; } "
               "while (wait(NULL) > 0) {} return children < 64 "
               "? \"Hello, learner!\" : \"unbounded\"; }\n");
  assert(hol_runner_execute(course, lesson, workspace, HOL_RUN,
                            &result, &error) == 0);
  assert(result.passed);
  hol_run_result_free(&result);

  assert(setenv("HOL_TEST_TIMEOUT_MS", "100", 1) == 0);
  write_source(workspace,
               "const char *lesson_greeting(void) { for (;;) {} return \"never\"; }\n");
  int64_t started = monotonic_ms();
  assert(hol_runner_execute(course, lesson, workspace, HOL_RUN, &result, &error) == 0);
  assert(result.timed_out && !result.passed);
  assert(monotonic_ms() - started < 2000);
  hol_run_result_free(&result);
  assert(unsetenv("HOL_TEST_TIMEOUT_MS") == 0);

  char *saved_path = strdup(getenv("PATH"));
  assert(saved_path != NULL && setenv("PATH", "/no-bubblewrap", 1) == 0);
  memset(&error, 0, sizeof(error));
  assert(hol_runner_execute(course, lesson, workspace, HOL_RUN, &result, &error) == -1);
  assert(error.code == HOL_ERR_UNSUPPORTED);
  assert(strstr(error.message, "Bubblewrap") != NULL);
  assert(setenv("PATH", saved_path, 1) == 0);
  free(saved_path);

  assert(hol_join_path(path, sizeof(path), course->root,
                       "exercises/visible_test.c", &error) == 0);
  assert(hol_atomic_write(path, forged_test, strlen(forged_test), &error) == 0);
  memset(&error, 0, sizeof(error));
  assert(hol_runner_execute(course, lesson, workspace, HOL_RUN, &result, &error) == -1);
  assert(error.code == HOL_ERR_CHECKSUM);

  const hol_lesson *stdout_lesson = hol_course_lesson(course, 2U);
  assert(stdout_lesson != NULL && stdout_lesson->runner.check_kind == HOL_CHECK_STDOUT);
  assert(hol_workspace_ensure(course, stdout_lesson, workspace, &error) == 0);
  write_named(workspace, "main.c",
              "#include <stdio.h>\nint main(void) { puts(\"Hello stdout!\"); }\n");
  write_named(workspace, "course.profile.json",
              "{\"expected\":\"substituted\"}\n");
  assert(hol_runner_execute(course, stdout_lesson, workspace, HOL_CHECK,
                            &result, &error) == 0);
  assert(result.passed && strcmp(result.stdout_data, "Hello stdout!") == 0);
  hol_run_result_free(&result);
  write_named(workspace, "main.c",
              "#include <stdio.h>\nint main(void) { puts(\"substituted\"); }\n");
  assert(hol_runner_execute(course, stdout_lesson, workspace, HOL_CHECK,
                            &result, &error) == 0);
  assert(!result.passed);
  hol_run_result_free(&result);

  const hol_lesson *sql_lesson = hol_course_lesson(course, 3U);
  assert(sql_lesson != NULL && strcmp(sql_lesson->runner.id, "sql") == 0);
  assert(hol_workspace_ensure(course, sql_lesson, workspace, &error) == 0);
  assert(hol_runner_execute(course, sql_lesson, workspace, HOL_RUN,
                            &result, &error) == 0);
  assert(result.passed && strstr(result.stdout_data, "Ada") != NULL);
  assert(strstr(result.stdout_data, "total") == NULL);
  hol_run_result_free(&result);
  assert(hol_runner_execute(course, sql_lesson, workspace, HOL_CHECK,
                            &result, &error) == 0);
  assert(result.passed && strstr(result.stdout_data, "total") != NULL);
  assert(strstr(result.stdout_data, "HOL_TEST_COMPLETE") == NULL);
  hol_run_result_free(&result);

  hol_course_free(course);
  char command[8192];
  (void)snprintf(command, sizeof(command), "rm -rf -- '%s'", workspace);
  assert(system(command) == 0);
  puts("runner tests passed");
  return 0;
}
