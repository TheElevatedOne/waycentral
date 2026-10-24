#include "../../waycentral.h"
#include "../util.h"
#include <stdio.h>
#include <string.h>

static int is_active_dev(const char *device) {
  static const char *inactive[] = {"lo",    "docker", "veth", "br-",
                                   "vmnet", "tun",    "tap"};

  for (size_t i = 0; i < sizeof(inactive) / sizeof(inactive[0]); i++) {
    if (strncmp(device, inactive[i], strlen(inactive[i])) == 0)
      return 0;
  }
  return 1;
}

static int read_totals(unsigned long long *rx, unsigned long long *tx) {
  FILE *file = fopen("/proc/net/dev", "r");
  if (!file)
    return -1;

  char line[512];
  *rx = 0;
  *tx = 0;

  // Skip the two header lines
  for (int i = 0; i < 2; i++) {
    if (!fgets(line, sizeof(line), file)) {
      fclose(file);
      return -1;
    }
  }

  while (fgets(line, sizeof(line), file)) {
    char name[64];
    unsigned long long rx_bytes = 0, tx_bytes = 0;
    if (sscanf(line, " %63[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu", name,
               &rx_bytes, &tx_bytes) == 3 &&
        is_active_dev(name)) {
      *rx += rx_bytes;
      *tx += tx_bytes;
    }
  }

  fclose(file);
  return 0;
}

int wc_netstat_get(struct wc_netstat *out, int interval_ms) {
  unsigned long long rx[2], tx[2];

  if (read_totals(&rx[0], &tx[0]) != 0)
    return -1;
  wc_sleep_ms(interval_ms);
  if (read_totals(&rx[1], &tx[1]) != 0)
    return -1;

  if (interval_ms <= 0)
    interval_ms = 1000;

  out->upload = tx[1];
  out->download = rx[1];
  // Counters drop when an interface goes away
  out->up_speed = (tx[1] > tx[0]) ? (tx[1] - tx[0]) * 1000 / interval_ms : 0;
  out->down_speed =
      (rx[1] > rx[0]) ? (rx[1] - rx[0]) * 1000 / interval_ms : 0;

  return 0;
}
