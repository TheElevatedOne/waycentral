#include "../waycentral.h"
#include "util.h"
#include <stdio.h>

#define SIZE_BUF 32

static double percent(unsigned long long part, unsigned long long total) {
  return total ? (double)part * 100.0 / (double)total : 0.0;
}

char *wc_cpu_json(int interval_ms) {
  struct wc_cpu_usage *cpu = wc_cpu_usage_get(interval_ms);
  if (!cpu)
    return NULL;

  struct wc_buf json = {0};
  wc_buf_appendf(&json, "{\"text\":\"%d%%\",\"tooltip\":\"", cpu->usage[0]);

  for (int i = 1; i < cpu->count; i++)
    wc_buf_appendf(&json, "CPU%d:\\t%d%%%s", i - 1, cpu->usage[i],
                   (i != cpu->count - 1) ? "\\n" : "");

  wc_buf_appendf(&json, "\"}");
  wc_cpu_usage_free(cpu);
  return wc_buf_finish(&json);
}

char *wc_mem_json(void) {
  struct wc_meminfo mem;
  if (wc_meminfo_get(&mem) != 0)
    return NULL;

  char total[SIZE_BUF], used[SIZE_BUF], free_[SIZE_BUF], avail[SIZE_BUF],
      buff[SIZE_BUF], cache[SIZE_BUF], shared[SIZE_BUF], swap_total[SIZE_BUF],
      swap_used[SIZE_BUF], swap_free[SIZE_BUF];

  wc_format_size(total, SIZE_BUF, 1, mem.mem_total, 1);
  wc_format_size(used, SIZE_BUF, 1, mem.mem_used, 1);
  wc_format_size(free_, SIZE_BUF, 1, mem.mem_free, 1);
  wc_format_size(avail, SIZE_BUF, 1, mem.mem_available, 1);
  wc_format_size(buff, SIZE_BUF, 1, mem.buffers, 1);
  wc_format_size(cache, SIZE_BUF, 1, mem.cached, 1);
  wc_format_size(shared, SIZE_BUF, 1, mem.shared, 1);
  wc_format_size(swap_total, SIZE_BUF, 1, mem.swap_total, 1);
  wc_format_size(swap_used, SIZE_BUF, 1, mem.swap_used, 1);
  wc_format_size(swap_free, SIZE_BUF, 1, mem.swap_free, 1);

  struct wc_buf json = {0};
  wc_buf_appendf(
      &json,
      "{\"text\":\"%d%%\",\"tooltip\":\"<b>RAM:</b>\\nTotal:\\t%s\\nUsed:\\t"
      "%s (%.1f%%)\\nFree:\\t%s (%.1f%%)\\nAvail:\\t%s "
      "(%.1f%%)\\nBuff:\\t%s (%.1f%%)\\nCached:\\t%s "
      "(%.1f%%)\\nShared:\\t%s "
      "(%.1f%%)\\n\\n<b>SWAP:</b>\\nTotal:\\t%s\\nUsed:\\t"
      "%s (%.1f%%)\\nFree:\\t%s (%.1f%%)\"}",
      (int)percent(mem.mem_used, mem.mem_total), total, used,
      percent(mem.mem_used, mem.mem_total), free_,
      percent(mem.mem_free, mem.mem_total), avail,
      percent(mem.mem_available, mem.mem_total), buff,
      percent(mem.buffers, mem.mem_total), cache,
      percent(mem.cached, mem.mem_total), shared,
      percent(mem.shared, mem.mem_total), swap_total, swap_used,
      percent(mem.swap_used, mem.swap_total), swap_free,
      percent(mem.swap_free, mem.swap_total));
  return wc_buf_finish(&json);
}

char *wc_net_json(int interval_ms) {
  struct wc_netstat net;
  if (wc_netstat_get(&net, interval_ms) != 0)
    return NULL;

  char speed[SIZE_BUF], down_speed[SIZE_BUF], download[SIZE_BUF],
      up_speed[SIZE_BUF], upload[SIZE_BUF];

  wc_format_size(speed, SIZE_BUF, 0, net.down_speed + net.up_speed, 0);
  wc_format_size(down_speed, SIZE_BUF, 0, net.down_speed, 0);
  wc_format_size(download, SIZE_BUF, 0, net.download, 2);
  wc_format_size(up_speed, SIZE_BUF, 0, net.up_speed, 0);
  wc_format_size(upload, SIZE_BUF, 0, net.upload, 2);

  struct wc_buf json = {0};
  wc_buf_appendf(
      &json,
      "{\"text\":\"%s/s\",\"tooltip\":\" %s/s\\t[%s]\\n %s/s\\t[%s]\"}",
      speed, down_speed, download, up_speed, upload);
  return wc_buf_finish(&json);
}
