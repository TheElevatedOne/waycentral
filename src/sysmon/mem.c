#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct meminfo_t {
  unsigned long long mem_total;
  unsigned long long mem_free;
  unsigned long long mem_available;
  unsigned long long buffers;
  unsigned long long cached;
  unsigned long long swap_total;
  unsigned long long swap_free;
  unsigned long long swap_used;
  unsigned long long mem_used;
  unsigned long long shared_mem;
  int mem_percent;
  int swap_percent;
};

struct meminfo_t *read_meminfo() {
  FILE *fp = fopen("/proc/meminfo", "r");
  if (!fp) {
    perror("fopen");
    exit(1);
  }

  char key[64];
  unsigned long value;
  char unit[32];

  struct meminfo_t *mem = malloc(sizeof(struct meminfo_t));

  while (fscanf(fp, "%63s %lu %31s", key, &value, unit) == 3) {
    if (strcmp(key, "MemTotal:") == 0)
      mem->mem_total = (unsigned long long)value;
    else if (strcmp(key, "MemFree:") == 0)
      mem->mem_free = (unsigned long long)value;
    else if (strcmp(key, "MemAvailable:") == 0)
      mem->mem_available = (unsigned long long)value;
    else if (strcmp(key, "Buffers:") == 0)
      mem->buffers = (unsigned long long)value;
    else if (strcmp(key, "Cached:") == 0)
      mem->cached = (unsigned long long)value;
    else if (strcmp(key, "SwapTotal:") == 0)
      mem->swap_total = (unsigned long long)value;
    else if (strcmp(key, "SwapFree:") == 0)
      mem->swap_free = (unsigned long long)value;
  }

  fclose(fp);

  // Compute derived values
  mem->swap_used = mem->swap_total - mem->swap_free;
  mem->mem_used = mem->mem_total - mem->mem_free - mem->buffers - mem->cached;
  mem->shared_mem =
      mem->buffers; // approximate, Linux doesn't give direct "shared" easily

  return mem;
}
