#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct lock_key_t {
  int caps;
  int num;
  int scroll;
};

static int has_capability(int fd, int ev_type, int code) {
  unsigned long bits[128] = {0}; // plenty big

  if (ioctl(fd, EVIOCGBIT(ev_type, sizeof(bits)), bits) < 0)
    return 0;

  return bits[code / (8 * sizeof(unsigned long))] &
         (1UL << (code % (8 * sizeof(unsigned long))));
}

static struct lock_key_t get_lock_keys(void) {
  struct lock_key_t keys;

  char path[64];
  int fd = -1;

  // Try up to 32 event devices
  for (int i = 0; i < 32; i++) {
    snprintf(path, sizeof(path), "/dev/input/event%d", i);

    int test_fd = open(path, O_RDONLY);
    if (test_fd < 0)
      continue;

    int is_keyboard = has_capability(test_fd, EV_KEY, KEY_A) &&
                      has_capability(test_fd, EV_LED, LED_CAPSL);

    if (is_keyboard) {
      fd = test_fd;
      break;
    }

    close(test_fd);
  }

  if (fd < 0) {
    fprintf(stderr, "Keyboard not found.\n");
    exit(1);
  }

  unsigned long leds = 0;
  if (ioctl(fd, EVIOCGLED(sizeof(leds)), &leds) < 0) {
    perror("ioctl");
    close(fd);
    exit(1);
  }

  keys.caps = (leds & (1 << LED_CAPSL)) ? 1 : 0;
  keys.num = (leds & (1 << LED_NUML)) ? 1 : 0;
  keys.scroll = (leds & (1 << LED_SCROLLL)) ? 1 : 0;

  close(fd);

  return keys;
}

void lock_keys(int type) {
  struct lock_key_t keys = get_lock_keys();

  // Caps Lock
  if (type == 0) {
    printf("{\"class\":\"%s\"}\n", (keys.caps == 1) ? "on" : "off");
    // Num Lock
  } else if (type == 1) {
    printf("{\"class\":\"%s\"}\n", (keys.num == 1) ? "on" : "off");
    // Scroll Lock
  } else if (type == 2) {
    printf("{\"class\":\"%s\"}\n", (keys.scroll == 1) ? "on" : "off");
  }
}
