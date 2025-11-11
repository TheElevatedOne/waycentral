#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct netstat_t {
  unsigned long long upload;
  unsigned long long download;
  unsigned long long up_speed;
  unsigned long long down_speed;
};

static bool is_active_dev(const char *device) {
  const char inactive[7][7] = {"lo",    "docker", "veth", "br-",
                               "vmnet", "tun",    "tap"};

  for (int i = 0; i < 7; i++) {
    if (strncmp(device, inactive[i], strlen(inactive[i])) == 0) {
      return false;
    }
  }
  return true;
}

struct netstat_t *get_network_stats(int interval_ms) {
  unsigned long long rx[2], tx[2];
  struct netstat_t *stats = malloc(sizeof(struct netstat_t));

  for (int i = 0; i < 2; i++) {

    FILE *file = fopen("/proc/net/dev", "r");

    char *line = malloc(256);
    int count = 0;
    int max = 32;

    fgets(line, 256, file);
    fgets(line, 256, file);

    rx[i] = 0;
    tx[i] = 0;

    while (fgets(line, 256, file) && count < max) {
      char *name = malloc(64);
      unsigned long long rx_bytes, tx_bytes = 0;
      if (sscanf(line, " %31[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu", name,
                 &rx_bytes, &tx_bytes) == 3) {
        if (is_active_dev(name)) {
          rx[i] += rx_bytes;
          tx[i] += tx_bytes;
        }
      }
    }
    fclose(file);
    free(line);

    if (i == 0) {
      struct timespec ts;
      ts.tv_sec = interval_ms / 1000;
      ts.tv_nsec = (interval_ms % 1000) * 1000000L;
      nanosleep(&ts, NULL);
    }
  }

  stats->upload = tx[1];
  stats->download = rx[1];
  stats->up_speed = tx[1] - tx[0];
  stats->down_speed = rx[1] - rx[0];

  return stats;
}
