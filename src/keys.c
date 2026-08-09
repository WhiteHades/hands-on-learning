#include "hol.h"

#include <string.h>

#define HOL_SEQUENCE_TIMEOUT_MS 750
#define HOL_CTRL_D 4
#define HOL_CTRL_U 21
#define HOL_ESCAPE 27

void hol_keys_init(hol_key_state *state) {
  memset(state, 0, sizeof(*state));
  state->mode = HOL_MODE_NORMAL;
}

static void clear_pending(hol_key_state *state) {
  state->pending[0] = '\0';
  state->pending_length = 0U;
  state->pending_since_ms = 0;
}

static void append_pending(hol_key_state *state, int key, int64_t now_ms) {
  if (state->pending_length + 1U >= sizeof(state->pending)) {
    clear_pending(state);
    return;
  }
  state->pending[state->pending_length++] = (char)key;
  state->pending[state->pending_length] = '\0';
  if (state->pending_length == 1U) state->pending_since_ms = now_ms;
}

static hol_action sequence_action(const char *sequence) {
  if (strcmp(sequence, "gg") == 0) return HOL_ACTION_TOP;
  if (strcmp(sequence, "gt") == 0 || strcmp(sequence, "]b") == 0)
    return HOL_ACTION_NEXT_BUFFER;
  if (strcmp(sequence, "gT") == 0 || strcmp(sequence, "[b") == 0)
    return HOL_ACTION_PREV_BUFFER;
  if (strcmp(sequence, " ll") == 0) return HOL_ACTION_LESSON_PICKER;
  if (strcmp(sequence, " lc") == 0) return HOL_ACTION_COURSE_PICKER;
  if (strcmp(sequence, " r") == 0) return HOL_ACTION_RUN;
  if (strcmp(sequence, " t") == 0) return HOL_ACTION_CHECK;
  if (strcmp(sequence, " x") == 0) return HOL_ACTION_RESET;
  if (strcmp(sequence, " m") == 0) return HOL_ACTION_MEDIA;
  if (strcmp(sequence, " ?") == 0) return HOL_ACTION_HELP;
  return HOL_ACTION_NONE;
}

static bool sequence_prefix(const char *sequence) {
  static const char *const sequences[] = {
    "gg", "gt", "gT", "]b", "[b", " ll", " lc", " r", " t", " x", " m", " ?",
  };
  size_t length = strlen(sequence);
  for (size_t index = 0U; index < sizeof(sequences) / sizeof(sequences[0]); index++) {
    if (strncmp(sequences[index], sequence, length) == 0) return true;
  }
  return false;
}

hol_action hol_keys_feed(hol_key_state *state, int key, int64_t now_ms) {
  if (state == NULL) return HOL_ACTION_NONE;
  if (state->pending_length > 0U &&
      now_ms - state->pending_since_ms > HOL_SEQUENCE_TIMEOUT_MS) {
    clear_pending(state);
  }
  if (key == HOL_ESCAPE) {
    clear_pending(state);
    state->mode = HOL_MODE_NORMAL;
    return HOL_ACTION_CANCEL;
  }
  if (state->mode == HOL_MODE_SEARCH) {
    if (key == '\n' || key == '\r') {
      state->mode = HOL_MODE_NORMAL;
      return HOL_ACTION_SELECT;
    }
    return HOL_ACTION_NONE;
  }
  if (state->pending_length > 0U) {
    append_pending(state, key, now_ms);
    hol_action action = sequence_action(state->pending);
    if (action != HOL_ACTION_NONE) {
      clear_pending(state);
      return action;
    }
    if (!sequence_prefix(state->pending)) clear_pending(state);
    return HOL_ACTION_NONE;
  }
  if (key == 'g' || key == ']' || key == '[' || key == ' ') {
    append_pending(state, key, now_ms);
    return HOL_ACTION_NONE;
  }
  switch (key) {
    case 'h': return HOL_ACTION_LEFT;
    case 'j': return HOL_ACTION_DOWN;
    case 'k': return HOL_ACTION_UP;
    case 'l': return HOL_ACTION_RIGHT;
    case 'G': return HOL_ACTION_BOTTOM;
    case HOL_CTRL_D: return HOL_ACTION_HALF_DOWN;
    case HOL_CTRL_U: return HOL_ACTION_HALF_UP;
    case '/':
      state->mode = HOL_MODE_SEARCH;
      return HOL_ACTION_SEARCH;
    case 'n': return HOL_ACTION_NEXT_MATCH;
    case 'N': return HOL_ACTION_PREV_MATCH;
    case 'e': return HOL_ACTION_EDIT;
    case '\n':
    case '\r': return HOL_ACTION_SELECT;
    case '?': return HOL_ACTION_HELP;
    case 'q': return HOL_ACTION_QUIT;
    default: return HOL_ACTION_NONE;
  }
}

const char *hol_action_name(hol_action action) {
  static const char *const names[] = {
    "none", "quit", "cancel", "up", "down", "left", "right", "top", "bottom",
    "half-up", "half-down", "search", "next-match", "previous-match", "select",
    "edit", "run", "check", "reset", "media", "lesson-picker", "course-picker",
    "next-buffer", "previous-buffer", "help",
  };
  size_t index = (size_t)action;
  if (index >= sizeof(names) / sizeof(names[0])) return "unknown";
  return names[index];
}
