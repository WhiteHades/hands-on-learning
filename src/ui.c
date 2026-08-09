#include "hol.h"

#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
  PAIR_TEXT = 1,
  PAIR_MUTED,
  PAIR_ACCENT,
  PAIR_SUCCESS,
  PAIR_WARNING,
  PAIR_ERROR,
  PAIR_HEADER,
  PAIR_SELECTED,
};

typedef enum {
  POPUP_NONE,
  POPUP_LESSONS,
  POPUP_COURSES,
  POPUP_HELP
} popup_kind;

typedef struct {
  hol_course *course;
  hol_state state;
  hol_key_state keys;
  size_t lesson_index;
  size_t file_index;
  size_t quiz_question;
  size_t quiz_choice;
  size_t media_index;
  bool *quiz_correct;
  size_t reader_scroll;
  size_t preview_scroll;
  size_t output_scroll;
  int pane;
  popup_kind popup;
  size_t popup_cursor;
  size_t popup_scroll;
  char query[128];
  size_t query_length;
  bool search_active;
  bool reset_pending;
  char state_path[4096];
  char workspace[4096];
  char *lesson_text;
  char *preview_text;
  char *output;
  char **course_roots;
  char **course_profiles;
  char **course_titles;
  size_t course_count;
  size_t active_course;
} ui_session;

static int64_t now_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
  return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static int mkdirs(const char *path, hol_error *error) {
  char copy[4096];
  int length = snprintf(copy, sizeof(copy), "%s", path);
  if (length < 0 || (size_t)length >= sizeof(copy)) return -1;
  for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(copy, 0700) < 0 && errno != EEXIST) {
      hol_error_set(error, HOL_ERR_IO, "cannot create %s", copy);
      return -1;
    }
    *cursor = '/';
  }
  if (mkdir(copy, 0700) < 0 && errno != EEXIST) return -1;
  return 0;
}

static int data_path(char *output, size_t size, const char *environment,
                     const char *fallback, const char *suffix,
                     hol_error *error) {
  const char *base = getenv(environment);
  char default_base[4096];
  if (base == NULL || base[0] == '\0') {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
      hol_error_set(error, HOL_ERR_PATH, "HOME is unavailable");
      return -1;
    }
    int length = snprintf(default_base, sizeof(default_base), "%s/%s", home, fallback);
    if (length < 0 || (size_t)length >= sizeof(default_base)) return -1;
    base = default_base;
  }
  int length = snprintf(output, size, "%s/hands-on-learning/%s", base, suffix);
  if (length < 0 || (size_t)length >= size) return -1;
  return 0;
}

static void set_output(ui_session *session, const char *text) {
  char *replacement = strdup(text != NULL ? text : "");
  if (replacement == NULL) return;
  free(session->output);
  session->output = replacement;
  session->output_scroll = 0U;
}

static void single_line(char *target, size_t capacity, const char *source) {
  size_t index = 0U;
  while (source[index] != '\0' && index + 1U < capacity) {
    char value = source[index];
    target[index] = value == '\n' || value == '\r' ? ' ' : value;
    index++;
  }
  target[index] = '\0';
}

static bool ends_with_text(const char *value, const char *suffix) {
  size_t value_length = strlen(value);
  size_t suffix_length = strlen(suffix);
  return value_length >= suffix_length &&
         strcmp(value + value_length - suffix_length, suffix) == 0;
}

static int add_course(ui_session *session, const char *root, const char *profile,
                      hol_error *error) {
  hol_course *course = NULL;
  int load_status = profile != NULL
    ? hol_course_load_profile(root, profile, &course, error)
    : hol_course_load(root, &course, error);
  if (load_status < 0) return -1;
  for (size_t index = 0U; index < session->course_count; index++) {
    if (strcmp(session->course_roots[index], course->source_path) == 0) {
      hol_course_free(course);
      return 0;
    }
  }
  size_t count = session->course_count + 1U;
  char *new_root = strdup(course->source_path);
  char *new_profile = profile != NULL ? realpath(profile, NULL) : NULL;
  char *new_title = strdup(course->title);
  if (new_root == NULL || (profile != NULL && new_profile == NULL) || new_title == NULL) {
    free(new_root);
    free(new_profile);
    free(new_title);
    hol_course_free(course);
    return -1;
  }
  char **roots = realloc(session->course_roots, count * sizeof(*roots));
  if (roots == NULL) {
    free(new_root);
    free(new_profile);
    free(new_title);
    hol_course_free(course);
    return -1;
  }
  session->course_roots = roots;
  char **profiles = realloc(session->course_profiles, count * sizeof(*profiles));
  if (profiles == NULL) {
    free(new_root);
    free(new_profile);
    free(new_title);
    hol_course_free(course);
    return -1;
  }
  session->course_profiles = profiles;
  char **titles = realloc(session->course_titles, count * sizeof(*titles));
  if (titles == NULL) {
    free(new_root);
    free(new_profile);
    free(new_title);
    hol_course_free(course);
    return -1;
  }
  session->course_titles = titles;
  session->course_roots[session->course_count] = new_root;
  session->course_profiles[session->course_count] = new_profile;
  session->course_titles[session->course_count] = new_title;
  session->course_count = count;
  hol_course_free(course);
  return 0;
}

static void scan_courses(ui_session *session, const char *directory) {
  DIR *entries = opendir(directory);
  if (entries == NULL) return;
  struct dirent *entry;
  while ((entry = readdir(entries)) != NULL) {
    if (!ends_with_text(entry->d_name, ".imscc")) continue;
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    if (length < 0 || (size_t)length >= sizeof(path)) continue;
    hol_error ignored = {0};
    (void)add_course(session, path, NULL, &ignored);
  }
  (void)closedir(entries);
}

static void discover_courses(ui_session *session, const char *profile_path) {
  hol_error ignored = {0};
  (void)add_course(session, session->course->source_path, profile_path, &ignored);
  char installed[4096];
  if (data_path(installed, sizeof(installed), "XDG_DATA_HOME", ".local/share",
                "courses", &ignored) == 0) scan_courses(session, installed);
  char parent[4096];
  (void)snprintf(parent, sizeof(parent), "%s", session->course->source_path);
  char *slash = strrchr(parent, '/');
  if (slash != NULL) {
    *slash = '\0';
    scan_courses(session, parent);
  }
  for (size_t index = 0U; index < session->course_count; index++)
    if (strcmp(session->course_roots[index], session->course->source_path) == 0)
      session->active_course = index;
}

static void free_course_list(ui_session *session) {
  for (size_t index = 0U; index < session->course_count; index++) {
    free(session->course_roots[index]);
    free(session->course_profiles[index]);
    free(session->course_titles[index]);
  }
  free(session->course_roots);
  free(session->course_profiles);
  free(session->course_titles);
}

static int capture_bat(const char *path, char **output) {
  int channel[2];
  if (pipe(channel) < 0) return -1;
  pid_t process = fork();
  if (process < 0) return -1;
  if (process == 0) {
    (void)close(channel[0]);
    if (dup2(channel[1], STDOUT_FILENO) < 0) _exit(126);
    (void)close(channel[1]);
    execlp("bat", "bat", "--color=never", "--style=plain", "--paging=never",
           "--theme=Catppuccin Mocha", "--", path, (char *)NULL);
    _exit(127);
  }
  (void)close(channel[1]);
  char *text = calloc(HOL_OUTPUT_MAX + 1U, 1U);
  if (text == NULL) return -1;
  size_t length = 0U;
  while (length < HOL_OUTPUT_MAX) {
    ssize_t count = read(channel[0], text + length, HOL_OUTPUT_MAX - length);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    length += (size_t)count;
  }
  (void)close(channel[0]);
  int status = 0;
  while (waitpid(process, &status, 0) < 0 && errno == EINTR) {}
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    free(text);
    return -1;
  }
  text[length] = '\0';
  *output = text;
  return 0;
}

static const hol_lesson *current_lesson(const ui_session *session) {
  return hol_course_lesson(session->course, session->lesson_index);
}

static void save_state(ui_session *session) {
  const hol_lesson *lesson = current_lesson(session);
  if (lesson == NULL) return;
  (void)snprintf(session->state.course_id, sizeof(session->state.course_id), "%s",
                 session->course->id);
  (void)snprintf(session->state.lesson_id, sizeof(session->state.lesson_id), "%s",
                 lesson->id);
  session->state.reader_scroll = session->reader_scroll;
  session->state.preview_scroll = session->preview_scroll;
  session->state.output_scroll = session->output_scroll;
  session->state.pane = session->pane;
  if (lesson->file_count > 0U && session->file_index < lesson->file_count)
    (void)snprintf(session->state.file_path, sizeof(session->state.file_path), "%s",
                   lesson->files[session->file_index].target);
  else session->state.file_path[0] = '\0';
  hol_error ignored = {0};
  (void)hol_state_save(session->state_path, &session->state, &ignored);
}

static int load_lesson(ui_session *session, size_t index, hol_error *error) {
  const hol_lesson *lesson = hol_course_lesson(session->course, index);
  if (lesson == NULL) return -1;
  char content_path[4096];
  if (hol_join_path(content_path, sizeof(content_path), session->course->root,
                    lesson->content_path, error) < 0) return -1;
  char *lesson_text = hol_read_text(content_path, HOL_OUTPUT_MAX, NULL, error);
  if (lesson_text == NULL) return -1;
  free(session->lesson_text);
  session->lesson_text = lesson_text;
  session->lesson_index = index;
  session->file_index = 0U;
  session->quiz_question = 0U;
  session->quiz_choice = 0U;
  session->media_index = 0U;
  free(session->quiz_correct);
  session->quiz_correct = NULL;
  if (lesson->question_count > 0U) {
    session->quiz_correct = calloc(lesson->question_count, sizeof(*session->quiz_correct));
    if (session->quiz_correct == NULL) return -1;
  }
  session->reader_scroll = 0U;
  session->preview_scroll = 0U;
  free(session->preview_text);
  session->preview_text = NULL;

  if (lesson->file_count > 0U) {
    char workspaces[4096];
    if (data_path(workspaces, sizeof(workspaces), "XDG_DATA_HOME", ".local/share",
                  "workspaces", error) < 0) return -1;
    int length = snprintf(session->workspace, sizeof(session->workspace), "%s/%s/%s",
                          workspaces, session->course->id, lesson->id);
    if (length < 0 || (size_t)length >= sizeof(session->workspace) ||
        mkdirs(session->workspace, error) < 0 ||
        hol_workspace_ensure(session->course, lesson, session->workspace, error) < 0)
      return -1;
    for (size_t file = 0U; file < lesson->file_count; file++) {
      if (strcmp(lesson->files[file].target, lesson->default_file) == 0) {
        session->file_index = file;
        break;
      }
    }
    char file_path[4096];
    if (hol_join_path(file_path, sizeof(file_path), session->workspace,
                      lesson->files[session->file_index].target, error) < 0) return -1;
    if (capture_bat(file_path, &session->preview_text) < 0)
      session->preview_text = hol_read_text(file_path, HOL_OUTPUT_MAX, NULL, error);
  } else session->workspace[0] = '\0';
  if (lesson->kind == HOL_LESSON_READING)
    (void)hol_state_mark_completed(&session->state, session->course->id,
                                   lesson->id, error);
  else if (lesson->kind == HOL_LESSON_EXERCISE)
    set_output(session,
               "Local checks protect packaged files from accidental edits. "
               "They are not an anti-cheat boundary.");
  save_state(session);
  return 0;
}

static size_t line_count(const char *text) {
  if (text == NULL || text[0] == '\0') return 0U;
  size_t count = 1U;
  for (const char *cursor = text; *cursor != '\0'; cursor++)
    if (*cursor == '\n') count++;
  return count;
}

static void draw_text(WINDOW *window, const char *text, size_t scroll,
                      int color_pair) {
  int height = 0;
  int width = 0;
  getmaxyx(window, height, width);
  if (height <= 2 || width <= 2 || text == NULL) return;
  wattrset(window, COLOR_PAIR(color_pair));
  const char *cursor = text;
  for (size_t line = 0U; line < scroll && *cursor != '\0'; line++) {
    const char *next = strchr(cursor, '\n');
    cursor = next != NULL ? next + 1 : cursor + strlen(cursor);
  }
  for (int row = 1; row < height - 1 && *cursor != '\0'; row++) {
    const char *next = strchr(cursor, '\n');
    size_t length = next != NULL ? (size_t)(next - cursor) : strlen(cursor);
    int visible = width - 3;
    if (visible > 0) {
      int amount = length < (size_t)visible ? (int)length : visible;
      (void)mvwaddnstr(window, row, 1, cursor, amount);
    }
    cursor = next != NULL ? next + 1 : cursor + length;
  }
}

static void draw_border(WINDOW *window, const char *title, bool focused) {
  wattrset(window, COLOR_PAIR(focused ? PAIR_ACCENT : PAIR_MUTED));
  box(window, 0, 0);
  (void)mvwprintw(window, 0, 2, " %s ", title);
}

static void initialize_colors(void) {
  start_color();
  use_default_colors();
  short background = COLOR_BLACK;
  short text = COLOR_WHITE;
  short muted = COLOR_CYAN;
  short accent = COLOR_MAGENTA;
  short green = COLOR_GREEN;
  short yellow = COLOR_YELLOW;
  short red = COLOR_RED;
  short surface = COLOR_BLACK;
  if (COLORS >= 32 && can_change_color()) {
    background = 16;
    text = 17;
    muted = 18;
    accent = 19;
    green = 20;
    yellow = 21;
    red = 22;
    surface = 23;
    (void)init_color(background, 118, 118, 180);
    (void)init_color(text, 804, 839, 957);
    (void)init_color(muted, 651, 678, 784);
    (void)init_color(accent, 796, 651, 969);
    (void)init_color(green, 651, 890, 631);
    (void)init_color(yellow, 976, 886, 686);
    (void)init_color(red, 953, 545, 659);
    (void)init_color(surface, 192, 196, 267);
  } else if (COLORS >= 256) {
    background = 234;
    text = 189;
    muted = 146;
    accent = 183;
    green = 151;
    yellow = 223;
    red = 211;
    surface = 237;
  }
  (void)init_pair(PAIR_TEXT, text, background);
  (void)init_pair(PAIR_MUTED, muted, background);
  (void)init_pair(PAIR_ACCENT, accent, background);
  (void)init_pair(PAIR_SUCCESS, green, background);
  (void)init_pair(PAIR_WARNING, yellow, background);
  (void)init_pair(PAIR_ERROR, red, background);
  (void)init_pair(PAIR_HEADER, text, surface);
  (void)init_pair(PAIR_SELECTED, background, accent);
  bkgdset(COLOR_PAIR(PAIR_TEXT));
}

static bool lesson_matches(const ui_session *session, size_t index) {
  const hol_lesson *lesson = hol_course_lesson(session->course, index);
  return lesson != NULL && (session->query_length == 0U ||
         strcasestr(lesson->title, session->query) != NULL);
}

static bool move_lesson_cursor(ui_session *session, int direction) {
  size_t index = session->popup_cursor;
  while ((direction > 0 && index + 1U < session->course->lesson_count) ||
         (direction < 0 && index > 0U)) {
    index = direction > 0 ? index + 1U : index - 1U;
    if (lesson_matches(session, index)) {
      session->popup_cursor = index;
      return true;
    }
  }
  return false;
}

static void select_lesson_edge(ui_session *session, bool last) {
  if (session->course->lesson_count == 0U) return;
  size_t index = last ? session->course->lesson_count - 1U : 0U;
  for (;;) {
    if (lesson_matches(session, index)) {
      session->popup_cursor = index;
      return;
    }
    if ((!last && index + 1U >= session->course->lesson_count) ||
        (last && index == 0U)) return;
    index = last ? index - 1U : index + 1U;
  }
}

static size_t lesson_cursor_rank(const ui_session *session) {
  size_t rank = 0U;
  for (size_t index = 0U; index < session->popup_cursor; index++)
    if (lesson_matches(session, index)) rank++;
  return rank;
}

static void draw_popup(ui_session *session, int rows, int columns) {
  int width = columns > 90 ? 72 : columns - 4;
  int height = rows > 30 ? 22 : rows - 4;
  if (width < 20 || height < 8) return;
  int start_y = (rows - height) / 2;
  int start_x = (columns - width) / 2;
  WINDOW *popup = newwin(height, width, start_y, start_x);
  if (popup == NULL) return;
  wbkgdset(popup, COLOR_PAIR(PAIR_TEXT));
  const char *title = session->popup == POPUP_LESSONS ? "LESSONS" :
                      session->popup == POPUP_COURSES ? "COURSES" : "KEYS";
  draw_border(popup, title, true);
  if (session->popup == POPUP_HELP) {
    static const char *help =
      "NORMAL\n"
      "h/j/k/l       switch pane or move\n"
      "gg / G        top / bottom\n"
      "Ctrl-d/u      half page down / up\n"
      "/  n  N       search / next / previous\n"
      "Space l l     lesson picker\n"
      "Space l c     course picker\n"
      "e              Edit\n"
      "Space r        Run\n"
      "Space t        Check\n"
      "Space x        Reset\n"
      "Checks protect files; they are not anti-cheat\n"
      "Enter          select or answer\n"
      "Esc / q       close / quit";
    draw_text(popup, help, 0U, PAIR_TEXT);
  } else if (session->popup == POPUP_COURSES) {
    size_t available = (size_t)(height - 4);
    if (session->popup_cursor < session->popup_scroll)
      session->popup_scroll = session->popup_cursor;
    else if (session->popup_cursor >= session->popup_scroll + available)
      session->popup_scroll = session->popup_cursor - available + 1U;
    for (size_t index = session->popup_scroll; index < session->course_count; index++) {
      int row = 2 + (int)(index - session->popup_scroll);
      if (row >= height - 3) break;
      wattrset(popup, COLOR_PAIR(index == session->popup_cursor ? PAIR_SELECTED : PAIR_TEXT));
      (void)mvwprintw(popup, row, 2, "%s %-*.*s",
                      index == session->active_course ? "*" : " ", width - 6,
                      width - 6, session->course_titles[index]);
    }
    wattrset(popup, COLOR_PAIR(PAIR_MUTED));
    (void)mvwprintw(popup, height - 2, 2, "Enter opens a course. Esc keeps the current course.");
  } else {
    int available = height - 4;
    size_t selected_rank = lesson_cursor_rank(session);
    if (selected_rank < session->popup_scroll)
      session->popup_scroll = selected_rank;
    else if (selected_rank >= session->popup_scroll + (size_t)available)
      session->popup_scroll = selected_rank - (size_t)available + 1U;
    size_t visible_index = 0U;
    for (size_t index = 0U; index < session->course->lesson_count; index++) {
      const hol_lesson *lesson = hol_course_lesson(session->course, index);
      if (lesson == NULL || (session->query_length > 0U &&
          strcasestr(lesson->title, session->query) == NULL)) continue;
      if (visible_index++ < session->popup_scroll) continue;
      int row = 2 + (int)(visible_index - session->popup_scroll - 1U);
      if (row >= 2 + available) break;
      bool selected = index == session->popup_cursor;
      wattrset(popup, COLOR_PAIR(selected ? PAIR_SELECTED : PAIR_TEXT));
      const char *mark = hol_state_completed(&session->state, session->course->id,
                                             lesson->id) ? "x" : " ";
      (void)mvwprintw(popup, row, 2, "[%s] %-*.*s", mark, width - 8, width - 8,
                      lesson->title);
    }
    wattrset(popup, COLOR_PAIR(PAIR_MUTED));
    (void)mvwprintw(popup, height - 2, 2, "/%s", session->query);
  }
  wrefresh(popup);
  delwin(popup);
}

static void render(ui_session *session) {
  int rows = 0;
  int columns = 0;
  getmaxyx(stdscr, rows, columns);
  erase();
  attrset(COLOR_PAIR(PAIR_HEADER) | A_BOLD);
  for (int column = 0; column < columns; column++) (void)mvaddch(0, column, ' ');
  const hol_lesson *lesson = current_lesson(session);
  (void)mvprintw(0, 1, "%s", HOL_APP_NAME);
  if (lesson != NULL) (void)mvprintw(0, 22, "%s  /  %s", session->course->title, lesson->title);
  attrset(COLOR_PAIR(PAIR_MUTED));
  (void)mvprintw(rows - 2, 1, "%s%s%s", session->search_active ? "/" : "NORMAL",
                 session->search_active ? session->query : "",
                  session->keys.pending_length > 0U ? session->keys.pending : "");
  int attribution_column = columns / 3;
  char attribution[1025];
  single_line(attribution, sizeof(attribution), session->course->attribution);
  if (attribution_column > 0 && attribution_column < columns - 1)
    (void)mvaddnstr(rows - 2, attribution_column, attribution,
                    columns - attribution_column - 1);
  if (lesson != NULL && lesson->kind == HOL_LESSON_EXERCISE)
    (void)mvprintw(rows - 1, 1,
                    "e edit  Space r run  Space t check  Space x reset  ? help  q quit");
  else
    (void)mvprintw(rows - 1, 1,
                    "Space ll lessons  Space lc courses  Enter select  ? help  q quit");

  int body_height = rows - 3;
  if (body_height < 5 || columns < 30 || lesson == NULL) {
    refresh();
    return;
  }
  bool wide = columns >= 100;
  if (wide) {
    int left_width = columns / 2;
    WINDOW *reader = newwin(body_height, left_width, 1, 0);
    WINDOW *right = newwin(body_height, columns - left_width, 1, left_width);
    draw_border(reader, "LESSON", session->pane == 0);
    draw_text(reader, session->lesson_text, session->reader_scroll, PAIR_TEXT);
    int right_height = body_height * 2 / 3;
    WINDOW *preview = derwin(right, right_height, columns - left_width, 0, 0);
    WINDOW *output = derwin(right, body_height - right_height,
                            columns - left_width, right_height, 0);
    const char *preview_title = lesson->kind == HOL_LESSON_QUIZ ? "QUIZ" : "BUFFER";
    draw_border(preview, preview_title, session->pane == 1);
    if (lesson->kind == HOL_LESSON_QUIZ && lesson->question_count > 0U) {
      hol_quiz_question *question = &lesson->questions[session->quiz_question];
      (void)mvwprintw(preview, 1, 2, "Question %zu of %zu",
                      session->quiz_question + 1U, lesson->question_count);
      (void)mvwaddnstr(preview, 3, 2, question->prompt, getmaxx(preview) - 4);
      for (size_t index = 0U; index < question->choice_count &&
           (int)index + 5 < getmaxy(preview) - 1; index++) {
        wattrset(preview, COLOR_PAIR(index == session->quiz_choice ? PAIR_SELECTED : PAIR_TEXT));
        (void)mvwprintw(preview, (int)index + 5, 3, "%s  %s",
                        question->choices[index].id, question->choices[index].text);
      }
    } else draw_text(preview, session->preview_text, session->preview_scroll, PAIR_TEXT);
    draw_border(output, "OUTPUT", session->pane == 2);
    draw_text(output, session->output, session->output_scroll,
              session->output != NULL && strstr(session->output, "PASSED") != NULL
                ? PAIR_SUCCESS : PAIR_TEXT);
    wrefresh(reader);
    wrefresh(preview);
    wrefresh(output);
    delwin(preview);
    delwin(output);
    delwin(reader);
    delwin(right);
  } else {
    WINDOW *pane = newwin(body_height, columns, 1, 0);
    const char *title = session->pane == 0 ? "LESSON" :
                        session->pane == 1 && lesson->kind == HOL_LESSON_QUIZ
                          ? "QUIZ" : session->pane == 1 ? "BUFFER" : "OUTPUT";
    draw_border(pane, title, true);
    if (session->pane == 0) draw_text(pane, session->lesson_text, session->reader_scroll, PAIR_TEXT);
    else if (session->pane == 1 && lesson->kind == HOL_LESSON_QUIZ &&
             lesson->question_count > 0U) {
      hol_quiz_question *question = &lesson->questions[session->quiz_question];
      (void)mvwprintw(pane, 1, 2, "Question %zu of %zu",
                      session->quiz_question + 1U, lesson->question_count);
      (void)mvwaddnstr(pane, 3, 2, question->prompt, getmaxx(pane) - 4);
      for (size_t index = 0U; index < question->choice_count &&
           (int)index + 5 < getmaxy(pane) - 1; index++) {
        wattrset(pane, COLOR_PAIR(index == session->quiz_choice ? PAIR_SELECTED : PAIR_TEXT));
        (void)mvwprintw(pane, (int)index + 5, 3, "%s  %s",
                        question->choices[index].id, question->choices[index].text);
      }
    } else if (session->pane == 1)
      draw_text(pane, session->preview_text, session->preview_scroll, PAIR_TEXT);
    else draw_text(pane, session->output, session->output_scroll, PAIR_TEXT);
    wrefresh(pane);
    delwin(pane);
  }
  refresh();
  if (session->popup != POPUP_NONE) draw_popup(session, rows, columns);
}

static size_t *active_scroll(ui_session *session) {
  if (session->pane == 0) return &session->reader_scroll;
  if (session->pane == 1) return &session->preview_scroll;
  return &session->output_scroll;
}

static const char *active_text(const ui_session *session) {
  if (session->pane == 0) return session->lesson_text;
  if (session->pane == 1) return session->preview_text;
  return session->output;
}

static void search_again(ui_session *session, bool forward) {
  const char *text = active_text(session);
  if (text == NULL || session->query_length == 0U) return;
  size_t current = *active_scroll(session);
  size_t line = 0U;
  size_t found = SIZE_MAX;
  const char *cursor = text;
  while (*cursor != '\0') {
    const char *next = strchr(cursor, '\n');
    size_t length = next != NULL ? (size_t)(next - cursor) : strlen(cursor);
    char buffer[1024];
    size_t retained = length < sizeof(buffer) - 1U ? length : sizeof(buffer) - 1U;
    memcpy(buffer, cursor, retained);
    buffer[retained] = '\0';
    if (strcasestr(buffer, session->query) != NULL &&
        ((forward && line > current) || (!forward && line < current))) {
      found = line;
      if (forward) break;
    }
    if (next == NULL) break;
    cursor = next + 1;
    line++;
  }
  if (found != SIZE_MAX) *active_scroll(session) = found;
}

static void reload_preview(ui_session *session, hol_error *error) {
  const hol_lesson *lesson = current_lesson(session);
  if (lesson == NULL || lesson->file_count == 0U) return;
  char path[4096];
  const hol_course_file *file = &lesson->files[session->file_index];
  const char *root = file->role == HOL_FILE_EDITABLE
                       ? session->workspace : session->course->root;
  const char *relative = file->role == HOL_FILE_EDITABLE
                           ? file->target : file->source;
  if (hol_join_path(path, sizeof(path), root, relative, error) < 0) return;
  free(session->preview_text);
  session->preview_text = NULL;
  if (capture_bat(path, &session->preview_text) < 0)
    session->preview_text = hol_read_text(path, HOL_OUTPUT_MAX, NULL, error);
}

static void run_lesson(ui_session *session, hol_run_mode mode, hol_error *error) {
  const hol_lesson *lesson = current_lesson(session);
  if (lesson == NULL || lesson->file_count == 0U) {
    set_output(session, "This lesson has no runnable workspace.");
    return;
  }
  set_output(session, mode == HOL_CHECK ? "Checking..." : "Running...");
  render(session);
  hol_run_result result = {0};
  if (hol_runner_execute(session->course, lesson, session->workspace, mode,
                         &result, error) < 0) {
    set_output(session, error->message);
    return;
  }
  const char *status = result.timed_out ? "TIMED OUT" :
                       result.passed ? (mode == HOL_CHECK ? "CHECK PASSED" : "RUN FINISHED") :
                       (mode == HOL_CHECK ? "CHECK FAILED" : "RUN FAILED");
  size_t needed = strlen(status) + strlen(result.stdout_data != NULL ? result.stdout_data : "") +
                  strlen(result.stderr_data != NULL ? result.stderr_data : "") + 64U;
  char *message = calloc(needed, 1U);
  if (message != NULL) {
    (void)snprintf(message, needed, "%s\n\n%s%s%s", status,
                   result.stdout_data != NULL ? result.stdout_data : "",
                   result.stderr_data != NULL && result.stderr_data[0] != '\0' ? "\n" : "",
                   result.stderr_data != NULL ? result.stderr_data : "");
    set_output(session, message);
    free(message);
  }
  if (mode == HOL_CHECK && result.passed) {
    (void)hol_state_mark_completed(&session->state, session->course->id,
                                   lesson->id, error);
    save_state(session);
  }
  hol_run_result_free(&result);
}

static void select_file(ui_session *session, int direction, hol_error *error) {
  const hol_lesson *lesson = current_lesson(session);
  if (lesson == NULL || lesson->file_count == 0U) return;
  size_t next = session->file_index;
  for (size_t attempts = 0U; attempts < lesson->file_count; attempts++) {
    next = direction > 0 ? (next + 1U) % lesson->file_count
                         : (next + lesson->file_count - 1U) % lesson->file_count;
    if (lesson->files[next].role != HOL_FILE_HIDDEN) break;
  }
  session->file_index = next;
  session->preview_scroll = 0U;
  reload_preview(session, error);
}

static void edit_file(ui_session *session, hol_error *error) {
  const hol_lesson *lesson = current_lesson(session);
  if (lesson == NULL || lesson->file_count == 0U ||
      lesson->files[session->file_index].role != HOL_FILE_EDITABLE) {
    set_output(session, "The selected buffer is read only.");
    return;
  }
  char path[4096];
  if (hol_join_path(path, sizeof(path), session->workspace,
                    lesson->files[session->file_index].target, error) < 0) return;
  def_prog_mode();
  endwin();
  int status = hol_launch_editor(path, error);
  reset_prog_mode();
  refresh();
  if (status < 0) set_output(session, error->message);
  reload_preview(session, error);
}

static void open_media(ui_session *session, hol_error *error) {
  const hol_lesson *lesson = current_lesson(session);
  if (lesson == NULL || lesson->media_count == 0U) {
    set_output(session, "This lesson has no local media.");
    return;
  }
  char path[4096];
  if (hol_join_path(path, sizeof(path), session->course->root,
                    lesson->media_paths[session->media_index], error) < 0) return;
  def_prog_mode();
  endwin();
  int status = hol_launch_media(path, error);
  reset_prog_mode();
  refresh();
  session->media_index = (session->media_index + 1U) % lesson->media_count;
  set_output(session, status == 0 ? "Media viewer closed." : error->message);
}

static void submit_quiz(ui_session *session, hol_error *error) {
  const hol_lesson *lesson = current_lesson(session);
  if (lesson == NULL || lesson->question_count == 0U) return;
  hol_quiz_question *question = &lesson->questions[session->quiz_question];
  bool correct = strcmp(question->choices[session->quiz_choice].id, question->answer) == 0;
  set_output(session, correct ? question->explanation : "Not quite. Try again.");
  if (correct) {
    session->quiz_correct[session->quiz_question] = true;
    size_t score = 0U;
    for (size_t index = 0U; index < lesson->question_count; index++)
      if (session->quiz_correct[index]) score++;
    if (score >= lesson->quiz_passing_score) {
      (void)hol_state_mark_completed(&session->state, session->course->id,
                                     lesson->id, error);
      set_output(session, "Quiz passed.");
    } else {
      for (size_t offset = 1U; offset <= lesson->question_count; offset++) {
        size_t next = (session->quiz_question + offset) % lesson->question_count;
        if (!session->quiz_correct[next]) {
          session->quiz_question = next;
          session->quiz_choice = 0U;
          break;
        }
      }
    }
    save_state(session);
  }
}

static void apply_action(ui_session *session, hol_action action, hol_error *error,
                         bool *running) {
  const hol_lesson *lesson = current_lesson(session);
  if (session->popup != POPUP_NONE) {
    if (action == HOL_ACTION_CANCEL || action == HOL_ACTION_QUIT) {
      session->popup = POPUP_NONE;
      session->query[0] = '\0';
      session->query_length = 0U;
      return;
    }
    if (action == HOL_ACTION_DOWN && session->popup == POPUP_LESSONS)
      (void)move_lesson_cursor(session, 1);
    if (action == HOL_ACTION_DOWN && session->popup == POPUP_COURSES &&
        session->popup_cursor + 1U < session->course_count)
      session->popup_cursor++;
    if (action == HOL_ACTION_UP && session->popup == POPUP_LESSONS)
      (void)move_lesson_cursor(session, -1);
    if (action == HOL_ACTION_UP && session->popup == POPUP_COURSES &&
        session->popup_cursor > 0U) session->popup_cursor--;
    if (action == HOL_ACTION_TOP && session->popup == POPUP_LESSONS)
      select_lesson_edge(session, false);
    if (action == HOL_ACTION_TOP && session->popup == POPUP_COURSES)
      session->popup_cursor = 0U;
    if (action == HOL_ACTION_BOTTOM && session->popup == POPUP_LESSONS)
      select_lesson_edge(session, true);
    if (action == HOL_ACTION_BOTTOM && session->popup == POPUP_COURSES &&
        session->course_count > 0U) session->popup_cursor = session->course_count - 1U;
    if (action == HOL_ACTION_HALF_DOWN && session->popup == POPUP_LESSONS)
      for (size_t step = 0U; step < 8U; step++)
        if (!move_lesson_cursor(session, 1)) break;
    if (action == HOL_ACTION_HALF_DOWN && session->popup == POPUP_COURSES &&
        session->popup_cursor + 8U < session->course_count)
      session->popup_cursor += 8U;
    if (action == HOL_ACTION_HALF_UP && session->popup == POPUP_LESSONS)
      for (size_t step = 0U; step < 8U; step++)
        if (!move_lesson_cursor(session, -1)) break;
    if (action == HOL_ACTION_HALF_UP && session->popup == POPUP_COURSES)
      session->popup_cursor = session->popup_cursor > 8U ? session->popup_cursor - 8U : 0U;
    if (action == HOL_ACTION_SELECT && session->popup == POPUP_LESSONS) {
      if (lesson_matches(session, session->popup_cursor) &&
          load_lesson(session, session->popup_cursor, error) < 0)
        set_output(session, error->message);
      session->popup = POPUP_NONE;
    }
    if (action == HOL_ACTION_SELECT && session->popup == POPUP_COURSES &&
        session->popup_cursor < session->course_count &&
        session->popup_cursor != session->active_course) {
      hol_course *next = NULL;
      const char *profile = session->course_profiles[session->popup_cursor];
      int load_status = profile != NULL
        ? hol_course_load_profile(session->course_roots[session->popup_cursor],
                                  profile, &next, error)
        : hol_course_load(session->course_roots[session->popup_cursor], &next, error);
      if (load_status < 0) {
        set_output(session, error->message);
      } else {
        hol_course_free(session->course);
        session->course = next;
        session->active_course = session->popup_cursor;
        if (load_lesson(session, 0U, error) < 0)
          set_output(session, error->message);
        else if (profile == NULL)
          set_output(session,
                     "Opened content-only. Quit and reopen this course from the "
                     "startup catalog menu to use its verified exercises.");
      }
      session->popup = POPUP_NONE;
    }
    return;
  }
  size_t *scroll = active_scroll(session);
  size_t total = line_count(active_text(session));
  switch (action) {
    case HOL_ACTION_QUIT: *running = false; break;
    case HOL_ACTION_LEFT: session->pane = session->pane > 0 ? session->pane - 1 : 0; break;
    case HOL_ACTION_RIGHT: session->pane = session->pane < 2 ? session->pane + 1 : 2; break;
    case HOL_ACTION_DOWN:
      if (lesson != NULL && lesson->kind == HOL_LESSON_QUIZ && session->pane == 1 &&
          lesson->questions[session->quiz_question].choice_count > 0U)
        session->quiz_choice = (session->quiz_choice + 1U) %
                               lesson->questions[session->quiz_question].choice_count;
      else if (*scroll + 1U < total) (*scroll)++;
      break;
    case HOL_ACTION_UP:
      if (lesson != NULL && lesson->kind == HOL_LESSON_QUIZ && session->pane == 1 &&
          lesson->questions[session->quiz_question].choice_count > 0U)
        session->quiz_choice = (session->quiz_choice +
          lesson->questions[session->quiz_question].choice_count - 1U) %
          lesson->questions[session->quiz_question].choice_count;
      else if (*scroll > 0U) (*scroll)--;
      break;
    case HOL_ACTION_TOP: *scroll = 0U; break;
    case HOL_ACTION_BOTTOM: *scroll = total > 0U ? total - 1U : 0U; break;
    case HOL_ACTION_HALF_DOWN: *scroll = *scroll + 10U < total ? *scroll + 10U : (total > 0U ? total - 1U : 0U); break;
    case HOL_ACTION_HALF_UP: *scroll = *scroll > 10U ? *scroll - 10U : 0U; break;
    case HOL_ACTION_NEXT_MATCH: search_again(session, true); break;
    case HOL_ACTION_PREV_MATCH: search_again(session, false); break;
    case HOL_ACTION_LESSON_PICKER:
      session->popup = POPUP_LESSONS;
      session->popup_cursor = session->lesson_index;
      session->popup_scroll = 0U;
      session->query[0] = '\0';
      session->query_length = 0U;
      break;
    case HOL_ACTION_COURSE_PICKER:
      session->popup = POPUP_COURSES;
      session->popup_cursor = session->active_course;
      session->popup_scroll = 0U;
      session->query[0] = '\0';
      session->query_length = 0U;
      break;
    case HOL_ACTION_HELP: session->popup = POPUP_HELP; break;
    case HOL_ACTION_RUN: run_lesson(session, HOL_RUN, error); break;
    case HOL_ACTION_CHECK: run_lesson(session, HOL_CHECK, error); break;
    case HOL_ACTION_EDIT: edit_file(session, error); break;
    case HOL_ACTION_SELECT:
      if (lesson != NULL && lesson->kind == HOL_LESSON_QUIZ) submit_quiz(session, error);
      else edit_file(session, error);
      break;
    case HOL_ACTION_NEXT_BUFFER: select_file(session, 1, error); break;
    case HOL_ACTION_PREV_BUFFER: select_file(session, -1, error); break;
    case HOL_ACTION_MEDIA: open_media(session, error); break;
    case HOL_ACTION_RESET:
      session->reset_pending = true;
      session->keys.mode = HOL_MODE_CONFIRM;
      set_output(session, "Reset this lesson? Press y to confirm or Esc to cancel.");
      break;
    default: break;
  }
  save_state(session);
}

int hol_ui_run(const char *course_root, const char *profile_path,
               hol_error *error) {
  ui_session session = {0};
  hol_keys_init(&session.keys);
  int load_status = profile_path != NULL
    ? hol_course_load_profile(course_root, profile_path, &session.course, error)
    : hol_course_load(course_root, &session.course, error);
  if (load_status < 0) return -1;
  discover_courses(&session, profile_path);
  if (data_path(session.state_path, sizeof(session.state_path), "XDG_STATE_HOME",
                ".local/state", "state.json", error) < 0 ||
      hol_state_load(session.state_path, &session.state, error) < 0) {
    hol_course_free(session.course);
    return -1;
  }
  bool restore_course_state = strcmp(session.state.course_id, session.course->id) == 0;
  if (restore_course_state) {
    for (size_t index = 0U; index < session.course->lesson_count; index++) {
      const hol_lesson *lesson = hol_course_lesson(session.course, index);
      if (lesson != NULL && strcmp(lesson->id, session.state.lesson_id) == 0) {
        session.lesson_index = index;
        break;
      }
    }
    session.pane = session.state.pane >= 0 && session.state.pane <= 2 ? session.state.pane : 0;
  }
  size_t saved_reader_scroll = restore_course_state ? session.state.reader_scroll : 0U;
  size_t saved_preview_scroll = restore_course_state ? session.state.preview_scroll : 0U;
  size_t saved_output_scroll = restore_course_state ? session.state.output_scroll : 0U;
  char saved_file[HOL_PATH_MAX + 1];
  (void)snprintf(saved_file, sizeof(saved_file), "%s",
                 restore_course_state ? session.state.file_path : "");
  set_output(&session, "Ready. Press ? for keybindings.");
  if (load_lesson(&session, session.lesson_index, error) < 0) goto failure;
  const hol_lesson *saved_lesson = current_lesson(&session);
  if (saved_lesson != NULL && saved_file[0] != '\0') {
    for (size_t index = 0U; index < saved_lesson->file_count; index++) {
      if (strcmp(saved_lesson->files[index].target, saved_file) == 0 &&
          saved_lesson->files[index].role != HOL_FILE_HIDDEN) {
        session.file_index = index;
        reload_preview(&session, error);
        break;
      }
    }
  }
  session.reader_scroll = saved_reader_scroll;
  session.preview_scroll = saved_preview_scroll;
  session.output_scroll = saved_output_scroll;
  save_state(&session);

  (void)setlocale(LC_ALL, "");
  if (initscr() == NULL) {
    hol_error_set(error, HOL_ERR_IO, "cannot initialize ncurses");
    goto failure;
  }
  cbreak();
  noecho();
  keypad(stdscr, true);
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
  timeout(100);
  curs_set(0);
  initialize_colors();
  bool running = true;
  while (running) {
    render(&session);
    int key = getch();
    if (key == ERR) continue;
    if (key == KEY_RESIZE) continue;
    if (key == KEY_MOUSE) {
      MEVENT event;
      if (getmouse(&event) == OK) {
        if (event.bstate & BUTTON4_PRESSED)
          apply_action(&session, HOL_ACTION_UP, error, &running);
        if (event.bstate & BUTTON5_PRESSED)
          apply_action(&session, HOL_ACTION_DOWN, error, &running);
      }
      continue;
    }
    if (session.reset_pending) {
      if (key == 'y') {
        const hol_lesson *lesson = current_lesson(&session);
        if (lesson != NULL && hol_workspace_reset(session.course, lesson,
                                                   session.workspace, error) == 0) {
          reload_preview(&session, error);
          set_output(&session, "Lesson workspace reset.");
        } else set_output(&session, error->message);
      }
      session.reset_pending = false;
      session.keys.mode = HOL_MODE_NORMAL;
      continue;
    }
    if (session.search_active || session.keys.mode == HOL_MODE_SEARCH) {
      if (key == 27) {
        session.search_active = false;
        session.query[0] = '\0';
        session.query_length = 0U;
        session.keys.mode = HOL_MODE_NORMAL;
      } else if (key == '\n' || key == '\r') {
        session.search_active = false;
        session.keys.mode = HOL_MODE_NORMAL;
        if (session.popup == POPUP_LESSONS && session.query_length > 0U) {
          for (size_t index = 0U; index < session.course->lesson_count; index++) {
            const hol_lesson *candidate = hol_course_lesson(session.course, index);
            if (candidate != NULL && strcasestr(candidate->title, session.query) != NULL) {
              session.popup_cursor = index;
              break;
            }
          }
        }
        const char *text = session.popup == POPUP_NONE ? active_text(&session) : NULL;
        if (text != NULL && session.query_length > 0U) {
          const char *match = strcasestr(text, session.query);
          if (match != NULL) {
            size_t line = 0U;
            for (const char *cursor = text; cursor < match; cursor++) if (*cursor == '\n') line++;
            *active_scroll(&session) = line;
          }
        }
      } else if ((key == KEY_BACKSPACE || key == 127) && session.query_length > 0U) {
        session.query[--session.query_length] = '\0';
      } else if (key >= 32 && key <= 126 && session.query_length + 1U < sizeof(session.query)) {
        session.query[session.query_length++] = (char)key;
        session.query[session.query_length] = '\0';
      }
      if (session.popup == POPUP_LESSONS &&
          !lesson_matches(&session, session.popup_cursor))
        select_lesson_edge(&session, false);
      continue;
    }
    hol_action action;
    if (key == KEY_UP) action = HOL_ACTION_UP;
    else if (key == KEY_DOWN) action = HOL_ACTION_DOWN;
    else if (key == KEY_LEFT) action = HOL_ACTION_LEFT;
    else if (key == KEY_RIGHT) action = HOL_ACTION_RIGHT;
    else action = hol_keys_feed(&session.keys, key, now_ms());
    if (action == HOL_ACTION_SEARCH) {
      session.search_active = true;
      session.query[0] = '\0';
      session.query_length = 0U;
    } else apply_action(&session, action, error, &running);
  }
  save_state(&session);
  endwin();
  free(session.lesson_text);
  free(session.preview_text);
  free(session.output);
  free(session.quiz_correct);
  hol_state_free(&session.state);
  hol_course_free(session.course);
  free_course_list(&session);
  return 0;

failure:
  free(session.lesson_text);
  free(session.preview_text);
  free(session.output);
  free(session.quiz_correct);
  hol_state_free(&session.state);
  hol_course_free(session.course);
  free_course_list(&session);
  return -1;
}
