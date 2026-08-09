#include "hol.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_for_tool(char *const arguments[], hol_error *error) {
  pid_t process = fork();
  if (process < 0) {
    hol_error_set(error, HOL_ERR_PROCESS, "cannot fork external tool");
    return -1;
  }
  if (process == 0) {
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGTERM, SIG_DFL);
    execvp(arguments[0], arguments);
    _exit(errno == ENOENT ? 127 : 126);
  }
  int status = 0;
  while (waitpid(process, &status, 0) < 0) {
    if (errno == EINTR) continue;
    hol_error_set(error, HOL_ERR_PROCESS, "cannot wait for external tool");
    return -1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    hol_error_set(error, HOL_ERR_PROCESS, "%s exited unsuccessfully", arguments[0]);
    return -1;
  }
  return 0;
}

static const char *editor_command(hol_error *error) {
  const char *editor = getenv("VISUAL");
  if (editor == NULL || editor[0] == '\0') editor = getenv("EDITOR");
  if (editor == NULL || editor[0] == '\0') editor = "nvim";
  if (strpbrk(editor, " \t\r\n") != NULL) {
    hol_error_set(error, HOL_ERR_ARGUMENT,
                  "VISUAL or EDITOR must name one executable without shell arguments");
    return NULL;
  }
  return editor;
}

int hol_launch_editor(const char *path, hol_error *error) {
  if (path == NULL || path[0] == '\0') {
    hol_error_set(error, HOL_ERR_ARGUMENT, "no editable file is selected");
    return -1;
  }
  const char *editor = editor_command(error);
  if (editor == NULL) return -1;
  char *arguments[] = {(char *)editor, "--", (char *)path, NULL};
  return wait_for_tool(arguments, error);
}

static bool media_extension(const char *path, const char *extension) {
  size_t path_length = strlen(path);
  size_t extension_length = strlen(extension);
  return path_length >= extension_length &&
         strcasecmp(path + path_length - extension_length, extension) == 0;
}

int hol_launch_media(const char *path, hol_error *error) {
  if (path == NULL || path[0] == '\0') {
    hol_error_set(error, HOL_ERR_ARGUMENT, "no lesson media is selected");
    return -1;
  }
  bool playable = media_extension(path, ".mp3") || media_extension(path, ".wav") ||
                  media_extension(path, ".ogg") || media_extension(path, ".m4a") ||
                  media_extension(path, ".mp4") || media_extension(path, ".webm") ||
                  media_extension(path, ".mov");
  char *arguments[] = {
    playable ? "mpv" : "xdg-open",
    playable ? "--no-config" : "--",
    (char *)path,
    NULL,
  };
  return wait_for_tool(arguments, error);
}
