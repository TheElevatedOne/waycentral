#include "sysmon/cpu.h"
#include "sysmon/mem.h"
#include "sysmon/net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* input_unit: 0 -> Byte, 1 -> kiB, 2 -> MiB, 3 -> GiB
 * num: number to convert */
static char *unitConversion(const int input_unit, long double num,
                            int rounding) {
  const char units[5][4] = {"B", "kiB", "MiB", "GiB", "TiB"};
  char *output = malloc(64);

  int count = input_unit;

  while (num >= 1024) {
    num /= 1024;
    count++;
  }

  char *format = malloc(64);
  sprintf(format, "%%.%dLf %%s", rounding);

  sprintf(output, format, num, units[count]);
  return output;
}

void system_monitor(int type) {
  if (type == 0) {
    struct cpu_usage_t *cpu = get_cpu_usage(1000);
    char *json = malloc(256);
    sprintf(json, "{\"text\":\"%d%%\",\"tooltip\":\"", cpu->usage[0]);

    for (int i = 1; i < cpu->count; i++) {
      char *temp = malloc(128);
      sprintf(temp, "CPU%d:\\t%d%%", i - 1, cpu->usage[i]);
      if (i != cpu->count - 1) {
        sprintf(temp, "%s\\n", temp);
      } else {
        sprintf(temp, "%s\"}", temp);
      }
      strcat(json, temp);
      free(temp);
    }
    printf("%s\n", json);
  } else if (type == 1) {
    struct meminfo_t *mem = read_meminfo();
    char *json = malloc(512);

    int mem_used_title = (int)((mem->mem_used * 100) / mem->mem_total);
    float mem_used = ((float)mem->mem_used * 100) / (float)mem->mem_total;
    float mem_free = ((float)mem->mem_free * 100) / (float)mem->mem_total;
    float mem_avail = ((float)mem->mem_available * 100) / (float)mem->mem_total;
    float mem_buff = ((float)mem->buffers * 100) / (float)mem->mem_total;
    float mem_cache = ((float)mem->cached * 100) / (float)mem->mem_total;
    float mem_shared = ((float)mem->shared_mem * 100) / (float)mem->mem_total;
    float swap_used = ((float)mem->swap_used * 100) / (float)mem->swap_total;
    float swap_free = ((float)mem->swap_free * 100) / (float)mem->swap_total;

    sprintf(
        json,
        "{\"text\":\"%d%%\",\"tooltip\":\"<b>RAM:</b>\\nTotal:\\t%s\\nUsed:\\t"
        "%s (%.1f%%)\\nFree:\\t%s (%.1f%%)\\nAvailable:\\t%s "
        "(%.1f%%)\\nBuffered:\\t%s (%.1f%%)\\nCached:\\t%s "
        "(%.1f%%)\\nShared:\\t%s "
        "(%.1f%%)\\n\\n<b>SWAP:</b>\\nTotal:\\t%s\\nUsed:\\t"
        "%s (%.1f%%)\\nFree:\\t%s (%.1f%%)\"}",
        mem_used_title, unitConversion(1, mem->mem_total, 1),
        unitConversion(1, mem->mem_used, 1), mem_used,
        unitConversion(1, mem->mem_free, 1), mem_free,
        unitConversion(1, mem->mem_available, 1), mem_avail,
        unitConversion(1, mem->buffers, 1), mem_buff,
        unitConversion(1, mem->cached, 1), mem_cache,
        unitConversion(1, mem->shared_mem, 1), mem_shared,
        unitConversion(1, mem->swap_total, 1),
        unitConversion(1, mem->swap_used, 1), swap_used,
        unitConversion(1, mem->swap_free, 1), swap_free);

    printf("%s\n", json);
  } else if (type == 2) {
    struct netstat_t *net = get_network_stats(1000);
    char *json = malloc(128);

    sprintf(
        json,
        "{\"text\":\"%s/s\",\"tooltip\":\" %s/s\\t[%s]\\n %s/s\\t[%s]\"}",
        unitConversion(0, net->down_speed + net->up_speed, 0),
        unitConversion(0, net->down_speed, 0),
        unitConversion(0, net->download, 2),
        unitConversion(0, net->up_speed, 0), unitConversion(0, net->upload, 2));
    printf("%s\n", json);
  } else {
    printf("Not Implemented");
  }
}
