#include "hol.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int hol_workspace_ensure(const hol_course *course, const hol_lesson *lesson,
                         const char *workspace, hol_error *error) {
  if (mkdir(workspace, 0700) < 0 && errno != EEXIST) {
    hol_error_set(error, HOL_ERR_IO, "cannot create lesson workspace");
    return -1;
  }
  struct stat status;
  if (lstat(workspace, &status) < 0 || !S_ISDIR(status.st_mode)) {
    hol_error_set(error, HOL_ERR_PATH, "lesson workspace is not a directory");
    return -1;
  }
  for (size_t index = 0U; index < lesson->file_count; index++) {
    char source[4096];
    char target[4096];
    if (hol_join_path(source, sizeof(source), course->root, lesson->files[index].source,
                      error) < 0 ||
        hol_join_path(target, sizeof(target), workspace, lesson->files[index].target,
                      error) < 0 ||
        hol_copy_file_if_missing(source, target, error) < 0) return -1;
  }
  return 0;
}

static int remove_tree(const char *path, hol_error *error) {
  struct stat root_status;
  if (lstat(path, &root_status) < 0) {
    if (errno == ENOENT) return 0;
    hol_error_set(error, HOL_ERR_IO, "cannot inspect workspace for reset");
    return -1;
  }
  if (!S_ISDIR(root_status.st_mode)) {
    hol_error_set(error, HOL_ERR_PATH, "lesson workspace is not a directory");
    return -1;
  }
  DIR *directory = opendir(path);
  if (directory == NULL) {
    hol_error_set(error, HOL_ERR_IO, "cannot open workspace for reset");
    return -1;
  }
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char child[4096];
    int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(child)) {
      (void)closedir(directory);
      return -1;
    }
    struct stat status;
    if (lstat(child, &status) < 0) {
      (void)closedir(directory);
      return -1;
    }
    if (S_ISDIR(status.st_mode)) {
      if (remove_tree(child, error) < 0) {
        (void)closedir(directory);
        return -1;
      }
    } else if (unlink(child) < 0) {
      (void)closedir(directory);
      return -1;
    }
  }
  (void)closedir(directory);
  if (rmdir(path) < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot remove lesson workspace");
    return -1;
  }
  return 0;
}

int hol_workspace_reset(const hol_course *course, const hol_lesson *lesson,
                        const char *workspace, hol_error *error) {
  if (remove_tree(workspace, error) < 0) return -1;
  return hol_workspace_ensure(course, lesson, workspace, error);
}
