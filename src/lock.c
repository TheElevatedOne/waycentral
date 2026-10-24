#include "../waycentral.h"
#include "util.h"
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static int test_bit(const unsigned long *bits, int bit) {
  return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

static int has_capability(int fd, int ev_type, int code) {
  unsigned long bits[NLONGS(KEY_MAX + 1)] = {0};

  if (ioctl(fd, EVIOCGBIT(ev_type, sizeof(bits)), bits) < 0)
    return 0;

  return test_bit(bits, code);
}

static int open_keyboard(void) {
  char path[64];

  // Try up to 32 event devices
  for (int i = 0; i < 32; i++) {
    snprintf(path, sizeof(path), "/dev/input/event%d", i);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      continue;

    if (has_capability(fd, EV_KEY, KEY_A) &&
        has_capability(fd, EV_LED, LED_CAPSL))
      return fd;

    close(fd);
  }

  return -1;
}

int wc_lock_keys_get(struct wc_lock_keys *out) {
  int fd = open_keyboard();
  if (fd < 0)
    return -1;

  unsigned long leds[NLONGS(LED_MAX + 1)] = {0};
  int r = ioctl(fd, EVIOCGLED(sizeof(leds)), leds);
  close(fd);
  if (r < 0)
    return -1;

  out->caps = test_bit(leds, LED_CAPSL);
  out->num = test_bit(leds, LED_NUML);
  out->scroll = test_bit(leds, LED_SCROLLL);
  return 0;
}

char *wc_lock_key_json(enum wc_lock_key key) {
  static const char *names[] = {"Capslock", "Numlock", "Scrolllock"};
  if (key < WC_KEY_CAPS || key > WC_KEY_SCROLL)
    return NULL;

  struct wc_buf json = {0};
  struct wc_lock_keys keys;

  if (wc_lock_keys_get(&keys) != 0) {
    wc_buf_appendf(&json,
                   "{\"class\":\"unknown\",\"tooltip\":\"%s Unknown "
                   "(Keyboard Not Found)\"}",
                   names[key]);
    return wc_buf_finish(&json);
  }

  int on = (key == WC_KEY_CAPS)  ? keys.caps
           : (key == WC_KEY_NUM) ? keys.num
                                 : keys.scroll;

  wc_buf_appendf(&json, "{\"class\":\"%s\",\"tooltip\":\"%s %s\"}",
                 on ? "on" : "off", names[key], on ? "On" : "Off");
  return wc_buf_finish(&json);
}
