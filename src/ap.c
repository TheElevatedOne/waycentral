#include "../waycentral.h"
#include "util.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define AP_CONF "/etc/create_ap.conf"

/* create_ap is a shell script, so look for an argument whose basename is
 * create_ap rather than matching the whole command line. */
static int cmdline_is_create_ap(const char *cmd, size_t n) {
  for (size_t i = 0; i < n; i += strlen(cmd + i) + 1) {
    const char *arg = cmd + i;
    const char *base = strrchr(arg, '/');
    base = base ? base + 1 : arg;
    if (strcmp(base, "create_ap") == 0)
      return 1;
  }
  return 0;
}

/* Returns 1 if create_ap is running, 0 if not, -1 on error */
static int is_active(void) {
  DIR *proc = opendir("/proc");
  if (!proc)
    return -1;

  int active = 0;
  struct dirent *ent;
  while (!active && (ent = readdir(proc)) != NULL) {
    // Only directories that are PIDs (all digits)
    if (!isdigit((unsigned char)ent->d_name[0]))
      continue;

    char path[288];
    snprintf(path, sizeof(path), "/proc/%s/cmdline", ent->d_name);

    FILE *f = fopen(path, "r");
    if (!f)
      continue;

    char cmd[4096];
    size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
    fclose(f);

    if (n == 0)
      continue;

    cmd[n] = '\0';
    active = cmdline_is_create_ap(cmd, n);
  }

  closedir(proc);
  return active;
}

/* Copies the value of "KEY=value" into out if line starts with key */
static int conf_value(const char *line, const char *key, char *out,
                      size_t size) {
  size_t klen = strlen(key);
  if (strncmp(line, key, klen) != 0 || line[klen] != '=')
    return 0;
  snprintf(out, size, "%s", line + klen + 1);
  return 1;
}

int wc_hotspot_get(struct wc_hotspot *out) {
  memset(out, 0, sizeof(*out));

  int active = is_active();
  if (active < 0)
    return -1;
  out->active = active;
  if (!active)
    return 0;

  FILE *ap_conf = fopen(AP_CONF, "r");
  if (!ap_conf)
    return 0;

  char line[512];
  char hidden[8] = "";
  while (fgets(line, sizeof(line), ap_conf)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (!conf_value(line, "SSID", out->ssid, sizeof(out->ssid)) &&
        !conf_value(line, "PASSPHRASE", out->passphrase,
                    sizeof(out->passphrase)) &&
        !conf_value(line, "FREQ_BAND", out->freq, sizeof(out->freq)) &&
        !conf_value(line, "WIFI_IFACE", out->wifi_iface,
                    sizeof(out->wifi_iface)) &&
        !conf_value(line, "INTERNET_IFACE", out->internet_iface,
                    sizeof(out->internet_iface)))
      conf_value(line, "HIDDEN", hidden, sizeof(hidden));
  }
  out->hidden = strcmp(hidden, "1") == 0;

  fclose(ap_conf);
  return 0;
}

char *wc_hotspot_json(void) {
  struct wc_hotspot ap;
  struct wc_buf json = {0};

  if (wc_hotspot_get(&ap) != 0 || !ap.active) {
    wc_buf_appendf(&json, "{\"class\":\"off\",\"alt\":\"off\",\"tooltip\":"
                          "\"Hotspot Inactive\"}");
    return wc_buf_finish(&json);
  }

  wc_buf_appendf(&json,
                 "{\"class\":\"on\",\"alt\":\"on\",\"tooltip\":\"<b>SSID:</"
                 "b>\\t");
  wc_buf_append_text(&json, ap.ssid);
  wc_buf_appendf(&json, "\\n<b>PASS:</b>\\t");
  wc_buf_append_text(&json, ap.passphrase);
  wc_buf_appendf(&json, "\\n<b>FREQ:</b>\\t");
  wc_buf_append_text(&json, ap.freq);
  wc_buf_appendf(&json, " GHz\\n<b>DEST:</b>\\t");
  wc_buf_append_text(&json, ap.wifi_iface);
  wc_buf_appendf(&json, "\\n<b>SRC:</b>\\t");
  wc_buf_append_text(&json, ap.internet_iface);
  wc_buf_appendf(&json, "\\n<b>HIDE:</b>\\t%s\"}", ap.hidden ? "Yes" : "No");
  return wc_buf_finish(&json);
}
