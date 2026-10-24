#ifndef WAYCENTRAL_H_
#define WAYCENTRAL_H_

/*
 * libwaycentral - the Waybar modules of waycentral as a C library.
 *
 * Link with -lwaycentral (or `pkg-config --libs waycentral`).
 *
 * Two kinds of functions are provided:
 *   - wc_*_get / wc_*_list fill plain structs with raw values, for programs
 *     that want to format the data themselves.
 *   - wc_*_json return a heap allocated Waybar JSON line (without the
 *     trailing newline), exactly what the waycentral binary prints.
 *     Release it with free(). NULL is returned on allocation failure.
 */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define WC_API __attribute__((visibility("default")))
#else
#define WC_API
#endif

/* ------------------------------------------------------------------------ */
/* General                                                                  */
/* ------------------------------------------------------------------------ */

/* Version of the library, e.g. "1.2.0". */
WC_API const char *wc_version(void);

/* ------------------------------------------------------------------------ */
/* Clock                                                                    */
/* ------------------------------------------------------------------------ */

enum wc_clock_format {
  WC_CLOCK_24H = 0,      /* 23:10:18    */
  WC_CLOCK_12H_AMPM = 1, /* 11:10:18 PM */
  WC_CLOCK_12H = 2,      /* 11:10:18    */
};

/* Seconds since boot, or -1 on error. */
WC_API long wc_uptime(void);

/* Days since the birth of the root filesystem (system install), or -1 if the
 * filesystem does not record a birth time. */
WC_API long wc_system_age_days(void);

/* {"text":"<time>","tooltip":"Uptime:\t...\nBirth:\t..."} */
WC_API char *wc_clock_json(enum wc_clock_format format);

/* ------------------------------------------------------------------------ */
/* Date / Calendar                                                          */
/* ------------------------------------------------------------------------ */

enum wc_date_format {
  WC_DATE_YMD = 0, /* yy/mm/dd */
  WC_DATE_DMY = 1, /* dd/mm/yy */
  WC_DATE_MDY = 2, /* mm/dd/yy */
};

enum wc_week_start {
  WC_WEEK_SUNDAY = 0,
  WC_WEEK_MONDAY = 1,
};

struct wc_calendar {
  char header[32];     /* "November 2025"                    */
  char weekdays[7][3]; /* "Mo", "Tu", ... in display order   */
  int rows;            /* number of week rows used (4-6)     */
  int days[6][7];      /* day of month, 0 for an empty cell  */
};

/* Fills a month grid. month is 1-12. Returns 0, or -1 on invalid input. */
WC_API int wc_calendar_make(struct wc_calendar *out, int month, int year,
                            enum wc_week_start start);

/* {"text":"<date>","tooltip":"<pango calendar of the current month>"} */
WC_API char *wc_date_json(enum wc_date_format format,
                          enum wc_week_start start);

/* ------------------------------------------------------------------------ */
/* Battery                                                                  */
/* ------------------------------------------------------------------------ */

#define WC_MAX_BATTERIES 8

/* Values as reported by /sys/class/power_supply/BAT* (µV, µA, µWh/µAh). */
struct wc_battery {
  char name[64];
  int capacity; /* percent */
  char status[32];
  long full;
  long full_design;
  long voltage_now;
  long current_now;
  char model_name[128];
  char manufacturer[128];
  char technology[64];
};

/* Fills up to max batteries. Returns how many were found, or -1 on error. */
WC_API int wc_battery_list(struct wc_battery *out, int max);

/* {"text":"<capacity>% <icon>","tooltip":"..."}
 * Without a battery: {"text":"","tooltip":"No Battery","class":"none"} */
WC_API char *wc_battery_json(void);

/* ------------------------------------------------------------------------ */
/* System Monitoring                                                        */
/* ------------------------------------------------------------------------ */

struct wc_cpu_usage {
  int count;  /* number of entries in usage                   */
  int *usage; /* percent; [0] is the total, [1..] every core  */
};

/* Samples /proc/stat twice, interval_ms apart (blocks for that long).
 * Release with wc_cpu_usage_free(). Returns NULL on error. */
WC_API struct wc_cpu_usage *wc_cpu_usage_get(int interval_ms);
WC_API void wc_cpu_usage_free(struct wc_cpu_usage *usage);

/* All values in kiB. */
struct wc_meminfo {
  unsigned long long mem_total;
  unsigned long long mem_free;
  unsigned long long mem_available;
  unsigned long long mem_used;
  unsigned long long buffers;
  unsigned long long cached;
  unsigned long long shared;
  unsigned long long swap_total;
  unsigned long long swap_free;
  unsigned long long swap_used;
};

/* Reads /proc/meminfo. Returns 0, or -1 on error. */
WC_API int wc_meminfo_get(struct wc_meminfo *out);

/* Totals in bytes since boot, speeds in bytes per second. Loopback and
 * virtual interfaces (docker, veth, br-, vmnet, tun, tap) are ignored. */
struct wc_netstat {
  unsigned long long upload;
  unsigned long long download;
  unsigned long long up_speed;
  unsigned long long down_speed;
};

/* Samples /proc/net/dev twice, interval_ms apart (blocks for that long).
 * Returns 0, or -1 on error. */
WC_API int wc_netstat_get(struct wc_netstat *out, int interval_ms);

/* The *_json functions block for interval_ms while sampling. */
WC_API char *wc_cpu_json(int interval_ms);
WC_API char *wc_mem_json(void);
WC_API char *wc_net_json(int interval_ms);

/* ------------------------------------------------------------------------ */
/* Lock Keys                                                                */
/* ------------------------------------------------------------------------ */

enum wc_lock_key {
  WC_KEY_CAPS = 0,
  WC_KEY_NUM = 1,
  WC_KEY_SCROLL = 2,
};

struct wc_lock_keys {
  int caps;
  int num;
  int scroll;
};

/* Reads the LED state of the first keyboard in /dev/input (needs read access,
 * usually the "input" group). Returns 0, or -1 if no keyboard is readable. */
WC_API int wc_lock_keys_get(struct wc_lock_keys *out);

/* {"class":"on|off","tooltip":"Capslock On|Off"}
 * Without a readable keyboard the class is "unknown". */
WC_API char *wc_lock_key_json(enum wc_lock_key key);

/* ------------------------------------------------------------------------ */
/* Hotspot (create_ap / linux-wifi-hotspot)                                 */
/* ------------------------------------------------------------------------ */

struct wc_hotspot {
  int active; /* 1 if a create_ap process is running */
  int hidden;
  char ssid[256];
  char passphrase[256];
  char freq[16];
  char wifi_iface[128];
  char internet_iface[128];
};

/* Fills active, and the rest from /etc/create_ap.conf when active.
 * Returns 0, or -1 on error. */
WC_API int wc_hotspot_get(struct wc_hotspot *out);

/* {"class":"on|off","alt":"on|off","tooltip":"..."} */
WC_API char *wc_hotspot_json(void);

#ifdef __cplusplus
}
#endif

#endif // !WAYCENTRAL_H_
