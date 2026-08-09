#include "hol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
  assert(snprintf(data, sizeof(data), "%s/data", home) > 0);
  assert(snprintf(state, sizeof(state), "%s/state", home) > 0);
  assert(setenv("HOME", home, 1) == 0);
  assert(setenv("TERM", "xterm-256color", 1) == 0);
  assert(setenv("XDG_DATA_HOME", data, 1) == 0);
  assert(setenv("XDG_STATE_HOME", state, 1) == 0);
  execl("build/hands-on-learning", "build/hands-on-learning", "--course",
        "courses/demo.holcourse", (char *)NULL);
  _exit(127);
}

int main(void) {
  char home[] = "/tmp/hol-ui-XXXXXX";
  assert(mkdtemp(home) != NULL);
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
  read_until(master, "KEYS");
  write_all(master, "\033");
  size = (struct winsize){.ws_row = 40, .ws_col = 120};
  assert(ioctl(master, TIOCSWINSZ, &size) == 0);
  assert(kill(process, SIGWINCH) == 0);
  write_all(master, " lc");
  read_until(master, "COURSES");
  write_all(master, "\033 ll/Header\r\r");
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
  hol_error error = {0};
  char *state = hol_read_text(state_path, 64U * 1024U, NULL, &error);
  assert(state != NULL);
  assert(strstr(state, "\"course_id\":\"hol.demo-c\"") != NULL);
  assert(strstr(state, "\"lesson_id\":\"header-quiz\"") != NULL);
  free(state);
  char command[8192];
  assert(snprintf(command, sizeof(command), "rm -rf -- '%s'", home) > 0);
  assert(system(command) == 0);
  puts("UI PTY tests passed");
  return 0;
}
