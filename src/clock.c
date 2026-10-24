#define _GNU_SOURCE
#include "../waycentral.h"
#include "util.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

long wc_uptime(void) {
  FILE *file = fopen("/proc/uptime", "r");
  if (!file)
    return -1;

  double uptime = 0;
  int ok = fscanf(file, "%lf", &uptime) == 1;
  fclose(file);

  return ok ? (long)uptime : -1;
}

long wc_system_age_days(void) {
  struct statx stx;
  if (statx(AT_FDCWD, "/", 0, STATX_BTIME, &stx) != 0 ||
      !(stx.stx_mask & STATX_BTIME))
    return -1;

  long now = (long)time(NULL);
  return (now - (long)stx.stx_btime.tv_sec) / (24 * 3600);
}

static void format_uptime(char *out, size_t size, long uptime) {
  if (uptime < 0) {
    snprintf(out, size, "Unknown");
    return;
  }

  long days = uptime / (24 * 3600);
  int hours = (int)(uptime % (24 * 3600) / 3600);
  int minutes = (int)(uptime % 3600 / 60);
  int seconds = (int)(uptime % 60);

  if (days != 0)
    snprintf(out, size, "%ld day/s %02d:%02d:%02d", days, hours, minutes,
             seconds);
  else
    snprintf(out, size, "%02d:%02d:%02d", hours, minutes, seconds);
}

static void format_time(char *out, size_t size, enum wc_clock_format format) {
  time_t now = time(NULL);
  struct tm local;
  localtime_r(&now, &local);

  switch (format) {
  case WC_CLOCK_24H:
    strftime(out, size, "%H:%M:%S", &local);
    break;
  case WC_CLOCK_12H_AMPM:
    strftime(out, size, "%I:%M:%S %p", &local);
    break;
  case WC_CLOCK_12H:
    strftime(out, size, "%I:%M:%S", &local);
    break;
  default:
    snprintf(out, size, "NULL");
  }
}

char *wc_clock_json(enum wc_clock_format format) {
  char clock[64], uptime[64], birth[64];

  format_time(clock, sizeof(clock), format);
  format_uptime(uptime, sizeof(uptime), wc_uptime());

  long age = wc_system_age_days();
  if (age < 0)
    snprintf(birth, sizeof(birth), "Unknown");
  else
    snprintf(birth, sizeof(birth), "%ld day/s", age);

  struct wc_buf json = {0};
  wc_buf_appendf(&json,
                 "{\"text\":\"%s\",\"tooltip\":\"Uptime:\\t%s\\nBirth:\\t%s\"}",
                 clock, uptime, birth);
  return wc_buf_finish(&json);
}
