#define _GNU_SOURCE
#include "../../waycentral.h"
#include "../util.h"
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DRM_PATH "/sys/class/drm"
#define PCI_IDS "/usr/share/hwdata/pci.ids"
#define SAMPLES 5

static void init_gpu(struct wc_gpu *g) {
  memset(g, 0, sizeof(*g));
  g->usage = -1;
  g->vram_used = -1;
  g->vram_total = -1;
  g->temp = -1;
  g->power = -1;
  g->core_clock = -1;
  g->mem_clock = -1;
  g->fan_rpm = -1;
  g->fan_percent = -1;
}

static int read_ll(const char *path, long long *out) {
  long v;
  if (wc_read_long(path, &v) != 0)
    return -1;
  *out = v;
  return 0;
}

/* Name from hwdata's pci.ids: "Ellesmere [Radeon RX 470/480/570/...]" */
static void pci_name(unsigned vendor, unsigned device, char *out,
                     size_t size) {
  FILE *f = fopen(PCI_IDS, "r");
  if (!f)
    return;

  char line[512], vendor_name[256] = "";
  int in_vendor = 0;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    unsigned id;
    if (line[0] != '\t' && line[0] != '#' &&
        sscanf(line, "%4x", &id) == 1) {
      if (in_vendor)
        break; // past our vendor without a match
      in_vendor = id == vendor;
      if (in_vendor)
        snprintf(vendor_name, sizeof(vendor_name), "%s", line + 6);
    } else if (in_vendor && line[0] == '\t' && line[1] != '\t' &&
               sscanf(line + 1, "%4x", &id) == 1 && id == device) {
      snprintf(out, size, "%s", line + 7);
      break;
    }
  }
  fclose(f);

  if (!out[0] && vendor_name[0])
    snprintf(out, size, "%s GPU", vendor_name);
}

/* Clock marked with '*' in pp_dpm_sclk / pp_dpm_mclk, in MHz */
static int dpm_clock(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  char line[128];
  int mhz = -1;
  while (fgets(line, sizeof(line), f)) {
    int level, value;
    if (strchr(line, '*') && sscanf(line, "%d: %dMhz", &level, &value) == 2)
      mhz = value;
  }
  fclose(f);
  return mhz;
}

/* Temperature, power, clocks and fan from the card's hwmon */
static void read_hwmon(const char *device, struct wc_gpu *g) {
  char dir[512];
  snprintf(dir, sizeof(dir), "%s/hwmon", device);
  DIR *d = opendir(dir);
  if (!d)
    return;

  struct dirent *ent;
  char base[800] = "";
  while ((ent = readdir(d))) {
    if (strncmp(ent->d_name, "hwmon", 5) == 0) {
      snprintf(base, sizeof(base), "%s/%s", dir, ent->d_name);
      break;
    }
  }
  closedir(d);
  if (!base[0])
    return;

  char path[900];
  long long v;

  snprintf(path, sizeof(path), "%s/temp1_input", base);
  if (read_ll(path, &v) == 0)
    g->temp = (int)(v / 1000);

  // Newer kernels have power1_input, older only power1_average (µW)
  snprintf(path, sizeof(path), "%s/power1_input", base);
  if (read_ll(path, &v) != 0) {
    snprintf(path, sizeof(path), "%s/power1_average", base);
    if (read_ll(path, &v) != 0)
      v = -1;
  }
  if (v >= 0)
    g->power = (double)v / 1e6;

  snprintf(path, sizeof(path), "%s/freq1_input", base);
  if (read_ll(path, &v) == 0)
    g->core_clock = (int)(v / 1000000);
  snprintf(path, sizeof(path), "%s/freq2_input", base);
  if (read_ll(path, &v) == 0)
    g->mem_clock = (int)(v / 1000000);

  snprintf(path, sizeof(path), "%s/fan1_input", base);
  if (read_ll(path, &v) == 0) {
    g->fan_rpm = (int)v;
    long long max;
    snprintf(path, sizeof(path), "%s/fan1_max", base);
    if (read_ll(path, &max) == 0 && max > 0)
      g->fan_percent = (int)(v * 100 / max);
  }
  if (g->fan_percent < 0) {
    long long pwm, max = 255;
    snprintf(path, sizeof(path), "%s/pwm1", base);
    if (read_ll(path, &pwm) == 0) {
      snprintf(path, sizeof(path), "%s/pwm1_max", base);
      read_ll(path, &max);
      if (max > 0)
        g->fan_percent = (int)(pwm * 100 / max);
    }
  }
}

/* Intel only reports the clock without perf counters */
static void read_intel(const char *card, struct wc_gpu *g) {
  char path[512];
  long long v;
  snprintf(path, sizeof(path), "%s/gt_act_freq_mhz", card);
  if (read_ll(path, &v) != 0) {
    snprintf(path, sizeof(path), "%s/device/tile0/gt0/freq0/act_freq", card);
    if (read_ll(path, &v) != 0)
      return;
  }
  g->core_clock = (int)v;
}

/* ---- NVIDIA: NVML, loaded at runtime so it is not a dependency ---- */

typedef void *nvml_dev;
typedef struct {
  unsigned int gpu, memory;
} nvml_util;
typedef struct {
  unsigned long long total, free, used;
} nvml_mem;

static struct {
  int tried, ok;
  int (*init)(void);
  int (*by_bus)(const char *, nvml_dev *);
  int (*name)(nvml_dev, char *, unsigned int);
  int (*util)(nvml_dev, nvml_util *);
  int (*mem)(nvml_dev, nvml_mem *);
  int (*temp)(nvml_dev, int, unsigned int *);
  int (*power)(nvml_dev, unsigned int *);
  int (*clock)(nvml_dev, int, unsigned int *);
  int (*fan)(nvml_dev, unsigned int *);
} nvml;

static int nvml_load(void) {
  if (nvml.tried)
    return nvml.ok;
  nvml.tried = 1;

  void *lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib)
    return 0;

  // POSIX way of turning dlsym's void * into a function pointer
#define LOAD(field, symbol) (*(void **)&nvml.field = dlsym(lib, symbol))
  LOAD(init, "nvmlInit_v2");
  LOAD(by_bus, "nvmlDeviceGetHandleByPciBusId_v2");
  LOAD(name, "nvmlDeviceGetName");
  LOAD(util, "nvmlDeviceGetUtilizationRates");
  LOAD(mem, "nvmlDeviceGetMemoryInfo");
  LOAD(temp, "nvmlDeviceGetTemperature");
  LOAD(power, "nvmlDeviceGetPowerUsage");
  LOAD(clock, "nvmlDeviceGetClockInfo");
  LOAD(fan, "nvmlDeviceGetFanSpeed");
#undef LOAD

  nvml.ok = nvml.init && nvml.by_bus && nvml.init() == 0;
  return nvml.ok;
}

static void read_nvidia(const char *bus_id, struct wc_gpu *g) {
  nvml_dev dev;
  if (!nvml_load() || nvml.by_bus(bus_id, &dev) != 0)
    return;

  char name[96];
  nvml_util u;
  nvml_mem m;
  unsigned int v;

  if (nvml.name && nvml.name(dev, name, sizeof(name)) == 0)
    snprintf(g->name, sizeof(g->name), "%s", name);
  if (nvml.util && nvml.util(dev, &u) == 0)
    g->usage = (int)u.gpu;
  if (nvml.mem && nvml.mem(dev, &m) == 0) {
    g->vram_used = (long long)m.used;
    g->vram_total = (long long)m.total;
  }
  if (nvml.temp && nvml.temp(dev, 0, &v) == 0) // NVML_TEMPERATURE_GPU
    g->temp = (int)v;
  if (nvml.power && nvml.power(dev, &v) == 0) // mW
    g->power = v / 1000.0;
  if (nvml.clock && nvml.clock(dev, 0, &v) == 0) // NVML_CLOCK_GRAPHICS
    g->core_clock = (int)v;
  if (nvml.clock && nvml.clock(dev, 2, &v) == 0) // NVML_CLOCK_MEM
    g->mem_clock = (int)v;
  if (nvml.fan && nvml.fan(dev, &v) == 0)
    g->fan_percent = (int)v;
}

/* ---- enumeration ---- */

static int is_card(const char *name) {
  if (strncmp(name, "card", 4) != 0 || !name[4])
    return 0;
  for (const char *c = name + 4; *c; c++)
    if (*c < '0' || *c > '9')
      return 0; // connectors like card1-DP-1
  return 1;
}

static int cmp_card(const void *a, const void *b) {
  return atoi((const char *)a + 4) - atoi((const char *)b + 4);
}

static void read_gpu(const char *card_name, struct wc_gpu *g) {
  char card[300], device[320], path[400], link[512];
  snprintf(card, sizeof(card), DRM_PATH "/%s", card_name);
  snprintf(device, sizeof(device), "%s/device", card);

  init_gpu(g);
  snprintf(g->card, sizeof(g->card), "%s", card_name);

  snprintf(path, sizeof(path), "%s/driver", device);
  ssize_t n = readlink(path, link, sizeof(link) - 1);
  if (n > 0) {
    link[n] = '\0';
    const char *slash = strrchr(link, '/');
    snprintf(g->driver, sizeof(g->driver), "%s", slash ? slash + 1 : link);
  }

  long long vendor = 0, dev_id = 0;
  char hex[32];
  snprintf(path, sizeof(path), "%s/vendor", device);
  if (wc_read_str(path, hex, sizeof(hex)) == 0)
    vendor = strtoll(hex, NULL, 16);
  snprintf(path, sizeof(path), "%s/device", device);
  if (wc_read_str(path, hex, sizeof(hex)) == 0)
    dev_id = strtoll(hex, NULL, 16);

  snprintf(path, sizeof(path), "%s/product_name", device);
  if (wc_read_str(path, g->name, sizeof(g->name)) != 0 || !g->name[0])
    pci_name((unsigned)vendor, (unsigned)dev_id, g->name, sizeof(g->name));

  // AMD (amdgpu, radeon)
  long long v;
  snprintf(path, sizeof(path), "%s/gpu_busy_percent", device);
  if (read_ll(path, &v) == 0)
    g->usage = (int)v;
  snprintf(path, sizeof(path), "%s/mem_info_vram_used", device);
  if (read_ll(path, &v) == 0)
    g->vram_used = v;
  snprintf(path, sizeof(path), "%s/mem_info_vram_total", device);
  if (read_ll(path, &v) == 0)
    g->vram_total = v;

  read_hwmon(device, g);

  if (g->core_clock < 0) {
    snprintf(path, sizeof(path), "%s/pp_dpm_sclk", device);
    g->core_clock = dpm_clock(path);
  }
  if (g->mem_clock < 0) {
    snprintf(path, sizeof(path), "%s/pp_dpm_mclk", device);
    g->mem_clock = dpm_clock(path);
  }

  if (strcmp(g->driver, "i915") == 0 || strcmp(g->driver, "xe") == 0)
    read_intel(card, g);

  if (strcmp(g->driver, "nvidia") == 0) {
    // PCI bus id is the last component of the device path
    if (realpath(device, link)) {
      const char *slash = strrchr(link, '/');
      read_nvidia(slash ? slash + 1 : link, g);
    }
  }
}

int wc_gpu_list(struct wc_gpu *out, int max) {
  DIR *d = opendir(DRM_PATH);
  if (!d)
    return -1;

  char names[WC_MAX_GPUS][32];
  int count = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) && count < WC_MAX_GPUS)
    if (is_card(ent->d_name))
      snprintf(names[count++], sizeof(names[0]), "%s", ent->d_name);
  closedir(d);

  qsort(names, (size_t)count, sizeof(names[0]), cmp_card);

  if (count > max)
    count = max;
  for (int i = 0; i < count; i++)
    read_gpu(names[i], &out[i]);
  return count;
}

static void append_gpu(struct wc_buf *json, int index,
                       const struct wc_gpu *g) {
  wc_buf_appendf(json, "<b>GPU%d:</b>\\t", index);
  wc_buf_append_text(json, g->name[0] ? g->name : g->card);
  wc_buf_appendf(json, "\\nDriver:\\t");
  wc_buf_append_text(json, g->driver[0] ? g->driver : "unknown");

  if (g->usage >= 0)
    wc_buf_appendf(json, "\\nUsage:\\t%d%%", g->usage);
  if (g->vram_total > 0 && g->vram_used >= 0) {
    char used[32], total[32];
    wc_format_size(used, sizeof(used), 0, (long double)g->vram_used, 1);
    wc_format_size(total, sizeof(total), 0, (long double)g->vram_total, 1);
    wc_buf_appendf(json, "\\nVRAM:\\t%s / %s (%.1f%%)", used, total,
                   (double)g->vram_used * 100.0 / (double)g->vram_total);
  }
  if (g->temp >= 0)
    wc_buf_appendf(json, "\\nTemp:\\t%d °C", g->temp);
  if (g->power >= 0)
    wc_buf_appendf(json, "\\nPower:\\t%.1f W", g->power);
  if (g->core_clock >= 0)
    wc_buf_appendf(json, "\\nCore:\\t%d MHz", g->core_clock);
  if (g->mem_clock >= 0)
    wc_buf_appendf(json, "\\nMem:\\t%d MHz", g->mem_clock);
  if (g->fan_rpm >= 0 && g->fan_percent >= 0)
    wc_buf_appendf(json, "\\nFan:\\t%d RPM (%d%%)", g->fan_rpm,
                   g->fan_percent);
  else if (g->fan_rpm >= 0)
    wc_buf_appendf(json, "\\nFan:\\t%d RPM", g->fan_rpm);
  else if (g->fan_percent >= 0)
    wc_buf_appendf(json, "\\nFan:\\t%d%%", g->fan_percent);
}

char *wc_gpu_json(int index, int interval_ms) {
  struct wc_gpu gpus[WC_MAX_GPUS];
  int n = wc_gpu_list(gpus, WC_MAX_GPUS);
  struct wc_buf json = {0};

  if (n <= 0) {
    wc_buf_appendf(&json, "{\"text\":\"\",\"class\":\"none\","
                          "\"tooltip\":\"No GPU Found\"}");
    return wc_buf_finish(&json);
  }
  if (index < 0 || index >= n)
    index = 0;

  /* The busy counter is instantaneous, average it over interval_ms like the
   * CPU usage */
  if (interval_ms > 0 && gpus[index].usage >= 0) {
    long sum = gpus[index].usage;
    int samples = 1;
    for (int s = 1; s < SAMPLES; s++) {
      wc_sleep_ms(interval_ms / SAMPLES);
      struct wc_gpu again[WC_MAX_GPUS];
      if (wc_gpu_list(again, WC_MAX_GPUS) != n || again[index].usage < 0)
        continue;
      sum += again[index].usage;
      samples++;
      if (s == SAMPLES - 1) // latest values for the rest of the tooltip
        memcpy(gpus, again, sizeof(gpus[0]) * (size_t)n);
    }
    gpus[index].usage = (int)(sum / samples);
  }

  if (gpus[index].usage >= 0)
    wc_buf_appendf(&json, "{\"text\":\"%d%%\",\"percentage\":%d,",
                   gpus[index].usage, gpus[index].usage);
  else
    wc_buf_appendf(&json, "{\"text\":\"N/A\",");
  wc_buf_appendf(&json, "\"tooltip\":\"");

  for (int i = 0; i < n; i++) {
    if (i)
      wc_buf_appendf(&json, "\\n\\n");
    append_gpu(&json, i, &gpus[i]);
  }

  wc_buf_appendf(&json, "\"}");
  return wc_buf_finish(&json);
}
