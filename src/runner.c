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

#define HOL_STAGE_MAX_BYTES (64U * 1024U * 1024U)

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

static int resolve_bwrap(char output[4096], hol_error *error) {
  const char *path = getenv("PATH");
  if (path != NULL) {
    const char *entry = path;
    while (*entry != '\0') {
      const char *separator = strchr(entry, ':');
      size_t length = separator != NULL ? (size_t)(separator - entry) : strlen(entry);
      if (length > 0U && length + sizeof("/bwrap") <= 4096U) {
        memcpy(output, entry, length);
        memcpy(output + length, "/bwrap", sizeof("/bwrap"));
        if (output[0] == '/' && access(output, X_OK) == 0) return 0;
      }
      if (separator == NULL) break;
      entry = separator + 1;
    }
  }
  hol_error_set(error, HOL_ERR_UNSUPPORTED,
                "Bubblewrap (bwrap) is required to run exercises; install it and ensure it is on PATH");
  return -1;
}

static size_t add_system_bind(char *arguments[], size_t count,
                              const char *path) {
  if (access(path, F_OK) == 0) {
    arguments[count++] = "--ro-bind";
    arguments[count++] = (char *)path;
    arguments[count++] = (char *)path;
  }
  return count;
}

static int execute_sandboxed(char *const command[], size_t command_count,
                             const char *stage, const char *input_path,
                             bool submit, const char *completion_token,
                             hol_run_result *result, hol_error *error) {
  char bwrap[4096];
  if (resolve_bwrap(bwrap, error) < 0) return -1;
  size_t capacity = command_count + 64U;
  char **arguments = calloc(capacity, sizeof(*arguments));
  if (arguments == NULL) return -1;
  size_t count = 0U;
  arguments[count++] = bwrap;
  arguments[count++] = "--die-with-parent";
  arguments[count++] = "--new-session";
  arguments[count++] = "--unshare-all";
  arguments[count++] = "--cap-drop";
  arguments[count++] = "ALL";
  arguments[count++] = "--proc";
  arguments[count++] = "/proc";
  arguments[count++] = "--dev";
  arguments[count++] = "/dev";
  count = add_system_bind(arguments, count, "/usr");
  count = add_system_bind(arguments, count, "/bin");
  count = add_system_bind(arguments, count, "/lib");
  count = add_system_bind(arguments, count, "/lib64");
  count = add_system_bind(arguments, count, "/etc");
  arguments[count++] = "--tmpfs";
  arguments[count++] = "/tmp";
  arguments[count++] = "--tmpfs";
  arguments[count++] = "/home";
  arguments[count++] = "--dir";
  arguments[count++] = "/home/learner";
  arguments[count++] = "--bind";
  arguments[count++] = (char *)stage;
  arguments[count++] = "/workspace";
  arguments[count++] = "--remount-ro";
  arguments[count++] = "/";
  arguments[count++] = "--chdir";
  arguments[count++] = "/workspace";
  arguments[count++] = "--clearenv";
  arguments[count++] = "--setenv";
  arguments[count++] = "PATH";
  arguments[count++] = "/usr/bin:/bin";
  arguments[count++] = "--setenv";
  arguments[count++] = "HOME";
  arguments[count++] = "/home/learner";
  arguments[count++] = "--setenv";
  arguments[count++] = "TMPDIR";
  arguments[count++] = "/tmp";
  arguments[count++] = "--setenv";
  arguments[count++] = "HOL_SUBMIT";
  arguments[count++] = submit ? "1" : "0";
  if (completion_token != NULL) {
    arguments[count++] = "--setenv";
    arguments[count++] = "HOL_TEST_TOKEN";
    arguments[count++] = (char *)completion_token;
  }
  arguments[count++] = "--";
  arguments[count++] = "/usr/bin/prlimit";
  arguments[count++] = "--nproc=32:32";
  arguments[count++] = "--";
  for (size_t index = 0U; index < command_count; index++)
    arguments[count++] = command[index];
  arguments[count] = NULL;
  if (count >= capacity) {
    free(arguments);
    return -1;
  }

  int standard_output[2] = {-1, -1};
  int standard_error[2] = {-1, -1};
  if (pipe(standard_output) < 0 || pipe(standard_error) < 0) {
    if (standard_output[0] >= 0) {
      (void)close(standard_output[0]);
      (void)close(standard_output[1]);
    }
    free(arguments);
    hol_error_set(error, HOL_ERR_PROCESS, "cannot create process pipes");
    return -1;
  }
  pid_t process = fork();
  if (process < 0) {
    (void)close(standard_output[0]);
    (void)close(standard_output[1]);
    (void)close(standard_error[0]);
    (void)close(standard_error[1]);
    free(arguments);
    hol_error_set(error, HOL_ERR_PROCESS, "cannot fork sandbox process");
    return -1;
  }
  if (process == 0) {
    (void)setpgid(0, 0);
    (void)close(standard_output[0]);
    (void)close(standard_error[0]);
    int input = input_path != NULL
      ? open(input_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
    if ((input_path != NULL && input < 0) ||
        (input >= 0 && dup2(input, STDIN_FILENO) < 0) ||
        dup2(standard_output[1], STDOUT_FILENO) < 0 ||
        dup2(standard_error[1], STDERR_FILENO) < 0 ||
        close(standard_output[1]) < 0 || close(standard_error[1]) < 0)
      _exit(126);
    if (input >= 0) (void)close(input);
    apply_limits();
    if (clearenv() < 0 || setenv("PATH", "/usr/bin:/bin", 1) < 0) _exit(126);
    execv(bwrap, arguments);
    _exit(errno == ENOENT ? 127 : 126);
  }
  free(arguments);

  (void)setpgid(process, process);
  (void)close(standard_output[1]);
  (void)close(standard_error[1]);
  if (set_nonblocking(standard_output[0]) < 0 ||
      set_nonblocking(standard_error[0]) < 0) {
    (void)kill(-process, SIGKILL);
    (void)waitpid(process, NULL, 0);
    (void)close(standard_output[0]);
    (void)close(standard_error[0]);
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
    (void)close(standard_output[0]);
    (void)close(standard_error[0]);
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
      if (waited == process) {
        exited = true;
        if ((!output.closed || !errors.closed) && !terminated) {
          terminated = true;
          terminated_at = monotonic_ms();
          (void)kill(-process, SIGTERM);
        }
      }
    }
    int64_t elapsed = monotonic_ms() - started;
    if (!terminated && elapsed >= effective_timeout_ms()) {
      result->timed_out = true;
      terminated = true;
      terminated_at = monotonic_ms();
      (void)kill(-process, SIGTERM);
    }
    int64_t termination_elapsed = monotonic_ms() - terminated_at;
    if (terminated && termination_elapsed >= 250) {
      (void)kill(-process, SIGKILL);
      if (!exited) (void)kill(process, SIGKILL);
    }
    if (terminated && termination_elapsed >= 500) {
      if (!output.closed) {
        (void)close(standard_output[0]);
        standard_output[0] = -1;
        output.closed = true;
      }
      if (!errors.closed) {
        (void)close(standard_error[0]);
        standard_error[0] = -1;
        errors.closed = true;
      }
    }
  }
  if (standard_output[0] >= 0) (void)close(standard_output[0]);
  if (standard_error[0] >= 0) (void)close(standard_error[0]);
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

static int cleanup_stage(const char *path, hol_error *error) {
  static const char prefix[] = "/tmp/hol-stage-";
  size_t prefix_length = sizeof(prefix) - 1U;
  if (strncmp(path, prefix, prefix_length) != 0 ||
      strlen(path) != prefix_length + 6U || strchr(path + prefix_length, '/') != NULL)
    goto failure;
  pid_t process = fork();
  if (process < 0) goto failure;
  if (process == 0) {
    char *arguments[] = {"rm", "-rf", "--", (char *)path, NULL};
    execv("/usr/bin/rm", arguments);
    _exit(127);
  }
  int status = 0;
  while (waitpid(process, &status, 0) < 0) {
    if (errno != EINTR) goto failure;
  }
  struct stat remaining;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
      lstat(path, &remaining) < 0 && errno == ENOENT) return 0;

failure:
  hol_error_set(error, HOL_ERR_IO, "cannot clean private exercise stage");
  return -1;
}

static bool file_applies(const hol_course_file *file, hol_run_mode mode) {
  hol_file_phase phase = mode == HOL_RUN ? HOL_PHASE_RUN : HOL_PHASE_CHECK;
  return (file->phase & phase) != 0;
}

static int copy_stage_file(const char *source, const char *target,
                           size_t *stage_bytes, hol_error *error) {
  struct stat status;
  if (lstat(source, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_nlink != 1 || status.st_size < 0 ||
      (uintmax_t)status.st_size > 16U * HOL_OUTPUT_MAX ||
      (uintmax_t)status.st_size > HOL_STAGE_MAX_BYTES - *stage_bytes) {
    hol_error_set(error, HOL_ERR_PATH, "invalid exercise input file");
    return -1;
  }
  size_t length = 0U;
  char *content = hol_read_text(source, 16U * HOL_OUTPUT_MAX, &length, error);
  if (content == NULL) return -1;
  int result = hol_atomic_write(target, content, length, error);
  free(content);
  if (result == 0) *stage_bytes += length;
  return result;
}

static int create_stage(const hol_course *course, const hol_lesson *lesson,
                        const char *workspace, hol_run_mode mode,
                        char stage[4096], hol_error *error) {
  (void)snprintf(stage, 4096, "/tmp/hol-stage-XXXXXX");
  if (mkdtemp(stage) == NULL) {
    hol_error_set(error, HOL_ERR_IO, "cannot create private exercise stage");
    return -1;
  }
  size_t stage_bytes = 0U;
  for (size_t index = 0U; index < lesson->file_count; index++) {
    const hol_course_file *file = &lesson->files[index];
    if (!file_applies(file, mode)) continue;
    char source[4096];
    char target[4096];
    const char *root = file->role == HOL_FILE_EDITABLE ? workspace : course->root;
    const char *relative = file->role == HOL_FILE_EDITABLE ? file->target : file->source;
    if (hol_join_path(source, sizeof(source), root, relative, error) < 0 ||
        hol_join_path(target, sizeof(target), stage, file->target, error) < 0)
      goto failure;
    if (file->role != HOL_FILE_EDITABLE) {
      char actual[65];
      if (hol_sha256_file(source, actual, error) < 0) goto failure;
      if (strcmp(actual, file->sha256) != 0) {
        hol_error_set(error, HOL_ERR_CHECKSUM,
                      "exercise resource checksum mismatch: %s", file->source);
        goto failure;
      }
    }
    if (copy_stage_file(source, target, &stage_bytes, error) < 0) goto failure;
    if (file->role != HOL_FILE_EDITABLE) {
      char staged[65];
      if (hol_sha256_file(target, staged, error) < 0 ||
          strcmp(staged, file->sha256) != 0) {
        hol_error_set(error, HOL_ERR_CHECKSUM,
                      "staged exercise resource checksum mismatch: %s", file->source);
        goto failure;
      }
    }
  }
  return 0;

failure:
  if (cleanup_stage(stage, error) < 0) return -1;
  stage[0] = '\0';
  return -1;
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

static int random_completion_token(char token[33], hol_error *error) {
  unsigned char bytes[16];
  int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) goto failure;
  size_t offset = 0U;
  while (offset < sizeof(bytes)) {
    ssize_t count = read(descriptor, bytes + offset, sizeof(bytes) - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      (void)close(descriptor);
      goto failure;
    }
    offset += (size_t)count;
  }
  if (close(descriptor) < 0) goto failure;
  static const char digits[] = "0123456789abcdef";
  for (size_t index = 0U; index < sizeof(bytes); index++) {
    token[index * 2U] = digits[bytes[index] >> 4U];
    token[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
  }
  token[32] = '\0';
  return 0;

failure:
  hol_error_set(error, HOL_ERR_IO, "cannot create test completion token");
  return -1;
}

static bool consume_completion_token(char *output, const char *token) {
  char marker[64];
  int written = snprintf(marker, sizeof(marker), "HOL_TEST_COMPLETE:%s", token);
  if (written < 0 || (size_t)written >= sizeof(marker)) return false;
  size_t length = strlen(output);
  while (length > 0U && (output[length - 1U] == '\n' || output[length - 1U] == '\r'))
    length--;
  size_t line = length;
  while (line > 0U && output[line - 1U] != '\n') line--;
  if (length - line != (size_t)written ||
      strncmp(output + line, marker, (size_t)written) != 0) return false;
  if (line > 0U) line--;
  output[line] = '\0';
  return true;
}

static bool course_contains_lesson(const hol_course *course,
                                   const hol_lesson *lesson) {
  for (size_t chapter = 0U; chapter < course->chapter_count; chapter++)
    for (size_t index = 0U; index < course->chapters[chapter].lesson_count; index++)
      if (&course->chapters[chapter].lessons[index] == lesson) return true;
  return false;
}

static int compare_targets(const void *left, const void *right) {
  const hol_course_file *const *first = left;
  const hol_course_file *const *second = right;
  return strcmp((*first)->target, (*second)->target);
}

static int run_sql(const hol_lesson *lesson, const char *workspace,
                   hol_run_mode mode, const char *completion_token,
                   hol_run_result *result,
                   hol_error *error) {
  hol_course_file **files = calloc(lesson->file_count, sizeof(*files));
  if (files == NULL) return -1;
  size_t file_count = 0U;
  for (size_t index = 0U; index < lesson->file_count; index++)
    if (ends_with(lesson->files[index].target, ".sql")) files[file_count++] = &lesson->files[index];
  qsort(files, file_count, sizeof(*files), compare_targets);
  const size_t maximum_sql = 16U * HOL_OUTPUT_MAX;
  char *sql = calloc(maximum_sql + 1U, 1U);
  if (sql == NULL) {
    free(files);
    return -1;
  }
  size_t length = 0U;
  for (size_t index = 0U; index < file_count; index++) {
    if (!file_applies(files[index], mode)) continue;
    char path[4096];
    if (hol_join_path(path, sizeof(path), workspace, files[index]->target, error) < 0) {
      free(sql);
      free(files);
      return -1;
    }
    if (length >= maximum_sql) {
      hol_error_set(error, HOL_ERR_SCHEMA, "SQL lesson input is too large");
      free(sql);
      free(files);
      return -1;
    }
    size_t available = maximum_sql - length - 1U;
    size_t file_length = 0U;
    char *content = hol_read_text(path, available, &file_length, error);
    if (content == NULL || file_length > available) {
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
  if (completion_token != NULL) {
    int marker_length = snprintf(sql + length, maximum_sql - length,
                                 ".print HOL_TEST_COMPLETE:%s\n",
                                 completion_token);
    if (marker_length < 0 || (size_t)marker_length >= maximum_sql - length) {
      free(sql);
      hol_error_set(error, HOL_ERR_SCHEMA, "SQL lesson input is too large");
      return -1;
    }
    length += (size_t)marker_length;
  }
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
    "sqlite3", "-batch", "-bail", "-header", "-column",
    "/workspace/.hol.sqlite3",
  };
  int status = execute_sandboxed(arguments, sizeof(arguments) / sizeof(arguments[0]),
                                 workspace, input, mode == HOL_CHECK, completion_token,
                                 result, error);
  int input_status = unlink(input);
  int input_errno = errno;
  int database_status = unlink(database);
  int database_errno = errno;
  if ((input_status < 0 && input_errno != ENOENT) ||
      (database_status < 0 && database_errno != ENOENT)) {
    hol_run_result_free(result);
    hol_error_set(error, HOL_ERR_IO, "cannot clean SQL runner files");
    return -1;
  }
  if (status == 0)
    result->passed = result->exit_code == 0 && !result->timed_out &&
                     !result->stdout_truncated && !result->stderr_truncated;
  return status;
}

int hol_runner_execute(const hol_course *course, const hol_lesson *lesson,
                       const char *workspace, hol_run_mode mode,
                       hol_run_result *result, hol_error *error) {
  if (lesson == NULL || workspace == NULL || result == NULL ||
      (mode != HOL_RUN && mode != HOL_CHECK)) {
    hol_error_set(error, HOL_ERR_ARGUMENT, "invalid runner arguments");
    return -1;
  }
  memset(result, 0, sizeof(*result));
  bool c_runner = strcmp(lesson->runner.id, "c") == 0;
  bool sql_runner = strcmp(lesson->runner.id, "sql") == 0;
  if (course == NULL || (!c_runner && !sql_runner) ||
      !course->has_exercise_profile || lesson->kind != HOL_LESSON_EXERCISE ||
      !course_contains_lesson(course, lesson)) {
    hol_error_set(error, HOL_ERR_UNSUPPORTED,
                   "lesson has no verified supported exercise profile");
    return -1;
  }
  char bwrap[4096];
  if (resolve_bwrap(bwrap, error) < 0) return -1;
  char token[33] = {0};
  const char *completion_token = NULL;
  if (mode == HOL_CHECK && lesson->runner.check_kind == HOL_CHECK_TESTS) {
    if (random_completion_token(token, error) < 0) return -1;
    completion_token = token;
  }
  char stage[4096] = {0};
  if (create_stage(course, lesson, workspace, mode, stage, error) < 0) return -1;
  if (sql_runner) {
    int status = run_sql(lesson, stage, mode, completion_token, result, error);
    if (cleanup_stage(stage, error) < 0) {
      hol_run_result_free(result);
      return -1;
    }
    if (status < 0) return -1;
    if (completion_token != NULL)
      result->passed = result->passed &&
        consume_completion_token(result->stdout_data, completion_token);
    return 0;
  }

  size_t capacity = lesson->file_count + 16U;
  char **compiler = calloc(capacity, sizeof(*compiler));
  char **source_paths = calloc(lesson->file_count, sizeof(*source_paths));
  if (compiler == NULL || source_paths == NULL) {
    free(compiler);
    free(source_paths);
    hol_error_set(error, HOL_ERR_IO, "out of memory preparing C exercise");
    (void)cleanup_stage(stage, error);
    return -1;
  }
  size_t count = 0U;
  size_t source_count = 0U;
  compiler[count++] = "gcc";
  compiler[count++] = "-O0";
  compiler[count++] = "-g";
  compiler[count++] = "-Wall";
  compiler[count++] = "-Wextra";
  if (strcmp(lesson->runner.profile, "c11-32") == 0) compiler[count++] = "-m32";
  compiler[count++] = strcmp(lesson->runner.profile, "c23") == 0 ? "-std=c23" : "-std=c11";
  for (size_t index = 0U; index < lesson->file_count; index++) {
    if (!file_applies(&lesson->files[index], mode) ||
        !ends_with(lesson->files[index].target, ".c")) continue;
    size_t length = strlen(lesson->files[index].target);
    source_paths[index] = malloc(length + 3U);
    if (source_paths[index] == NULL) goto allocation_failure;
    (void)snprintf(source_paths[index], length + 3U, "./%s",
                   lesson->files[index].target);
    compiler[count++] = source_paths[index];
    source_count++;
  }
  if (source_count == 0U) {
    hol_error_set(error, HOL_ERR_SCHEMA, "C lesson has no source files for this action");
    goto allocation_failure;
  }
  if (strcmp(lesson->runner.profile, "c11-32") == 0) {
    compiler[count++] = "-Wl,--wrap=malloc";
    compiler[count++] = "-Wl,--wrap=calloc";
    compiler[count++] = "-Wl,--wrap=realloc";
    compiler[count++] = "-Wl,--wrap=free";
  }
  compiler[count++] = "-lm";
  compiler[count++] = "-o";
  compiler[count++] = "/workspace/.hol-program";
  compiler[count] = NULL;

  hol_run_result compilation = {0};
  int status = execute_sandboxed(compiler, count, stage, NULL, false, NULL,
                                 &compilation, error);
  for (size_t index = 0U; index < lesson->file_count; index++) free(source_paths[index]);
  free(source_paths);
  free(compiler);
  if (status < 0) {
    (void)cleanup_stage(stage, error);
    return -1;
  }
  if (compilation.exit_code != 0 || compilation.timed_out ||
      compilation.stdout_truncated || compilation.stderr_truncated) {
    *result = compilation;
    if (cleanup_stage(stage, error) < 0) {
      hol_run_result_free(result);
      return -1;
    }
    return 0;
  }
  hol_run_result_free(&compilation);
  char *execution[] = {"/workspace/.hol-program"};
  status = execute_sandboxed(execution, 1U, stage, NULL,
                             mode == HOL_CHECK, completion_token, result, error);
  if (cleanup_stage(stage, error) < 0) {
    hol_run_result_free(result);
    return -1;
  }
  if (status < 0) return -1;
  result->passed = result->exit_code == 0 && !result->timed_out &&
                   !result->stdout_truncated && !result->stderr_truncated;
  if (completion_token != NULL)
    result->passed = result->passed &&
      consume_completion_token(result->stdout_data, completion_token);
  if (mode == HOL_CHECK && lesson->runner.check_kind == HOL_CHECK_STDOUT &&
      lesson->runner.expected_output != NULL) {
    char *expected = strdup(lesson->runner.expected_output);
    if (expected == NULL) {
      hol_run_result_free(result);
      hol_error_set(error, HOL_ERR_IO, "out of memory checking C exercise");
      return -1;
    }
    trim_trailing_newlines(result->stdout_data);
    trim_trailing_newlines(expected);
    result->passed = result->passed && strcmp(result->stdout_data, expected) == 0;
    free(expected);
  }
  return 0;

allocation_failure:
  if (error != NULL && error->message[0] == '\0')
    hol_error_set(error, HOL_ERR_IO, "out of memory preparing C exercise");
  for (size_t index = 0U; index < lesson->file_count; index++) free(source_paths[index]);
  free(source_paths);
  free(compiler);
  (void)cleanup_stage(stage, error);
  return -1;
}

void hol_run_result_free(hol_run_result *result) {
  if (result == NULL) return;
  free(result->stdout_data);
  free(result->stderr_data);
  memset(result, 0, sizeof(*result));
}
