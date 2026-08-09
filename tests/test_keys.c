#include "hol.h"

#include <assert.h>
#include <string.h>

static hol_action feed(hol_key_state *state, const char *keys) {
  hol_action action = HOL_ACTION_NONE;
  int64_t now = 1000;
  for (size_t index = 0U; keys[index] != '\0'; index++) {
    action = hol_keys_feed(state, (unsigned char)keys[index], now);
    now += 10;
  }
  return action;
}

int main(void) {
  hol_key_state state;
  hol_keys_init(&state);
  assert(feed(&state, "gg") == HOL_ACTION_TOP);
  assert(feed(&state, "gt") == HOL_ACTION_NEXT_BUFFER);
  assert(feed(&state, "gT") == HOL_ACTION_PREV_BUFFER);
  assert(feed(&state, "]b") == HOL_ACTION_NEXT_BUFFER);
  assert(feed(&state, "[b") == HOL_ACTION_PREV_BUFFER);
  assert(feed(&state, " ll") == HOL_ACTION_LESSON_PICKER);
  assert(feed(&state, " lc") == HOL_ACTION_COURSE_PICKER);
  assert(feed(&state, " r") == HOL_ACTION_RUN);
  assert(feed(&state, " t") == HOL_ACTION_CHECK);
  assert(feed(&state, " x") == HOL_ACTION_RESET);
  assert(feed(&state, " m") == HOL_ACTION_MEDIA);
  assert(feed(&state, " ?") == HOL_ACTION_HELP);
  assert(feed(&state, "G") == HOL_ACTION_BOTTOM);
  assert(feed(&state, "j") == HOL_ACTION_DOWN);
  assert(feed(&state, "k") == HOL_ACTION_UP);
  assert(feed(&state, "h") == HOL_ACTION_LEFT);
  assert(feed(&state, "l") == HOL_ACTION_RIGHT);
  assert(feed(&state, "n") == HOL_ACTION_NEXT_MATCH);
  assert(feed(&state, "N") == HOL_ACTION_PREV_MATCH);
  assert(feed(&state, "e") == HOL_ACTION_EDIT);
  assert(feed(&state, "?") == HOL_ACTION_HELP);
  assert(feed(&state, "q") == HOL_ACTION_QUIT);
  assert(hol_keys_feed(&state, 4, 2000) == HOL_ACTION_HALF_DOWN);
  assert(hol_keys_feed(&state, 21, 2010) == HOL_ACTION_HALF_UP);

  assert(hol_keys_feed(&state, 'g', 3000) == HOL_ACTION_NONE);
  assert(hol_keys_feed(&state, 'g', 3800) == HOL_ACTION_NONE);
  assert(state.pending_length == 1U);
  assert(hol_keys_feed(&state, 27, 3810) == HOL_ACTION_CANCEL);
  assert(state.pending_length == 0U);

  assert(hol_keys_feed(&state, '/', 4000) == HOL_ACTION_SEARCH);
  assert(state.mode == HOL_MODE_SEARCH);
  assert(hol_keys_feed(&state, 'x', 4010) == HOL_ACTION_NONE);
  assert(hol_keys_feed(&state, '\n', 4020) == HOL_ACTION_SELECT);
  assert(state.mode == HOL_MODE_NORMAL);
  assert(strcmp(hol_action_name(HOL_ACTION_LESSON_PICKER), "lesson-picker") == 0);
  puts("key tests passed");
  return 0;
}
