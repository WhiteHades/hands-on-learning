#include "hol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static void write_all(int descriptor, const char *text) {
  size_t length = strlen(text);
  size_t offset = 0U;
  while (offset < length) {
    ssize_t count = write(descriptor, text + offset, length - offset);
    if (count < 0 && errno == EINTR) continue;
    assert(count > 0);
    offset += (size_t)count;
  }
}

static void read_until(int descriptor, const char *expected) {
  char output[256U * 1024U] = {0};
  size_t length = 0U;
  int64_t deadline = monotonic_ms() + 3000;
  while (monotonic_ms() < deadline) {
    struct pollfd item = {.fd = descriptor, .events = POLLIN};
    int ready = poll(&item, 1U, 50);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) continue;
    ssize_t count = read(descriptor, output + length, sizeof(output) - length - 1U);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    length += (size_t)count;
    output[length] = '\0';
    if (strstr(output, expected) != NULL) return;
    if (length + 1U == sizeof(output)) length = 0U;
  }
  assert(!"expected text was not rendered");
}

static pid_t launch_ui(int master, const char *slave_name, const char *home) {
  pid_t process = fork();
  assert(process >= 0);
  if (process != 0) return process;
  assert(setsid() >= 0);
  int slave = open(slave_name, O_RDWR);
  assert(slave >= 0);
  assert(ioctl(slave, TIOCSCTTY, 0) == 0);
  assert(dup2(slave, STDIN_FILENO) >= 0);
  assert(dup2(slave, STDOUT_FILENO) >= 0);
  assert(dup2(slave, STDERR_FILENO) >= 0);
  if (slave > STDERR_FILENO) (void)close(slave);
  (void)close(master);
  char data[4096];
  char state[4096];
  char path[4096];
  char editor[4096];
  assert(snprintf(data, sizeof(data), "%s/data", home) > 0);
  assert(snprintf(state, sizeof(state), "%s/state", home) > 0);
  assert(snprintf(path, sizeof(path), "%s/bin", home) > 0);
  assert(snprintf(editor, sizeof(editor), "%s/fake-editor", home) > 0);
  assert(setenv("HOME", home, 1) == 0);
  assert(setenv("TERM", "xterm-256color", 1) == 0);
  assert(setenv("XDG_DATA_HOME", data, 1) == 0);
  assert(setenv("XDG_STATE_HOME", state, 1) == 0);
  assert(setenv("PATH", path, 1) == 0);
  assert(setenv("EDITOR", editor, 1) == 0);
  execl("build/hands-on-learning", "build/hands-on-learning", "--course",
        "build/test-course.imscc", "--exercise-profile",
        "build/test-course.profile.json", (char *)NULL);
  _exit(127);
}

int main(void) {
  char home[] = "/tmp/hol-ui-XXXXXX";
  assert(mkdtemp(home) != NULL);
  char command[16384];
  assert(snprintf(command, sizeof(command),
                  "mkdir -p '%s/bin' '%s/data/hands-on-learning/courses' && "
                  "cp build/test-course.imscc '%s/data/hands-on-learning/courses/managed.imscc' && "
                  "cp build/test-course.profile.json '%s/data/hands-on-learning/courses/managed.profile.json'",
                  home, home, home, home) > 0);
  assert(system(command) == 0);
  char editor_path[4096];
  assert(snprintf(editor_path, sizeof(editor_path), "%s/fake-editor", home) > 0);
  static const char editor_script[] =
    "#!/bin/sh\n"
    "printf '%s\\n' 'const char *lesson_greeting(void) { return \"Hello, learner!\"; }' > \"$2\"\n";
  hol_error error = {0};
  assert(hol_atomic_write(editor_path, editor_script, strlen(editor_script), &error) == 0);
  assert(chmod(editor_path, 0700) == 0);
  char bwrap_path[4096];
  assert(snprintf(bwrap_path, sizeof(bwrap_path), "%s/bin/bwrap", home) > 0);
  assert(symlink("/usr/bin/bwrap", bwrap_path) == 0);
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  assert(master >= 0);
  assert(grantpt(master) == 0);
  assert(unlockpt(master) == 0);
  char *slave_name = ptsname(master);
  assert(slave_name != NULL);
  pid_t process = launch_ui(master, slave_name, home);
  struct winsize size = {.ws_row = 24, .ws_col = 80};
  assert(ioctl(master, TIOCSWINSZ, &size) == 0);

  read_until(master, "Hands-on Learning");
  write_all(master, "?");
  read_until(master, "Reset");
  write_all(master, "\033");
  size = (struct winsize){.ws_row = 40, .ws_col = 120};
  assert(ioctl(master, TIOCSWINSZ, &size) == 0);
  assert(kill(process, SIGWINCH) == 0);
  write_all(master, " ll/C Greeting\r\r");
  read_until(master, "return \"TODO\"");
  write_all(master, "e");
  read_until(master, "Hello, learner!");
  write_all(master, " r");
  read_until(master, "RUN FINISHED");
  char exercise_path[4096];
  assert(snprintf(exercise_path, sizeof(exercise_path),
                  "%s/data/hands-on-learning/workspaces/test.course/c-greeting/greeting.c",
                  home) > 0);
  static const char wrong[] =
    "const char *lesson_greeting(void) { return \"Wrong\"; }\n";
  static const char correct[] =
    "const char *lesson_greeting(void) { return \"Hello, learner!\"; }\n";
  assert(hol_atomic_write(exercise_path, wrong, strlen(wrong), &error) == 0);
  write_all(master, " t");
  read_until(master, "lesson_greeting returned the wrong text.");
  assert(hol_atomic_write(exercise_path, correct, strlen(correct), &error) == 0);
  write_all(master, " t");
  read_until(master, "CHECK PASSED");
  write_all(master, " x");
  read_until(master, "Reset this lesson?");
  write_all(master, "y");
  read_until(master, "Lesson workspace reset.");
  char *starter = hol_read_text(exercise_path, HOL_OUTPUT_MAX, NULL, &error);
  assert(starter != NULL && strstr(starter, "TODO") != NULL);
  free(starter);
  assert(unlink(bwrap_path) == 0);
  write_all(master, " r");
  read_until(master, "Bubblewrap (bwrap) is required");
  write_all(master, " lc");
  read_until(master, "COURSES");
  write_all(master, "j\r");
  read_until(master, "Opened content-only");
  write_all(master, " ll/C Greeting\r\r");
  read_until(master, "Change lesson_greeting to return Hello, learner!");
  write_all(master, " r");
  read_until(master, "This lesson has no runnable workspace.");
  write_all(master, " ll/Knowledge\r\r");
  struct timespec pause = {.tv_nsec = 200000000L};
  (void)nanosleep(&pause, NULL);
  write_all(master, "\033");
  pause.tv_nsec = 100000000L;
  (void)nanosleep(&pause, NULL);
  write_all(master, "q");

  int status = 0;
  int64_t deadline = monotonic_ms() + 3000;
  for (;;) {
    pid_t waited = waitpid(process, &status, WNOHANG);
    if (waited == process) break;
    assert(waited == 0);
    if (monotonic_ms() >= deadline) {
      (void)kill(process, SIGKILL);
      (void)waitpid(process, NULL, 0);
      assert(!"UI did not exit after q");
    }
    struct pollfd item = {.fd = master, .events = POLLIN};
    if (poll(&item, 1U, 50) > 0) {
      char discard[65536];
      ssize_t discarded = read(master, discard, sizeof(discard));
      (void)discarded;
    }
  }
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  (void)close(master);

  char state_path[4096];
  assert(snprintf(state_path, sizeof(state_path),
                  "%s/state/hands-on-learning/state.json", home) > 0);
  char *state = hol_read_text(state_path, 64U * 1024U, NULL, &error);
  assert(state != NULL);
  assert(strstr(state, "\"course_id\":\"test.course\"") != NULL);
  assert(strstr(state, "\"lesson_id\":\"knowledge-check\"") != NULL);
  free(state);
  hol_state loaded = {0};
  assert(hol_state_load(state_path, &loaded, &error) == 0);
  assert(hol_state_completed(&loaded, "test.course", "c-greeting"));
  hol_state_free(&loaded);
  assert(snprintf(command, sizeof(command), "rm -rf -- '%s'", home) > 0);
  assert(system(command) == 0);
  puts("UI PTY tests passed");
  return 0;
}
