#include "util.h"
#include "../waycentral.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef WC_VERSION
#define WC_VERSION "unknown"
#endif

const char *wc_version(void) { return WC_VERSION; }

static int buf_reserve(struct wc_buf *buf, size_t extra) {
  if (buf->failed)
    return -1;
  if (buf->len + extra + 1 <= buf->cap)
    return 0;

  size_t cap = buf->cap ? buf->cap : 256;
  while (cap < buf->len + extra + 1)
    cap *= 2;

  char *data = realloc(buf->data, cap);
  if (!data) {
    buf->failed = 1;
    return -1;
  }
  buf->data = data;
  buf->cap = cap;
  return 0;
}

void wc_buf_appendf(struct wc_buf *buf, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  if (n < 0) {
    buf->failed = 1;
    return;
  }
  if (buf_reserve(buf, (size_t)n) != 0)
    return;

  va_start(ap, fmt);
  vsnprintf(buf->data + buf->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  buf->len += (size_t)n;
}

void wc_buf_append_text(struct wc_buf *buf, const char *text) {
  for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
    switch (*c) {
    case '"':
      wc_buf_appendf(buf, "\\\"");
      break;
    case '\\':
      wc_buf_appendf(buf, "\\\\");
      break;
    case '\n':
      wc_buf_appendf(buf, "\\n");
      break;
    case '\t':
      wc_buf_appendf(buf, "\\t");
      break;
    case '&':
      wc_buf_appendf(buf, "&amp;");
      break;
    case '<':
      wc_buf_appendf(buf, "&lt;");
      break;
    case '>':
      wc_buf_appendf(buf, "&gt;");
      break;
    default:
      if (*c < 0x20)
        wc_buf_appendf(buf, "\\u%04x", *c);
      else
        wc_buf_appendf(buf, "%c", *c);
    }
  }
}

char *wc_buf_finish(struct wc_buf *buf) {
  if (buf->failed || !buf->data) {
    free(buf->data);
    return NULL;
  }
  return buf->data;
}

void wc_format_size(char *out, size_t size, int input_unit,
                    long double num, int decimals) {
  static const char *units[] = {"B", "kiB", "MiB", "GiB", "TiB", "PiB"};
  const int max_unit = (int)(sizeof(units) / sizeof(units[0])) - 1;

  int unit = input_unit;
  while (num >= 1024 && unit < max_unit) {
    num /= 1024;
    unit++;
  }

  snprintf(out, size, "%.*Lf %s", decimals, num, units[unit]);
}

int wc_read_int(const char *path, int *out) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  int ok = fscanf(f, "%d", out) == 1;
  fclose(f);
  return ok ? 0 : -1;
}

int wc_read_long(const char *path, long *out) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  int ok = fscanf(f, "%ld", out) == 1;
  fclose(f);
  return ok ? 0 : -1;
}

int wc_read_str(const char *path, char *buf, size_t size) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  if (!fgets(buf, (int)size, f)) {
    fclose(f);
    return -1;
  }
  buf[strcspn(buf, "\n")] = '\0';
  fclose(f);
  return 0;
}

void wc_sleep_ms(int ms) {
  if (ms <= 0)
    return;
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
    ;
}
