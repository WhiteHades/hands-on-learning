#include "hol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern void __asan_init(void) __attribute__((weak));

typedef struct {
  char *data;
  size_t length;
  bool truncated;
  bool closed;
} capture_buffer;

static int64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
  return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static int effective_timeout_ms(void) {
  const char *override = getenv("HOL_TEST_TIMEOUT_MS");
  if (override == NULL) return HOL_PROCESS_TIMEOUT_MS;
  char *end = NULL;
  long value = strtol(override, &end, 10);
  if (end == override || *end != '\0' || value < 50L ||
      value > HOL_PROCESS_TIMEOUT_MS) return HOL_PROCESS_TIMEOUT_MS;
  return (int)value;
}

static void apply_limits(void) {
  struct rlimit cpu = {.rlim_cur = 10U, .rlim_max = 10U};
  struct rlimit memory = {.rlim_cur = 512U * 1024U * 1024U,
                          .rlim_max = 512U * 1024U * 1024U};
  struct rlimit files = {.rlim_cur = 64U, .rlim_max = 64U};
  struct rlimit output = {.rlim_cur = 16U * 1024U * 1024U,
                          .rlim_max = 16U * 1024U * 1024U};
  (void)setrlimit(RLIMIT_CPU, &cpu);
  if (__asan_init == NULL) (void)setrlimit(RLIMIT_AS, &memory);
  (void)setrlimit(RLIMIT_NOFILE, &files);
  (void)setrlimit(RLIMIT_FSIZE, &output);
}

static int set_nonblocking(int descriptor) {
  int flags = fcntl(descriptor, F_GETFL);
  if (flags < 0) return -1;
  return fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
}

static void capture_read(int descriptor, capture_buffer *buffer, pid_t process) {
  char chunk[8192];
  for (;;) {
    ssize_t count = read(descriptor, chunk, sizeof(chunk));
    if (count > 0) {
      size_t incoming = (size_t)count;
      size_t available = HOL_OUTPUT_MAX - buffer->length;
      size_t retained = incoming < available ? incoming : available;
      if (retained > 0U) {
        memcpy(buffer->data + buffer->length, chunk, retained);
        buffer->length += retained;
        buffer->data[buffer->length] = '\0';
      }
      if (retained != incoming) {
        buffer->truncated = true;
        (void)kill(-process, SIGTERM);
      }
      continue;
    }
    if (count == 0) buffer->closed = true;
    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      buffer->closed = true;
    break;
  }
}

static int execute_process(char *const arguments[], const char *working_directory,
                           const char *input_path, bool submit, hol_run_result *result,
                           hol_error *error) {
  int standard_output[2];
  int standard_error[2];
  if (pipe(standard_output) < 0 || pipe(standard_error) < 0) {
    hol_error_set(error, HOL_ERR_PROCESS, "cannot create process pipes");
    return -1;
  }
  pid_t process = fork();
  if (process < 0) {
    hol_error_set(error, HOL_ERR_PROCESS, "cannot fork child process");
    return -1;
  }
  if (process == 0) {
    (void)setpgid(0, 0);
    (void)close(standard_output[0]);
    (void)close(standard_error[0]);
    int input = input_path != NULL ? open(input_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
    if ((input_path != NULL && input < 0) ||
        (input >= 0 && dup2(input, STDIN_FILENO) < 0) ||
        dup2(standard_output[1], STDOUT_FILENO) < 0 ||
        dup2(standard_error[1], STDERR_FILENO) < 0 ||
        close(standard_output[1]) < 0 || close(standard_error[1]) < 0 ||
        chdir(working_directory) < 0) _exit(126);
    if (input >= 0) (void)close(input);
    apply_limits();
    if (setenv("HOL_SUBMIT", submit ? "1" : "0", 1) < 0) _exit(126);
    execvp(arguments[0], arguments);
    _exit(errno == ENOENT ? 127 : 126);
  }

  (void)setpgid(process, process);
  (void)close(standard_output[1]);
  (void)close(standard_error[1]);
  if (set_nonblocking(standard_output[0]) < 0 ||
      set_nonblocking(standard_error[0]) < 0) {
    (void)kill(-process, SIGKILL);
    (void)waitpid(process, NULL, 0);
    hol_error_set(error, HOL_ERR_PROCESS, "cannot configure process pipes");
    return -1;
  }
  capture_buffer output = {.data = calloc(HOL_OUTPUT_MAX + 1U, 1U)};
  capture_buffer errors = {.data = calloc(HOL_OUTPUT_MAX + 1U, 1U)};
  if (output.data == NULL || errors.data == NULL) {
    free(output.data);
    free(errors.data);
    (void)kill(-process, SIGKILL);
    (void)waitpid(process, NULL, 0);
    return -1;
  }

  int status = 0;
  bool exited = false;
  bool terminated = false;
  int64_t started = monotonic_ms();
  int64_t terminated_at = 0;
  while (!exited || !output.closed || !errors.closed) {
    struct pollfd descriptors[2] = {
      {.fd = standard_output[0], .events = POLLIN | POLLHUP},
      {.fd = standard_error[0], .events = POLLIN | POLLHUP},
    };
    (void)poll(descriptors, 2U, 25);
    capture_read(standard_output[0], &output, process);
    capture_read(standard_error[0], &errors, process);
    if (!exited) {
      pid_t waited = waitpid(process, &status, WNOHANG);
      if (waited == process) exited = true;
    }
    int64_t elapsed = monotonic_ms() - started;
    if (!exited && !terminated && elapsed >= effective_timeout_ms()) {
      result->timed_out = true;
      terminated = true;
      terminated_at = monotonic_ms();
      (void)kill(-process, SIGTERM);
    }
    if (!exited && terminated && monotonic_ms() - terminated_at >= 250) {
      (void)kill(-process, SIGKILL);
    }
  }
  (void)close(standard_output[0]);
  (void)close(standard_error[0]);
  result->stdout_data = output.data;
  result->stderr_data = errors.data;
  result->stdout_truncated = output.truncated;
  result->stderr_truncated = errors.truncated;
  if (WIFEXITED(status)) result->exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) {
    result->exit_code = 128 + WTERMSIG(status);
    result->term_signal = WTERMSIG(status);
  } else result->exit_code = -1;
  return 0;
}

static bool ends_with(const char *value, const char *suffix) {
  size_t value_length = strlen(value);
  size_t suffix_length = strlen(suffix);
  return value_length >= suffix_length &&
         strcmp(value + value_length - suffix_length, suffix) == 0;
}

static void trim_trailing_newlines(char *value) {
  size_t length = strlen(value);
  while (length > 0U && (value[length - 1U] == '\n' || value[length - 1U] == '\r'))
    value[--length] = '\0';
}

static int compare_targets(const void *left, const void *right) {
  const hol_course_file *const *first = left;
  const hol_course_file *const *second = right;
  return strcmp((*first)->target, (*second)->target);
}

static int run_sql(const hol_lesson *lesson, const char *workspace,
                   hol_run_mode mode, hol_run_result *result,
                   hol_error *error) {
  hol_course_file **files = calloc(lesson->file_count, sizeof(*files));
  if (files == NULL) return -1;
  size_t file_count = 0U;
  for (size_t index = 0U; index < lesson->file_count; index++)
    if (ends_with(lesson->files[index].target, ".sql")) files[file_count++] = &lesson->files[index];
  qsort(files, file_count, sizeof(*files), compare_targets);
  char *sql = calloc(16U * HOL_OUTPUT_MAX + 1U, 1U);
  if (sql == NULL) {
    free(files);
    return -1;
  }
  size_t length = 0U;
  for (size_t index = 0U; index < file_count; index++) {
    bool check_only = strstr(files[index]->target, "test") != NULL;
    if (mode == HOL_RUN && check_only) continue;
    char path[4096];
    if (hol_join_path(path, sizeof(path), workspace, files[index]->target, error) < 0) {
      free(sql);
      free(files);
      return -1;
    }
    size_t file_length = 0U;
    char *content = hol_read_text(path, 16U * HOL_OUTPUT_MAX - length,
                                  &file_length, error);
    if (content == NULL || file_length > 16U * HOL_OUTPUT_MAX - length - 2U) {
      free(content);
      free(sql);
      free(files);
      return -1;
    }
    memcpy(sql + length, content, file_length);
    length += file_length;
    sql[length++] = '\n';
    free(content);
  }
  free(files);
  char input[4096];
  char database[4096];
  int input_length = snprintf(input, sizeof(input), "%s/.hol-input.sql", workspace);
  int database_length = snprintf(database, sizeof(database), "%s/.hol.sqlite3", workspace);
  if (input_length < 0 || (size_t)input_length >= sizeof(input) || database_length < 0 ||
      (size_t)database_length >= sizeof(database) ||
      hol_atomic_write(input, sql, length, error) < 0) {
    free(sql);
    return -1;
  }
  free(sql);
  (void)unlink(database);
  char *arguments[] = {
    "sqlite3", "-batch", "-bail", "-header", "-column", database, NULL,
  };
  int status = execute_process(arguments, workspace, input, mode == HOL_CHECK,
                               result, error);
  (void)unlink(input);
  (void)unlink(database);
  if (status == 0)
    result->passed = result->exit_code == 0 && !result->timed_out &&
                     !result->stdout_truncated && !result->stderr_truncated;
  return status;
}

int hol_runner_execute(const hol_course *course, const hol_lesson *lesson,
                       const char *workspace, hol_run_mode mode,
                       hol_run_result *result, hol_error *error) {
  (void)course;
  if (lesson == NULL || workspace == NULL || result == NULL ||
      (strcmp(lesson->runner.id, "c") != 0 && strcmp(lesson->runner.id, "sql") != 0)) {
    hol_error_set(error, HOL_ERR_UNSUPPORTED, "lesson has no supported runner");
    return -1;
  }
  memset(result, 0, sizeof(*result));
  if (strcmp(lesson->runner.id, "sql") == 0)
    return run_sql(lesson, workspace, mode, result, error);
  char executable[4096];
  int executable_length = snprintf(executable, sizeof(executable), "%s/.hol-program", workspace);
  if (executable_length < 0 || (size_t)executable_length >= sizeof(executable)) return -1;

  size_t capacity = lesson->file_count + 16U;
  char **compiler = calloc(capacity, sizeof(*compiler));
  if (compiler == NULL) return -1;
  size_t count = 0U;
  compiler[count++] = "gcc";
  compiler[count++] = "-O0";
  compiler[count++] = "-g";
  compiler[count++] = "-Wall";
  compiler[count++] = "-Wextra";
  if (strcmp(lesson->runner.profile, "c11-32") == 0) compiler[count++] = "-m32";
  compiler[count++] = strcmp(lesson->runner.profile, "c23") == 0 ? "-std=c23" : "-std=c11";
  for (size_t index = 0U; index < lesson->file_count; index++) {
    if (ends_with(lesson->files[index].target, ".c"))
      compiler[count++] = lesson->files[index].target;
  }
  if (count <= 6U) {
    free(compiler);
    hol_error_set(error, HOL_ERR_SCHEMA, "C lesson has no source files");
    return -1;
  }
  if (strcmp(lesson->runner.profile, "c11-32") == 0) {
    compiler[count++] = "-Wl,--wrap=malloc";
    compiler[count++] = "-Wl,--wrap=calloc";
    compiler[count++] = "-Wl,--wrap=realloc";
    compiler[count++] = "-Wl,--wrap=free";
  }
  compiler[count++] = "-lm";
  compiler[count++] = "-o";
  compiler[count++] = executable;
  compiler[count] = NULL;

  hol_run_result compilation = {0};
  int status = execute_process(compiler, workspace, NULL, false, &compilation, error);
  free(compiler);
  if (status < 0) return -1;
  if (compilation.exit_code != 0 || compilation.timed_out) {
    *result = compilation;
    return 0;
  }
  hol_run_result_free(&compilation);
  char *execution_arguments[] = {executable, NULL};
  if (execute_process(execution_arguments, workspace, NULL, mode == HOL_CHECK,
                      result, error) < 0) {
    (void)unlink(executable);
    return -1;
  }
  (void)unlink(executable);
  result->passed = result->exit_code == 0 && !result->timed_out &&
                   !result->stdout_truncated && !result->stderr_truncated;
  if (mode == HOL_CHECK && lesson->runner.check_kind == HOL_CHECK_STDOUT &&
      lesson->runner.expected_output != NULL) {
    char *expected = strdup(lesson->runner.expected_output);
    if (expected == NULL) return -1;
    trim_trailing_newlines(result->stdout_data);
    trim_trailing_newlines(expected);
    result->passed = result->passed && strcmp(result->stdout_data, expected) == 0;
    free(expected);
  }
  return 0;
}

void hol_run_result_free(hol_run_result *result) {
  if (result == NULL) return;
  free(result->stdout_data);
  free(result->stderr_data);
  memset(result, 0, sizeof(*result));
}
