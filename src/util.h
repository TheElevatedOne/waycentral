#ifndef UTIL_H_
#define UTIL_H_

#include <stddef.h>

/* Growable string buffer used to build the JSON output. */
struct wc_buf {
  char *data;
  size_t len;
  size_t cap;
  int failed;
};

void wc_buf_appendf(struct wc_buf *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Appends text from outside sources escaped for a JSON string holding Pango
 * markup. */
void wc_buf_append_text(struct wc_buf *buf, const char *text);

/* Returns the built string (caller frees), or NULL if any append failed. */
char *wc_buf_finish(struct wc_buf *buf);

/* Formats a size into out, e.g. "1.5 GiB".
 * input_unit: 0 -> B, 1 -> kiB, 2 -> MiB, 3 -> GiB */
void wc_format_size(char *out, size_t size, int input_unit,
                    long double num, int decimals);

int wc_read_int(const char *path, int *out);
int wc_read_long(const char *path, long *out);
int wc_read_str(const char *path, char *buf, size_t size);

void wc_sleep_ms(int ms);

#endif // !UTIL_H_
