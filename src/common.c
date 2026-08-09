#include "hol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  uint32_t state[8];
  uint64_t bit_length;
  unsigned char block[64];
  size_t block_length;
} sha256_context;

static const uint32_t sha256_constants[64] = {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
  0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
  0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
  0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
  0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
  0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
  0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
  0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
  0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
  0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
  0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static uint32_t rotate_right(uint32_t value, unsigned count) {
  return (value >> count) | (value << (32U - count));
}

static void sha256_transform(sha256_context *context) {
  uint32_t words[64];
  for (size_t index = 0U; index < 16U; index++) {
    size_t offset = index * 4U;
    words[index] = (uint32_t)context->block[offset] << 24U |
                   (uint32_t)context->block[offset + 1U] << 16U |
                   (uint32_t)context->block[offset + 2U] << 8U |
                   (uint32_t)context->block[offset + 3U];
  }
  for (size_t index = 16U; index < 64U; index++) {
    uint32_t first = rotate_right(words[index - 15U], 7U) ^
                     rotate_right(words[index - 15U], 18U) ^
                     (words[index - 15U] >> 3U);
    uint32_t second = rotate_right(words[index - 2U], 17U) ^
                      rotate_right(words[index - 2U], 19U) ^
                      (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + first + words[index - 7U] + second;
  }
  uint32_t a = context->state[0];
  uint32_t b = context->state[1];
  uint32_t c = context->state[2];
  uint32_t d = context->state[3];
  uint32_t e = context->state[4];
  uint32_t f = context->state[5];
  uint32_t g = context->state[6];
  uint32_t h = context->state[7];
  for (size_t index = 0U; index < 64U; index++) {
    uint32_t sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                    rotate_right(e, 25U);
    uint32_t choice = (e & f) ^ (~e & g);
    uint32_t temporary1 = h + sum1 + choice + sha256_constants[index] + words[index];
    uint32_t sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                    rotate_right(a, 22U);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  context->state[0] += a;
  context->state[1] += b;
  context->state[2] += c;
  context->state[3] += d;
  context->state[4] += e;
  context->state[5] += f;
  context->state[6] += g;
  context->state[7] += h;
}

static void sha256_init(sha256_context *context) {
  *context = (sha256_context){
    .state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U},
  };
}

static void sha256_update(sha256_context *context, const unsigned char *data,
                          size_t length) {
  for (size_t index = 0U; index < length; index++) {
    context->block[context->block_length++] = data[index];
    if (context->block_length == sizeof(context->block)) {
      sha256_transform(context);
      context->bit_length += 512U;
      context->block_length = 0U;
    }
  }
}

static void sha256_finish(sha256_context *context, unsigned char digest[32]) {
  context->bit_length += (uint64_t)context->block_length * 8U;
  context->block[context->block_length++] = 0x80U;
  if (context->block_length > 56U) {
    while (context->block_length < 64U) context->block[context->block_length++] = 0U;
    sha256_transform(context);
    context->block_length = 0U;
  }
  while (context->block_length < 56U) context->block[context->block_length++] = 0U;
  for (size_t index = 0U; index < 8U; index++)
    context->block[63U - index] = (unsigned char)(context->bit_length >> (index * 8U));
  sha256_transform(context);
  for (size_t index = 0U; index < 8U; index++) {
    digest[index * 4U] = (unsigned char)(context->state[index] >> 24U);
    digest[index * 4U + 1U] = (unsigned char)(context->state[index] >> 16U);
    digest[index * 4U + 2U] = (unsigned char)(context->state[index] >> 8U);
    digest[index * 4U + 3U] = (unsigned char)context->state[index];
  }
}

int hol_sha256_file(const char *path, char hexadecimal[65], hol_error *error) {
  int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot hash %s", path);
    return -1;
  }
  sha256_context context;
  sha256_init(&context);
  unsigned char buffer[64U * 1024U];
  for (;;) {
    ssize_t count = read(descriptor, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      (void)close(descriptor);
      return -1;
    }
    if (count == 0) break;
    sha256_update(&context, buffer, (size_t)count);
  }
  (void)close(descriptor);
  unsigned char digest[32];
  sha256_finish(&context, digest);
  static const char digits[] = "0123456789abcdef";
  for (size_t index = 0U; index < sizeof(digest); index++) {
    hexadecimal[index * 2U] = digits[digest[index] >> 4U];
    hexadecimal[index * 2U + 1U] = digits[digest[index] & 0x0fU];
  }
  hexadecimal[64] = '\0';
  return 0;
}

void hol_error_set(hol_error *error, hol_errc code, const char *format, ...) {
  if (error == NULL) return;
  error->code = code;
  error->system_errno = errno;
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
  va_end(arguments);
}

bool hol_safe_relative_path(const char *path) {
  if (path == NULL || path[0] == '\0' || path[0] == '/' ||
      strlen(path) > HOL_PATH_MAX) return false;
  const char *segment = path;
  for (const char *cursor = path;; cursor++) {
    unsigned char byte = (unsigned char)*cursor;
    if (byte == '\\' || (byte != 0U && byte < 0x20U) || byte == 0x7fU)
      return false;
    if (byte == '/' || byte == '\0') {
      size_t length = (size_t)(cursor - segment);
      if (length == 0U || (length == 1U && segment[0] == '.') ||
          (length == 2U && segment[0] == '.' && segment[1] == '.')) return false;
      if (byte == '\0') break;
      segment = cursor + 1;
    }
  }
  return true;
}

bool hol_valid_id(const char *id) {
  size_t length = strlen(id);
  if (length == 0U || length > HOL_ID_MAX) return false;
  for (size_t index = 0U; index < length; index++) {
    char value = id[index];
    bool allowed = (value >= 'a' && value <= 'z') ||
                   (value >= '0' && value <= '9') || value == '.' ||
                   value == '_' || value == '-';
    if (!allowed || (index == 0U && (value == '.' || value == '_' || value == '-')))
      return false;
  }
  return true;
}

int hol_join_path(char *output, size_t size, const char *root,
                  const char *relative, hol_error *error) {
  if (!hol_safe_relative_path(relative)) {
    hol_error_set(error, HOL_ERR_PATH, "unsafe relative path: %s",
                  relative != NULL ? relative : "(null)");
    return -1;
  }
  int written = snprintf(output, size, "%s/%s", root, relative);
  if (written < 0 || (size_t)written >= size) {
    hol_error_set(error, HOL_ERR_PATH, "joined path is too long");
    return -1;
  }
  return 0;
}

static int make_parent_directories(const char *path, hol_error *error) {
  char copy[4096];
  int written = snprintf(copy, sizeof(copy), "%s", path);
  if (written < 0 || (size_t)written >= sizeof(copy)) {
    hol_error_set(error, HOL_ERR_PATH, "path is too long");
    return -1;
  }
  for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(copy, 0700) < 0 && errno != EEXIST) {
      hol_error_set(error, HOL_ERR_IO, "cannot create directory %s", copy);
      return -1;
    }
    *cursor = '/';
  }
  return 0;
}

int hol_atomic_write(const char *path, const char *data, size_t length,
                     hol_error *error) {
  if (make_parent_directories(path, error) < 0) return -1;
  char temporary[4096];
  int written = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
  if (written < 0 || (size_t)written >= sizeof(temporary)) {
    hol_error_set(error, HOL_ERR_PATH, "temporary path is too long");
    return -1;
  }
  int descriptor = mkstemp(temporary);
  if (descriptor < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot create temporary file");
    return -1;
  }
  size_t offset = 0U;
  while (offset < length) {
    ssize_t count = write(descriptor, data + offset, length - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      hol_error_set(error, HOL_ERR_IO, "cannot write temporary file");
      (void)close(descriptor);
      (void)unlink(temporary);
      return -1;
    }
    offset += (size_t)count;
  }
  if (fsync(descriptor) < 0 || close(descriptor) < 0 || rename(temporary, path) < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot publish atomic file");
    (void)unlink(temporary);
    return -1;
  }
  return 0;
}

char *hol_read_text(const char *path, size_t maximum, size_t *length,
                    hol_error *error) {
  int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot open %s", path);
    return NULL;
  }
  struct stat status;
  if (fstat(descriptor, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 || (uintmax_t)status.st_size > maximum) {
    hol_error_set(error, HOL_ERR_IO, "invalid or oversized file: %s", path);
    (void)close(descriptor);
    return NULL;
  }
  size_t size = (size_t)status.st_size;
  char *data = calloc(size + 1U, 1U);
  if (data == NULL) {
    hol_error_set(error, HOL_ERR_IO, "out of memory reading %s", path);
    (void)close(descriptor);
    return NULL;
  }
  size_t offset = 0U;
  while (offset < size) {
    ssize_t count = read(descriptor, data + offset, size - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      hol_error_set(error, HOL_ERR_IO, "short read from %s", path);
      free(data);
      (void)close(descriptor);
      return NULL;
    }
    offset += (size_t)count;
  }
  (void)close(descriptor);
  if (length != NULL) *length = size;
  return data;
}

int hol_copy_file_if_missing(const char *source, const char *target,
                             hol_error *error) {
  struct stat existing;
  if (lstat(target, &existing) == 0) {
    if (!S_ISREG(existing.st_mode) || existing.st_nlink != 1) {
      hol_error_set(error, HOL_ERR_PATH, "workspace target is not a file");
      return -1;
    }
    return 0;
  }
  if (errno != ENOENT || make_parent_directories(target, error) < 0) return -1;
  size_t length = 0U;
  char *content = hol_read_text(source, HOL_OUTPUT_MAX, &length, error);
  if (content == NULL) return -1;
  int descriptor = open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    if (errno == EEXIST) {
      free(content);
      return 0;
    }
    hol_error_set(error, HOL_ERR_IO, "cannot create workspace file");
    free(content);
    return -1;
  }
  size_t offset = 0U;
  while (offset < length) {
    ssize_t count = write(descriptor, content + offset, length - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      hol_error_set(error, HOL_ERR_IO, "cannot copy workspace file");
      (void)close(descriptor);
      (void)unlink(target);
      free(content);
      return -1;
    }
    offset += (size_t)count;
  }
  free(content);
  if (close(descriptor) < 0) {
    hol_error_set(error, HOL_ERR_IO, "cannot close workspace file");
    return -1;
  }
  return 0;
}

static bool parse_version(const char *version, unsigned parts[3]) {
  const char *cursor = version;
  for (size_t index = 0U; index < 3U; index++) {
    if (*cursor < '0' || *cursor > '9') return false;
    unsigned value = 0U;
    while (*cursor >= '0' && *cursor <= '9') {
      unsigned digit = (unsigned)(*cursor - '0');
      if (value > (UINT32_MAX - digit) / 10U) return false;
      value = value * 10U + digit;
      cursor++;
    }
    parts[index] = value;
    if (index < 2U) {
      if (*cursor != '.') return false;
      cursor++;
    }
  }
  return *cursor == '\0';
}

bool hol_version_supported(const char *minimum_version) {
  unsigned current[3];
  unsigned minimum[3];
  if (!parse_version(HOL_APP_VERSION, current) ||
      !parse_version(minimum_version, minimum)) return false;
  for (size_t index = 0U; index < 3U; index++) {
    if (current[index] > minimum[index]) return true;
    if (current[index] < minimum[index]) return false;
  }
  return true;
}
