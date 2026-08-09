#include "hol.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void copy_file(const char *source, const char *target) {
  int input = open(source, O_RDONLY | O_CLOEXEC);
  int output = open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  assert(input >= 0 && output >= 0);
  char buffer[8192];
  for (;;) {
    ssize_t count = read(input, buffer, sizeof(buffer));
    assert(count >= 0);
    if (count == 0) break;
    size_t offset = 0U;
    while (offset < (size_t)count) {
      ssize_t written = write(output, buffer + offset, (size_t)count - offset);
      assert(written > 0);
      offset += (size_t)written;
    }
  }
  assert(close(input) == 0 && close(output) == 0);
}

static void write_replaced(const char *source, const char *target,
                           const char *needle, const char *replacement) {
  hol_error error = {0};
  size_t length = 0U;
  char *text = hol_read_text(source, 1024U * 1024U, &length, &error);
  assert(text != NULL);
  char *match = strstr(text, needle);
  assert(match != NULL);
  size_t prefix = (size_t)(match - text);
  size_t needle_length = strlen(needle);
  size_t replacement_length = strlen(replacement);
  size_t output_length = length - needle_length + replacement_length;
  char *output = malloc(output_length + 1U);
  assert(output != NULL);
  memcpy(output, text, prefix);
  memcpy(output + prefix, replacement, replacement_length);
  memcpy(output + prefix + replacement_length, match + needle_length,
         length - prefix - needle_length);
  output[output_length] = '\0';
  assert(hol_atomic_write(target, output, output_length, &error) == 0);
  free(output);
  free(text);
}

static void assert_profile_rejected(const char *cartridge, const char *profile) {
  hol_error error = {0};
  hol_course *course = NULL;
  assert(hol_course_load_profile(cartridge, profile, &course, &error) == -1);
  assert(course == NULL && error.code != HOL_OK);
}

static void assert_manifest_variant_rejected(const char *root, const char *name,
                                             const char *needle,
                                             const char *replacement) {
  hol_error error = {0};
  char source[4096];
  char cartridge[4096];
  char profile[4096];
  char manifest[4096];
  assert(snprintf(source, sizeof(source), "%s/%s-source", root, name) > 0);
  assert(snprintf(cartridge, sizeof(cartridge), "%s/%s.imscc", root, name) > 0);
  assert(snprintf(profile, sizeof(profile), "%s/%s.profile.json", root, name) > 0);
  assert(snprintf(manifest, sizeof(manifest), "%s/imsmanifest.xml", source) > 0);
  char command[16384];
  assert(snprintf(command, sizeof(command),
                  "mkdir -p '%s' && cp -R tests/fixtures/cartridge/. '%s/'",
                  source, source) > 0);
  assert(system(command) == 0);
  write_replaced(manifest, manifest, needle, replacement);
  assert(snprintf(command, sizeof(command),
                  "cd '%s' && find imsmanifest.xml LICENSE web assessments exercises -type f -print | LC_ALL=C sort | zip -X -q '%s' -@",
                  source, cartridge) > 0);
  assert(system(command) == 0);
  char original_digest[65];
  char variant_digest[65];
  assert(hol_sha256_file("build/test-course.imscc", original_digest, &error) == 0);
  assert(hol_sha256_file(cartridge, variant_digest, &error) == 0);
  write_replaced("build/test-course.profile.json", profile,
                 original_digest, variant_digest);
  assert_profile_rejected(cartridge, profile);
  assert(snprintf(command, sizeof(command), "rm -rf -- '%s'", source) > 0);
  assert(system(command) == 0);
  assert(unlink(profile) == 0);
  assert(unlink(cartridge) == 0);
}

int main(void) {
  assert(hol_safe_relative_path("web/lesson.html"));
  assert(!hol_safe_relative_path("../private"));
  assert(!hol_safe_relative_path("/absolute"));
  assert(!hol_safe_relative_path("double//segment"));
  assert(!hol_safe_relative_path("windows\\path"));
  assert(hol_valid_id("course.lesson-1"));
  assert(!hol_valid_id("Course Lesson"));
  assert(hol_version_supported("0.1.0"));
  assert(!hol_version_supported("0.2.0"));

  hol_error error = {0};
  hol_course *course = NULL;
  assert(hol_course_load("build/test-course.imscc", &course, &error) == 0);
  assert(hol_course_lesson(course, 1U)->kind == HOL_LESSON_READING);
  assert(!course->has_exercise_profile);
  hol_course_free(course);
  course = NULL;
  assert(hol_course_load_profile("build/test-course.imscc",
                                 "build/test-course.profile.json",
                                 &course, &error) == 0);
  assert(course != NULL);
  assert(strcmp(course->id, "test.course") == 0);
  assert(strcmp(course->version, "1.0.0") == 0);
  assert(strcmp(course->license_spdx, "MIT") == 0);
  assert(course->chapter_count == 1U);
  assert(course->lesson_count == 5U);
  const hol_lesson *welcome = hol_course_lesson(course, 0U);
  assert(welcome != NULL && strcmp(welcome->id, "intro") == 0);
  assert(welcome->kind == HOL_LESSON_READING);
  const hol_lesson *exercise = hol_course_lesson(course, 1U);
  assert(exercise != NULL && strcmp(exercise->id, "c-greeting") == 0);
  assert(exercise->kind == HOL_LESSON_EXERCISE);
  assert(exercise->file_count == 4U);
  assert(strcmp(exercise->default_file, "greeting.c") == 0);
  assert(strcmp(exercise->runner.id, "c") == 0);
  assert(strcmp(exercise->runner.profile, "c23") == 0);
  const hol_lesson *stdout_exercise = hol_course_lesson(course, 2U);
  assert(stdout_exercise != NULL && stdout_exercise->kind == HOL_LESSON_EXERCISE);
  assert(stdout_exercise->runner.check_kind == HOL_CHECK_STDOUT);
  assert(strcmp(stdout_exercise->runner.expected_output, "Hello stdout!\n") == 0);
  const hol_lesson *sql_exercise = hol_course_lesson(course, 3U);
  assert(sql_exercise != NULL && sql_exercise->kind == HOL_LESSON_EXERCISE);
  assert(strcmp(sql_exercise->runner.id, "sql") == 0);
  assert(strcmp(sql_exercise->runner.profile, "sqlite3") == 0);
  const hol_lesson *quiz = hol_course_lesson(course, 4U);
  assert(quiz != NULL && quiz->kind == HOL_LESSON_QUIZ);
  assert(quiz->question_count == 1U && quiz->quiz_passing_score == 1U);
  assert(strcmp(quiz->questions[0].answer, "a") == 0);

  char root[] = "/tmp/hol-state-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char state_path[4096];
  assert(hol_join_path(state_path, sizeof(state_path), root, "state.json", &error) == 0);
  hol_state state = {0};
  (void)strcpy(state.course_id, course->id);
  (void)strcpy(state.lesson_id, quiz->id);
  state.reader_scroll = 12U;
  assert(hol_state_mark_completed(&state, course->id, quiz->id, &error) == 0);
  assert(hol_state_save(state_path, &state, &error) == 0);
  hol_state loaded = {0};
  assert(hol_state_load(state_path, &loaded, &error) == 0);
  assert(strcmp(loaded.lesson_id, "knowledge-check") == 0);
  assert(loaded.reader_scroll == 12U);
  assert(hol_state_completed(&loaded, course->id, quiz->id));
  hol_state_free(&loaded);
  hol_state_free(&state);
  hol_course_free(course);

  char profile_root[] = "/tmp/hol-profile-XXXXXX";
  assert(mkdtemp(profile_root) != NULL);
  char copied_cartridge[4096];
  char copied_profile[4096];
  assert(hol_join_path(copied_cartridge, sizeof(copied_cartridge), profile_root,
                       "course.imscc", &error) == 0);
  assert(hol_join_path(copied_profile, sizeof(copied_profile), profile_root,
                       "course.profile.json", &error) == 0);
  copy_file("build/test-course.imscc", copied_cartridge);

  hol_course *readable = NULL;
  assert(hol_course_load(copied_cartridge, &readable, &error) == 0);
  assert(hol_course_lesson(readable, 1U)->kind == HOL_LESSON_READING);
  hol_course_free(readable);

  size_t profile_length = 0U;
  char *profile = hol_read_text("build/test-course.profile.json", 1024U * 1024U,
                                &profile_length, &error);
  assert(profile != NULL);
  char *digest = strstr(profile, "\"cartridge_sha256\": \"");
  assert(digest != NULL);
  digest += strlen("\"cartridge_sha256\": \"");
  digest[0] = digest[0] == '0' ? '1' : '0';
  assert(hol_atomic_write(copied_profile, profile, profile_length, &error) == 0);
  free(profile);
  hol_course *mismatched = NULL;
  memset(&error, 0, sizeof(error));
  assert(hol_course_load_profile(copied_cartridge, copied_profile,
                                 &mismatched, &error) == -1);
  assert(mismatched == NULL && error.code == HOL_ERR_CHECKSUM);

  assert(unlink(copied_profile) == 0);
  write_replaced("build/test-course.profile.json", copied_profile,
                 "\n  \"lessons\": [",
                 "\n  \"command\": \"sh\",\n  \"lessons\": [");
  memset(&error, 0, sizeof(error));
  assert(hol_course_load_profile(copied_cartridge, copied_profile,
                                 &mismatched, &error) == -1);
  assert(mismatched == NULL && error.code == HOL_ERR_SCHEMA);
  assert(strstr(error.message, "unsupported exercise profile field") != NULL);

  assert(unlink(copied_profile) == 0);
  profile = hol_read_text("build/test-course.profile.json", 1024U * 1024U,
                          &profile_length, &error);
  assert(profile != NULL);
  digest = strstr(profile, "\"sha256\": \"");
  assert(digest != NULL);
  digest += strlen("\"sha256\": \"");
  digest[0] = digest[0] == '0' ? '1' : '0';
  assert(hol_atomic_write(copied_profile, profile, profile_length, &error) == 0);
  free(profile);
  memset(&error, 0, sizeof(error));
  assert(hol_course_load_profile(copied_cartridge, copied_profile,
                                 &mismatched, &error) == -1);
  assert(mismatched == NULL && error.code == HOL_ERR_CHECKSUM);

  assert(unlink(copied_profile) == 0);
  write_replaced("build/test-course.profile.json", copied_profile,
                 "\"id\": \"c-greeting\"", "\"id\": \"missing-lesson\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  write_replaced("build/test-course.profile.json", copied_profile,
                 "\"role\": \"editable\"", "\"role\": \"owner\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  write_replaced("build/test-course.profile.json", copied_profile,
                 "\"phase\": \"both\"", "\"phase\": \"run\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  write_replaced("build/test-course.profile.json", copied_profile,
                  "\"id\": \"c\"", "\"id\": \"shell\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  write_replaced("build/test-course.profile.json", copied_profile,
                 "\"profile\": \"c23\"", "\"profile\": \"c99\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  write_replaced("build/test-course.profile.json", copied_profile,
                 "\"profile\": \"sqlite3\"", "\"profile\": \"postgres\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  write_replaced("build/test-course.profile.json", copied_profile,
                 "\"kind\": \"tests\"", "\"kind\": \"command\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  write_replaced("build/test-course.profile.json", copied_profile,
                 "\"phase\": \"check\"", "\"phase\": \"run\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  write_replaced("build/test-course.profile.json", copied_profile,
                 "\"target\": \"check_test.c\",\n            \"role\": \"hidden\"",
                 "\"target\": \"check_test.c\",\n            \"role\": \"readonly\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  write_replaced("build/test-course.profile.json", copied_profile,
                 "\"phase\": \"run\"", "\"phase\": \"check\"");
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  char starter_digest[65];
  char license_digest[65];
  assert(hol_sha256_file("tests/fixtures/cartridge/exercises/greeting.c",
                         starter_digest, &error) == 0);
  assert(hol_sha256_file("tests/fixtures/cartridge/LICENSE",
                         license_digest, &error) == 0);
  write_replaced("build/test-course.profile.json", copied_profile,
                 "exercises/greeting.c", "LICENSE");
  write_replaced(copied_profile, copied_profile, starter_digest, license_digest);
  assert_profile_rejected(copied_cartridge, copied_profile);
  assert(unlink(copied_profile) == 0);

  assert_manifest_variant_rejected(
    profile_root, "duplicate-item",
    "        </item>\n      </item>\n    </organization>",
    "        </item>\n"
    "        <item identifier=\"c-greeting\"><title>Duplicate ID</title>\n"
    "          <item identifier=\"extra-reading\" identifierref=\"resource-intro\"><title>Extra</title></item>\n"
    "        </item>\n"
    "      </item>\n    </organization>");
  assert_manifest_variant_rejected(
    profile_root, "duplicate-resource",
    "  </resources>",
    "    <resource identifier=\"resource-c-greeting\" type=\"webcontent\" href=\"web/c-greeting.html\">\n"
    "      <file href=\"web/c-greeting.html\"/>\n"
    "      <file href=\"exercises/greeting.c\"/>\n"
    "      <file href=\"exercises/exercise_support.c\"/>\n"
    "      <file href=\"exercises/visible_test.c\"/>\n"
    "      <file href=\"exercises/check_test.c\"/>\n"
    "    </resource>\n"
    "  </resources>");
  assert_manifest_variant_rejected(
    profile_root, "foreign-file",
    "      <file href=\"exercises/greeting.c\"/>",
    "      <file xmlns=\"urn:foreign\" href=\"exercises/greeting.c\"/>");

  assert(unlink(copied_cartridge) == 0);
  assert(rmdir(profile_root) == 0);

  hol_course *invalid = NULL;
  memset(&error, 0, sizeof(error));
  assert(hol_course_load("tests/fixtures/cartridge", &invalid, &error) == -1);
  assert(invalid == NULL && error.code == HOL_ERR_UNSUPPORTED);
  assert(unlink(state_path) == 0);
  assert(rmdir(root) == 0);
  puts("core tests passed");
  return 0;
}
