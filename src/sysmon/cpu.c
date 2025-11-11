#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CPUS 256

typedef struct {
  unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
  unsigned long long total;
} cpu_stat_t;

struct cpu_usage_t {
  int count;
  int *usage;
};

// Read CPU stats from /proc/stat
static int read_cpu_stats(cpu_stat_t *stats, int *cpu_count) {
  FILE *fp = fopen("/proc/stat", "r");
  if (!fp)
    return -1;

  char line[1024];
  int count = 0;

  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "cpu", 3) != 0)
      break;

    if (count >= MAX_CPUS)
      break;

    char label[10];
    sscanf(line, "%s %llu %llu %llu %llu %llu %llu %llu %llu", label,
           &stats[count].user, &stats[count].nice, &stats[count].system,
           &stats[count].idle, &stats[count].iowait, &stats[count].irq,
           &stats[count].softirq, &stats[count].steal);

    stats[count].total = stats[count].user + stats[count].nice +
                         stats[count].system + stats[count].idle +
                         stats[count].iowait + stats[count].irq +
                         stats[count].softirq + stats[count].steal;

    count++;
  }

  fclose(fp);
  *cpu_count = count;
  return 0;
}

// Get CPU usage (returns dynamically allocated struct)
struct cpu_usage_t *get_cpu_usage(int interval_ms) {
  cpu_stat_t prev[MAX_CPUS], curr[MAX_CPUS];
  int cpu_count;

  if (read_cpu_stats(prev, &cpu_count) != 0)
    return NULL;

  struct timespec ts;
  ts.tv_sec = interval_ms / 1000;
  ts.tv_nsec = (interval_ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);

  if (read_cpu_stats(curr, &cpu_count) != 0)
    return NULL;

  struct cpu_usage_t *result = malloc(sizeof(struct cpu_usage_t));
  if (!result)
    return NULL;

  result->count = cpu_count;
  result->usage = calloc(cpu_count, sizeof(int));
  if (!result->usage) {
    free(result);
    return NULL;
  }

  for (int i = 0; i < cpu_count; i++) {
    unsigned long long total_diff = curr[i].total - prev[i].total;
    unsigned long long idle_diff = curr[i].idle - prev[i].idle;

    int usage = 0;
    if (total_diff > 0)
      usage = (int)(100 * (total_diff - idle_diff) / total_diff);

    result->usage[i] = usage;
  }

  return result;
}

// Free the allocated struct
void free_cpu_usage(struct cpu_usage_t *data) {
  if (!data)
    return;
  free(data->usage);
  free(data);
}
