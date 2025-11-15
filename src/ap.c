#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct hotspot_t {
  char ssid[256];
  char passwd[256];
  char freq[8];
  char ap_iface[128];
  char eth_iface[128];
  int hidden;
  int class;
};

static int is_active(void) {
  int is_active = 0;

  const char *needle = "create_ap";
  DIR *proc = opendir("/proc");
  if (!proc) {
    perror("opendir");
    return 1;
  }

  struct dirent *ent;
  while ((ent = readdir(proc)) != NULL) {
    // Only directories that are PIDs (all digits)
    if (!isdigit(ent->d_name[0]))
      continue;

    char path[256];
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

    // /proc/<pid>/cmdline is null-separated; change to spaces for easier
    // searching
    for (size_t i = 0; i < n; i++)
      if (cmd[i] == '\0')
        cmd[i] = ' ';

    if (strstr(cmd, needle)) {
      is_active = 1;
    }
  }

  closedir(proc);

  return is_active;
}

static struct hotspot_t parse_ap(void) {
  struct hotspot_t ap;
  ap.hidden = 0;
  ap.class = 0;

  if (is_active() == 0) {
    ap.class = 1;
    return ap;
  } else if (is_active() == 1) {
    FILE *ap_conf = fopen("/etc/create_ap.conf", "r");
    if (!ap_conf) {
      ap.class = 1;
      return ap;
    }

    char line[256];
    while (fgets(line, sizeof(line), ap_conf)) {
      sscanf(line, "SSID=%s", ap.ssid);
      sscanf(line, "PASSPHRASE=%s", ap.passwd);
      sscanf(line, "FREQ_BAND=%s", ap.freq);
      sscanf(line, "WIFI_IFACE=%s", ap.ap_iface);
      sscanf(line, "INTERNET_IFACE=%s", ap.eth_iface);
      if (strcmp(line, "HIDDEN=1\n") == 0) {
        ap.hidden = 1;
      }
    }

    fclose(ap_conf);
  }
  return ap;
}

void get_hotspot(void) {
  struct hotspot_t ap = parse_ap();

  if (ap.class != 0) {
    printf("{\"class\":\"off\",\"percentage\":\"0\",\"tooltip\":\"Hotspot "
           "Inactive\"}\n");
  } else {
    printf("{\"class\":\"on\",\"percentage\":\"100\",\"tooltip\":\"<b>SSID:</"
           "b>\\t%s\\n<b>PASS:</b>\\t%s\\n\\n<b>FREQ:</"
           "b>\\t%s\\n<b>AP_IFACE:</b>\\t%s\\n<b>NET_IFACE:</"
           "b>\\t%s\\n<b>HIDDEN:</b>\\t%s\"}\n",
           ap.ssid, ap.passwd, ap.freq, ap.ap_iface, ap.eth_iface,
           (ap.hidden == 1) ? "Yes" : "No");
  }
}
