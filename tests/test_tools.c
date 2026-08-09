#include "hol.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  char root[] = "/tmp/hol-tools-XXXXXX";
  assert(mkdtemp(root) != NULL);
  hol_error error = {0};
  char editor[4096];
  char target[4096];
  assert(hol_join_path(editor, sizeof(editor), root, "editor", &error) == 0);
  assert(hol_join_path(target, sizeof(target), root, "edited.txt", &error) == 0);
  const char script[] = "#!/bin/sh\nprintf edited > \"$2\"\n";
  assert(hol_atomic_write(editor, script, strlen(script), &error) == 0);
  assert(chmod(editor, 0700) == 0);
  assert(setenv("VISUAL", editor, 1) == 0);
  assert(hol_launch_editor(target, &error) == 0);
  size_t length = 0U;
  char *content = hol_read_text(target, 64U, &length, &error);
  assert(content != NULL && strcmp(content, "edited") == 0);
  free(content);

  assert(setenv("VISUAL", "nvim --clean", 1) == 0);
  memset(&error, 0, sizeof(error));
  assert(hol_launch_editor(target, &error) < 0);
  assert(error.code == HOL_ERR_ARGUMENT);

  assert(unlink(target) == 0);
  assert(unlink(editor) == 0);
  assert(rmdir(root) == 0);
  puts("tool tests passed");
  return 0;
}
