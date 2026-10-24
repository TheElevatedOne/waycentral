#include "../../waycentral.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CPUS 1024

typedef struct {
  unsigned long long idle;
  unsigned long long total;
} cpu_stat_t;

// Read CPU stats from /proc/stat, returns the number of entries or -1
static int read_cpu_stats(cpu_stat_t *stats, int max) {
  FILE *fp = fopen("/proc/stat", "r");
  if (!fp)
    return -1;

  char line[1024];
  int count = 0;

  while (count < max && fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "cpu", 3) != 0)
      break;

    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0,
                       irq = 0, softirq = 0, steal = 0;
    if (sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu", &user,
               &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4)
      continue;

    stats[count].idle = idle + iowait;
    stats[count].total =
        user + nice + system + idle + iowait + irq + softirq + steal;
    count++;
  }

  fclose(fp);
  return count;
}

struct wc_cpu_usage *wc_cpu_usage_get(int interval_ms) {
  cpu_stat_t *prev = calloc(MAX_CPUS, sizeof(cpu_stat_t));
  cpu_stat_t *curr = calloc(MAX_CPUS, sizeof(cpu_stat_t));
  struct wc_cpu_usage *result = NULL;

  if (!prev || !curr)
    goto out;

  int prev_count = read_cpu_stats(prev, MAX_CPUS);
  if (prev_count <= 0)
    goto out;

  wc_sleep_ms(interval_ms);

  int cpu_count = read_cpu_stats(curr, MAX_CPUS);
  if (cpu_count <= 0)
    goto out;
  if (cpu_count > prev_count)
    cpu_count = prev_count; // a CPU came online in between

  result = malloc(sizeof(*result));
  if (!result)
    goto out;

  result->count = cpu_count;
  result->usage = calloc(cpu_count, sizeof(int));
  if (!result->usage) {
    free(result);
    result = NULL;
    goto out;
  }

  for (int i = 0; i < cpu_count; i++) {
    if (curr[i].total <= prev[i].total || curr[i].idle < prev[i].idle)
      continue;
    unsigned long long total_diff = curr[i].total - prev[i].total;
    unsigned long long idle_diff = curr[i].idle - prev[i].idle;
    if (idle_diff > total_diff)
      idle_diff = total_diff;

    result->usage[i] = (int)(100 * (total_diff - idle_diff) / total_diff);
  }

out:
  free(prev);
  free(curr);
  return result;
}

void wc_cpu_usage_free(struct wc_cpu_usage *usage) {
  if (!usage)
    return;
  free(usage->usage);
  free(usage);
}
