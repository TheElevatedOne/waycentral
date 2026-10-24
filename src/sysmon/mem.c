#include "../../waycentral.h"
#include <stdio.h>
#include <string.h>

int wc_meminfo_get(struct wc_meminfo *out) {
  FILE *fp = fopen("/proc/meminfo", "r");
  if (!fp)
    return -1;

  memset(out, 0, sizeof(*out));

  // Not every line has a unit, so parse line by line
  char line[256];
  char key[64];
  unsigned long long value;
  unsigned long long sreclaimable = 0;

  while (fgets(line, sizeof(line), fp)) {
    if (sscanf(line, "%63s %llu", key, &value) != 2)
      continue;

    if (strcmp(key, "MemTotal:") == 0)
      out->mem_total = value;
    else if (strcmp(key, "MemFree:") == 0)
      out->mem_free = value;
    else if (strcmp(key, "MemAvailable:") == 0)
      out->mem_available = value;
    else if (strcmp(key, "Buffers:") == 0)
      out->buffers = value;
    else if (strcmp(key, "Cached:") == 0)
      out->cached = value;
    else if (strcmp(key, "SReclaimable:") == 0)
      sreclaimable = value;
    else if (strcmp(key, "Shmem:") == 0)
      out->shared = value;
    else if (strcmp(key, "SwapTotal:") == 0)
      out->swap_total = value;
    else if (strcmp(key, "SwapFree:") == 0)
      out->swap_free = value;
  }

  fclose(fp);

  if (out->mem_total == 0)
    return -1;

  // Same definition of used memory as free(1)
  unsigned long long cache = out->cached + sreclaimable;
  unsigned long long not_used = out->mem_free + out->buffers + cache;
  out->mem_used = (out->mem_total > not_used) ? out->mem_total - not_used : 0;
  out->swap_used =
      (out->swap_total > out->swap_free) ? out->swap_total - out->swap_free : 0;

  return 0;
}
