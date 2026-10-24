#include "../waycentral.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES 32
#define RESCAN_MS 5000

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

struct wc_lock_monitor {
  struct pollfd fds[MAX_DEVICES];
  int count;
  long long last_scan_ms;
};

static int test_bit(const unsigned long *bits, int bit) {
  return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

static int has_capability(int fd, int ev_type, int code) {
  unsigned long bits[NLONGS(KEY_MAX + 1)] = {0};

  if (ioctl(fd, EVIOCGBIT(ev_type, sizeof(bits)), bits) < 0)
    return 0;

  return test_bit(bits, code);
}

static int is_keyboard(int fd) {
  return has_capability(fd, EV_KEY, KEY_A) &&
         has_capability(fd, EV_LED, LED_CAPSL);
}

/* Opens keyboards from /dev/input/event0..31 into fds, returns the count */
static int open_keyboards(struct pollfd *fds, int max, int flags) {
  char path[64];
  int count = 0;

  for (int i = 0; i < MAX_DEVICES && count < max; i++) {
    snprintf(path, sizeof(path), "/dev/input/event%d", i);

    int fd = open(path, O_RDONLY | O_CLOEXEC | flags);
    if (fd < 0)
      continue;

    if (is_keyboard(fd)) {
      fds[count].fd = fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      count++;
    } else {
      close(fd);
    }
  }

  return count;
}

static int read_leds(int fd, struct wc_lock_keys *out) {
  unsigned long leds[NLONGS(LED_MAX + 1)] = {0};
  if (ioctl(fd, EVIOCGLED(sizeof(leds)), leds) < 0)
    return -1;

  out->caps = test_bit(leds, LED_CAPSL);
  out->num = test_bit(leds, LED_NUML);
  out->scroll = test_bit(leds, LED_SCROLLL);
  return 0;
}

int wc_lock_keys_get(struct wc_lock_keys *out) {
  struct pollfd kb;
  if (open_keyboards(&kb, 1, 0) != 1)
    return -1;

  int r = read_leds(kb.fd, out);
  close(kb.fd);
  return r;
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void monitor_close_fds(struct wc_lock_monitor *mon) {
  for (int i = 0; i < mon->count; i++)
    close(mon->fds[i].fd);
  mon->count = 0;
}

static void monitor_rescan(struct wc_lock_monitor *mon) {
  monitor_close_fds(mon);
  mon->count = open_keyboards(mon->fds, MAX_DEVICES, O_NONBLOCK);
  mon->last_scan_ms = now_ms();
}

struct wc_lock_monitor *wc_lock_monitor_open(void) {
  struct wc_lock_monitor *mon = calloc(1, sizeof(*mon));
  if (mon)
    monitor_rescan(mon);
  return mon;
}

void wc_lock_monitor_close(struct wc_lock_monitor *mon) {
  if (!mon)
    return;
  monitor_close_fds(mon);
  free(mon);
}

/* Reads the current state from the first keyboard that answers */
static int monitor_read(struct wc_lock_monitor *mon,
                        struct wc_lock_keys *out) {
  for (int i = 0; i < mon->count; i++)
    if (read_leds(mon->fds[i].fd, out) == 0)
      return 0;
  return -1;
}

/* Drains pending events, returns 1 if any could change the lock state,
 * -1 if the device is gone */
static int drain_events(int fd) {
  struct input_event ev[64];
  int relevant = 0;

  for (;;) {
    ssize_t n = read(fd, ev, sizeof(ev));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return (errno == EAGAIN) ? relevant : -1;
    }
    if (n == 0)
      return -1;

    for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
      if (ev[i].type == EV_LED ||
          (ev[i].type == EV_KEY &&
           (ev[i].code == KEY_CAPSLOCK || ev[i].code == KEY_NUMLOCK ||
            ev[i].code == KEY_SCROLLLOCK)))
        relevant = 1;
    }
  }
}

int wc_lock_monitor_wait(struct wc_lock_monitor *mon,
                         struct wc_lock_keys *state, int timeout_ms) {
  long long deadline = (timeout_ms >= 0) ? now_ms() + timeout_ms : -1;

  for (;;) {
    long long now = now_ms();

    // Pick up hotplugged or reconnected keyboards
    if (mon->count == 0 || now - mon->last_scan_ms >= RESCAN_MS)
      monitor_rescan(mon);

    struct wc_lock_keys current;
    if (monitor_read(mon, &current) != 0) {
      monitor_rescan(mon);
      if (monitor_read(mon, &current) != 0)
        return -1;
    }

    if (current.caps != state->caps || current.num != state->num ||
        current.scroll != state->scroll) {
      *state = current;
      return 1;
    }

    if (deadline >= 0 && now >= deadline)
      return 0;

    // Sleep until an event, the next rescan or the deadline
    long long wake = mon->last_scan_ms + RESCAN_MS;
    if (deadline >= 0 && deadline < wake)
      wake = deadline;
    int wait = (wake > now) ? (int)(wake - now) : 0;

    int r = poll(mon->fds, (nfds_t)mon->count, wait);
    if (r < 0 && errno != EINTR)
      return -1;

    for (int i = 0; r > 0 && i < mon->count; i++) {
      if (!mon->fds[i].revents)
        continue;
      if ((mon->fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
          drain_events(mon->fds[i].fd) < 0)
        mon->last_scan_ms = 0; // device went away, rescan next round
    }

    /* The keyboard LED is updated by the compositor right after the key
     * press reaches it; a short pause catches that update. */
    if (r > 0)
      wc_sleep_ms(10);
  }
}

static const char *key_names[] = {"Capslock", "Numlock", "Scrolllock"};

char *wc_lock_key_json_from(enum wc_lock_key key,
                            const struct wc_lock_keys *state) {
  if (key < WC_KEY_CAPS || key > WC_KEY_SCROLL)
    return NULL;

  struct wc_buf json = {0};

  if (!state) {
    wc_buf_appendf(&json,
                   "{\"class\":\"unknown\",\"tooltip\":\"%s Unknown "
                   "(Keyboard Not Found)\"}",
                   key_names[key]);
    return wc_buf_finish(&json);
  }

  int on = (key == WC_KEY_CAPS)  ? state->caps
           : (key == WC_KEY_NUM) ? state->num
                                 : state->scroll;

  wc_buf_appendf(&json, "{\"class\":\"%s\",\"tooltip\":\"%s %s\"}",
                 on ? "on" : "off", key_names[key], on ? "On" : "Off");
  return wc_buf_finish(&json);
}

char *wc_lock_key_json(enum wc_lock_key key) {
  struct wc_lock_keys keys;
  if (wc_lock_keys_get(&keys) != 0)
    return wc_lock_key_json_from(key, NULL);
  return wc_lock_key_json_from(key, &keys);
}
