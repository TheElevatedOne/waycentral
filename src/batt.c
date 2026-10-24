#include "../waycentral.h"
#include "util.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SYS_PATH "/sys/class/power_supply"
#define PATH_BUF 512

static int starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void load_battery(struct wc_battery *b, const char *name) {
  char path[PATH_BUF];

  memset(b, 0, sizeof(*b));
  snprintf(b->name, sizeof(b->name), "%s", name);

  // Missing attributes are left empty / zero
  snprintf(path, sizeof(path), SYS_PATH "/%s/capacity", name);
  wc_read_int(path, &b->capacity);

  snprintf(path, sizeof(path), SYS_PATH "/%s/status", name);
  wc_read_str(path, b->status, sizeof(b->status));

  snprintf(path, sizeof(path), SYS_PATH "/%s/model_name", name);
  wc_read_str(path, b->model_name, sizeof(b->model_name));

  snprintf(path, sizeof(path), SYS_PATH "/%s/manufacturer", name);
  wc_read_str(path, b->manufacturer, sizeof(b->manufacturer));

  snprintf(path, sizeof(path), SYS_PATH "/%s/technology", name);
  wc_read_str(path, b->technology, sizeof(b->technology));

  snprintf(path, sizeof(path), SYS_PATH "/%s/voltage_now", name);
  wc_read_long(path, &b->voltage_now);

  snprintf(path, sizeof(path), SYS_PATH "/%s/current_now", name);
  wc_read_long(path, &b->current_now);

  // Try to find energy_full or charge_full
  static const char *charge_names[] = {"energy", "charge"};
  for (int i = 0; i < 2; ++i) {
    snprintf(path, sizeof(path), SYS_PATH "/%s/%s_full", name,
             charge_names[i]);
    if (access(path, F_OK) == 0) {
      wc_read_long(path, &b->full);
      snprintf(path, sizeof(path), SYS_PATH "/%s/%s_full_design", name,
               charge_names[i]);
      wc_read_long(path, &b->full_design);
      break;
    }
  }
}

int wc_battery_list(struct wc_battery *out, int max) {
  DIR *d = opendir(SYS_PATH);
  if (!d)
    return -1;

  struct dirent *entry;
  int count = 0;
  while (count < max && (entry = readdir(d))) {
    if (!starts_with(entry->d_name, "BAT"))
      continue;
    load_battery(&out[count], entry->d_name);
    count++;
  }

  closedir(d);
  return count;
}

static const char *battery_icon(int capacity, int charging) {
  static const char *icons[11] = {"󰂎", "󰁺", "󰁻", "󰁼", "󰁽", "󰁾",
                                  "󰁿", "󰂀", "󰂁", "󰂂", "󰁹"};
  static const char *icons_charge[11] = {"󰢟 ", "󰢜 ", "󰂆 ", "󰂇 ",
                                         "󰂈 ", "󰢝 ", "󰂉 ", "󰢞 ",
                                         "󰂊 ", "󰂋 ", "󰂅 "};

  int level = capacity / 10;
  if (level < 0)
    level = 0;
  else if (level > 10)
    level = 10;

  return charging ? icons_charge[level] : icons[level];
}

char *wc_battery_json(void) {
  struct wc_battery bat[WC_MAX_BATTERIES];
  int n = wc_battery_list(bat, WC_MAX_BATTERIES);

  struct wc_buf json = {0};

  if (n <= 0) {
    wc_buf_appendf(&json,
                   "{\"text\":\"\",\"tooltip\":\"No Battery\",\"class\":"
                   "\"none\"}");
    return wc_buf_finish(&json);
  }

  int charging = strcmp(bat[0].status, "Charging") == 0;

  int main_capacity = 0;
  for (int i = 0; i < n; i++)
    main_capacity += bat[i].capacity;
  main_capacity /= n;

  wc_buf_appendf(&json, "{\"text\":\"%d%% %s\",\"tooltip\":\"", main_capacity,
                 battery_icon(main_capacity, charging));

  for (int i = 0; i < n; i++) {
    const struct wc_battery *b = &bat[i];

    double voltage = b->voltage_now / 1e6;
    double current = b->current_now / 1e6;
    double power = voltage * current;

    if (!charging && power != 0.0)
      power *= -1;

    long health = (b->full_design > 0) ? (b->full * 100) / b->full_design : 0;

    wc_buf_appendf(&json, "<b>");
    wc_buf_append_text(&json, b->name);
    wc_buf_appendf(&json, ":</b>\\t");
    wc_buf_append_text(&json, b->model_name);
    wc_buf_appendf(&json, "\\nManuf:\\t");
    wc_buf_append_text(&json, b->manufacturer);
    wc_buf_appendf(&json, "\\nTech:\\t");
    wc_buf_append_text(&json, b->technology);
    wc_buf_appendf(&json, "\\nStatus:\\t");
    wc_buf_append_text(&json, b->status);
    wc_buf_appendf(&json,
                   "\\nCharge:\\t%d%%\\nHealth:\\t%ld%%\\nVolt:\\t%.1f "
                   "V\\nCurr:\\t%.1f A\\nPower:\\t%.1f W",
                   b->capacity, health, voltage, current, power);

    if (i != n - 1)
      wc_buf_appendf(&json, "\\n\\n");
  }

  wc_buf_appendf(&json, "\"}");
  return wc_buf_finish(&json);
}
