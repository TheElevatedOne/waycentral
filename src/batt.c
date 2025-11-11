#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_BATTERIES 8
#define SYS_PATH "/sys/class/power_supply"
#define PATH_BUF 512

struct battery {
  char name[64];
  int capacity;
  char status[32];
  long full;
  long full_design;
  long voltage_now;
  long current_now;
  char model_name[128];
  char manufacturer[128];
  char technology[64];
  int present;
};

static int read_int(const char *path, int *out) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  if (fscanf(f, "%d", out) != 1) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int read_long(const char *path, long *out) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  if (fscanf(f, "%ld", out) != 1) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int read_str(const char *path, char *buf, size_t bufsize) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  if (!fgets(buf, (int)bufsize, f)) {
    fclose(f);
    return -1;
  }
  buf[strcspn(buf, "\n")] = '\0';
  fclose(f);
  return 0;
}

static int starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int load_batteries(struct battery *out, int max) {
  DIR *d = opendir(SYS_PATH);
  if (!d) {
    perror("opendir");
    return -1;
  }

  struct dirent *entry;
  int count = 0;
  while ((entry = readdir(d)) && count < max) {
    if (!starts_with(entry->d_name, "BAT"))
      continue;

    char path[PATH_BUF];
    struct battery *b = &out[count];
    memset(b, 0, sizeof(*b));
    b->present = 1;
    snprintf(b->name, sizeof(b->name), "%s", entry->d_name);

    // capacity
    snprintf(path, sizeof(path), SYS_PATH "/%s/capacity", entry->d_name);
    read_int(path, &b->capacity); // ignore errors for a quick demo

    // status
    snprintf(path, sizeof(path), SYS_PATH "/%s/status", entry->d_name);
    read_str(path, b->status, sizeof(b->status));

    // model_name
    snprintf(path, sizeof(path), SYS_PATH "/%s/model_name", entry->d_name);
    read_str(path, b->model_name, sizeof(b->model_name));

    // manufacturer
    snprintf(path, sizeof(path), SYS_PATH "/%s/manufacturer", entry->d_name);
    read_str(path, b->manufacturer, sizeof(b->manufacturer));

    // technology
    snprintf(path, sizeof(path), SYS_PATH "/%s/technology", entry->d_name);
    read_str(path, b->technology, sizeof(b->technology));

    // voltage_now
    snprintf(path, sizeof(path), SYS_PATH "/%s/voltage_now", entry->d_name);
    read_long(path, &b->voltage_now);

    // current_now
    snprintf(path, sizeof(path), SYS_PATH "/%s/current_now", entry->d_name);
    read_long(path, &b->current_now);

    // Try to find energy_full or charge_full
    const char *charge_names[] = {"energy", "charge"};
    for (int i = 0; i < 2; ++i) {
      snprintf(path, sizeof(path), SYS_PATH "/%s/%s_full", entry->d_name,
               charge_names[i]);
      if (access(path, F_OK) == 0) {
        read_long(path, &b->full);
        snprintf(path, sizeof(path), SYS_PATH "/%s/%s_full_design",
                 entry->d_name, charge_names[i]);
        read_long(path, &b->full_design);
        break;
      }
    }

    count++;
  }

  closedir(d);
  return count;
}

static char *battery_icon(const int capacity, const int charging) {
  const char icons[11][8] = {"󰂎", "󰁺", "󰁻", "󰁼", "󰁽", "󰁾",
                             "󰁿", "󰂀", "󰂁", "󰂂", "󰁹"};
  const char icons_charge[11][8] = {"󰢟 ", "󰢜 ", "󰂆 ", "󰂇 ",
                                    "󰂈 ", "󰢝 ", "󰂉 ", "󰢞 ",
                                    "󰂊 ", "󰂋 ", "󰂅 "};

  int cap = 0;

  if (capacity >= 100) {
    cap = 10;
  } else if (capacity >= 90) {
    cap = 9;
  } else if (capacity >= 80) {
    cap = 8;
  } else if (capacity >= 70) {
    cap = 7;
  } else if (capacity >= 60) {
    cap = 6;
  } else if (capacity >= 50) {
    cap = 5;
  } else if (capacity >= 40) {
    cap = 4;
  } else if (capacity >= 30) {
    cap = 3;
  } else if (capacity >= 20) {
    cap = 2;
  } else if (capacity >= 10) {
    cap = 1;
  } else if (capacity >= 0) {
    cap = 0;
  }

  char *icon = malloc(8);
  if (charging == 0) {
    sprintf(icon, "%s", icons[cap]);
  } else if (charging == 1) {
    sprintf(icon, "%s", icons_charge[cap]);
  }

  return icon;
}

void format_batt(void) {
  struct battery bat[MAX_BATTERIES];
  int n = load_batteries(bat, MAX_BATTERIES);
  if (n < 0) {
    fprintf(stderr, "Failed to load batteries\n");
    exit(1);
  }
  char *json = malloc(512 * n);

  int main_capacity = 0;
  int charging = 0;

  if (strcmp(bat[0].status, "Charging") == 0) {
    charging = 1;
  }

  for (int i = 0; i < n; i++) {
    struct battery *b = &bat[i];
    main_capacity += b->capacity;
  }

  main_capacity /= n;

  sprintf(json, "{\"text\":\"%d%% %s\",\"tooltip\":\"", main_capacity,
          battery_icon(main_capacity, charging));

  for (int i = 0; i < n; i++) {
    struct battery *b = &bat[i];

    float voltage = b->voltage_now / 1e6;
    float current = b->current_now / 1e6;
    float power = voltage * current;

    if (charging == 0 && power != 0.0) {
      power *= -1;
    }

    sprintf(json,
            "%s<b>%s:</"
            "b>\\t%s\\nManufacturer:\\t%s\\nTechnology:\\t%s\\nStatus:\\t%"
            "s\\nCharge:\\t%d\\nHealth:\\t%lu%%\\nVoltage:\\t%.1f "
            "V\\nCurrent:\\t%.1f A\\nCharging Power:\\t%.1f W",
            json, b->name, b->model_name, b->manufacturer, b->technology,
            b->status, b->capacity, ((b->full * 100) / b->full_design), voltage,
            current, power);

    if (i != (n - 1)) {
      sprintf(json, "%s\\n\\n", json);
    }
  }

  sprintf(json, "%s\"}", json);
  printf("%s\n", json);
}
